#pragma once

// NUMA memory policies (the same ids as in the MultiQueue experiments, benchmarks/util/memory_policy.hpp) and buffers
// with a selectable page size.

#include "mb/fail.hpp"
#include "mb/topology.hpp"

#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mb::memory_policy {

enum Policy : int {
    FirstTouch = 0,
    InterleaveUsed = 1,
    InterleaveAll = 2,
};

inline constexpr int max_id = InterleaveAll;
inline constexpr int default_id = InterleaveUsed;

inline char const* name(int id) noexcept {
    switch (id) {
        case FirstTouch:
            return "First touch";
        case InterleaveUsed:
            return "Interleave over used nodes";
        case InterleaveAll:
            return "Interleave over all nodes";
        default:
            return "Unknown";
    }
}

namespace detail {

inline std::vector<std::size_t> parse_list(std::string const& list) {
    std::vector<std::size_t> ids;
    std::size_t pos = 0;
    while (pos < list.size()) {
        auto end = list.find(',', pos);
        if (end == std::string::npos) {
            end = list.size();
        }
        auto range = list.substr(pos, end - pos);
        if (!range.empty()) {
            auto dash = range.find('-');
            auto first = std::strtoul(range.c_str(), nullptr, 10);
            auto last = dash == std::string::npos ? first : std::strtoul(range.c_str() + dash + 1, nullptr, 10);
            for (auto i = first; i <= last; ++i) {
                ids.push_back(i);
            }
        }
        pos = end + 1;
    }
    return ids;
}

}  // namespace detail

inline std::vector<std::size_t> nodes_with_memory() {
    std::string list;
    std::ifstream("/sys/devices/system/node/has_memory") >> list;
    auto nodes = detail::parse_list(list);
    if (nodes.empty()) {
        nodes.push_back(0);
    }
    return nodes;
}

// The nodes to interleave over, given the CPUs the threads are pinned to; empty for first touch.
inline std::vector<std::size_t> nodes(int policy, std::vector<std::size_t> const& cpus) {
    if (policy < 0 || policy > max_id) {
        throw std::runtime_error("Invalid memory policy " + std::to_string(policy));
    }
    if (policy == FirstTouch) {
        return {};
    }
    auto with_memory = nodes_with_memory();
    if (policy == InterleaveAll || cpus.empty()) {
        return with_memory;
    }
    std::set<std::size_t> used;
    for (auto cpu : cpus) {
        used.insert(affinity::numa_node(cpu));
    }
    std::vector<std::size_t> result;
    std::set_intersection(used.begin(), used.end(), with_memory.begin(), with_memory.end(),
                          std::back_inserter(result));
    return result.empty() ? with_memory : result;
}

// Sets the memory policy of the calling thread; threads created afterwards inherit it.
inline void apply(std::vector<std::size_t> const& nodes) {
    if (nodes.empty()) {
        return;
    }
    constexpr std::size_t bits = sizeof(unsigned long) * CHAR_BIT;
    std::array<unsigned long, 16> mask{};
    for (auto node : nodes) {
        if (node >= mask.size() * bits) {
            fail("NUMA node out of range", EINVAL);
        }
        mask[node / bits] |= 1UL << (node % bits);
    }
    if (syscall(SYS_set_mempolicy, MPOL_INTERLEAVE, mask.data(), mask.size() * bits) != 0) {
        fail("Failed to set memory policy", errno);
    }
}

}  // namespace mb::memory_policy

namespace mb {

enum class Pages {
    Default,      // whatever the system does (transparent huge pages per its setting)
    Small,        // base pages only (madvise MADV_NOHUGEPAGE)
    Transparent,  // transparent huge pages (madvise MADV_HUGEPAGE)
    HugeTlb,      // explicit huge pages of the default size (MAP_HUGETLB); needs reserved huge pages
};

inline Pages parse_pages(std::string const& s) {
    if (s == "default") {
        return Pages::Default;
    }
    if (s == "small" || s == "4k") {
        return Pages::Small;
    }
    if (s == "thp") {
        return Pages::Transparent;
    }
    if (s == "hugetlb") {
        return Pages::HugeTlb;
    }
    throw std::runtime_error("Invalid page mode '" + s + "' (default, small, thp, hugetlb)");
}

inline char const* name(Pages pages) noexcept {
    switch (pages) {
        case Pages::Default:
            return "default";
        case Pages::Small:
            return "small";
        case Pages::Transparent:
            return "thp";
        case Pages::HugeTlb:
            return "hugetlb";
    }
    return "unknown";
}

inline std::size_t base_page_size() {
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
}

// The size of a transparent huge page (2 MiB with 4 KiB base pages).
inline std::size_t huge_page_size() {
    std::size_t size = 0;
    if (!(std::ifstream("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size") >> size) || size == 0) {
        size = std::size_t{2} << 20;
    }
    return size;
}

// Available memory according to /proc/meminfo, 0 if unknown.
inline std::size_t available_memory() {
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream in(line.substr(13));
            std::size_t kib = 0;
            in >> kib;
            return kib * 1024;
        }
    }
    return 0;
}

// Anonymous memory aligned to the huge page size. Pages are not touched; the thread that first writes a page
// determines its NUMA node under first touch.
class Buffer {
    void* base_ = nullptr;
    std::size_t mapped_ = 0;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;

   public:
    Buffer() = default;

    Buffer(std::size_t size, Pages pages) : size_(size) {
        auto const huge = huge_page_size();
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (pages == Pages::HugeTlb) {
            // Without MAP_NORESERVE, mmap fails if not enough huge pages are reserved, instead of a SIGBUS on access.
            // Huge pages of the default size, which is assumed to be the transparent huge page size.
            flags |= MAP_HUGETLB;
            mapped_ = (size + huge - 1) / huge * huge;
        } else {
            // Over-allocate to align the start to a huge page.
            flags |= MAP_NORESERVE;
            mapped_ = (size + huge - 1) / huge * huge + huge;
        }
        base_ = mmap(nullptr, mapped_, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (base_ == MAP_FAILED) {
            fail("mmap", errno);
        }
        auto addr = reinterpret_cast<std::uintptr_t>(base_);
        data_ = static_cast<std::byte*>(base_) + ((huge - addr % huge) % huge);
        int advice = pages == Pages::Small ? MADV_NOHUGEPAGE : pages == Pages::Transparent ? MADV_HUGEPAGE : -1;
        if (advice != -1 && madvise(base_, mapped_, advice) != 0) {
            fail("madvise", errno);
        }
    }

    Buffer(Buffer&& other) noexcept
        : base_(std::exchange(other.base_, nullptr)),
          mapped_(std::exchange(other.mapped_, 0)),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            release();
            base_ = std::exchange(other.base_, nullptr);
            mapped_ = std::exchange(other.mapped_, 0);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    Buffer(Buffer const&) = delete;
    Buffer& operator=(Buffer const&) = delete;

    ~Buffer() {
        release();
    }

    void release() noexcept {
        if (base_ != nullptr) {
            munmap(base_, mapped_);
            base_ = nullptr;
        }
    }

    [[nodiscard]] std::byte* data() const noexcept {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    template <typename T>
    [[nodiscard]] T* as() const noexcept {
        return reinterpret_cast<T*>(data_);
    }
};

}  // namespace mb
