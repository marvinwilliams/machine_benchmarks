#pragma once

// CPU topology and thread pinning orders. The affinity policies and their ids are the same as in the MultiQueue
// experiments (benchmarks/util/thread_coordination.hpp), so that -a 7 (CoresFirst) pins threads in the same order.

#include "mb/fail.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace mb::affinity {

enum Policy : int {
    None = 0,
    ThreadId = 1,
    Same = 2,
    CloseCaches = 3,
    FarCaches = 4,
    CloseL3FarL1 = 5,
    FarL1CloseL3 = 6,
    CoresFirst = 7,
};

inline constexpr int max_id = CoresFirst;
inline constexpr int default_id = CoresFirst;

inline char const* name(int id) noexcept {
    switch (id) {
        case None:
            return "None";
        case ThreadId:
            return "Thread Id";
        case Same:
            return "Same";
        case CloseCaches:
            return "Close caches";
        case FarCaches:
            return "Far caches";
        case CloseL3FarL1:
            return "Close L3 Far L1";
        case FarL1CloseL3:
            return "Far L1 Close L3";
        case CoresFirst:
            return "Cores first (L3, NUMA node, then SMT)";
        default:
            return "Unknown";
    }
}

inline std::vector<std::size_t> available_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    std::vector<std::size_t> cpus;
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        cpus.resize(std::thread::hardware_concurrency());
        std::iota(cpus.begin(), cpus.end(), 0);
        return cpus;
    }
    for (std::size_t i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &set)) {
            cpus.push_back(i);
        }
    }
    return cpus;
}

namespace detail {

struct CpuInfo {
    std::size_t cpu;
    // [0]: rank of the CPU within its core (SMT sibling)
    // [1]: rank of the core within its L3
    // [2]: rank of the L3 within its NUMA node
    // [3]: rank of the NUMA node within its package
    // [4]: id of the package
    std::array<std::size_t, 5> level;
};

inline std::size_t numa_node(std::size_t cpu) {
    std::error_code ec;
    auto dir = std::filesystem::directory_iterator("/sys/devices/system/cpu/cpu" + std::to_string(cpu), ec);
    for (; !ec && dir != std::filesystem::directory_iterator{}; dir.increment(ec)) {
        auto name = dir->path().filename().string();
        if (name.size() > 4 && name.compare(0, 4, "node") == 0 &&
            std::all_of(name.begin() + 4, name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return std::strtoul(name.c_str() + 4, nullptr, 10);
        }
    }
    return 0;
}

inline std::size_t first_cpu_of(std::string const& list_file, std::size_t fallback) {
    std::string list;
    std::ifstream(list_file) >> list;
    auto end = list.find_first_not_of("0123456789");
    return end == 0 || list.empty() ? fallback : std::strtoul(list.c_str(), nullptr, 10);
}

// The first CPU sharing the L3 cache with cpu, 0 without an L3.
inline std::size_t l3_group(std::size_t cpu) {
    auto base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/";
    for (int i = 0;; ++i) {
        auto index = base + "index" + std::to_string(i);
        int level = 0;
        if (!(std::ifstream(index + "/level") >> level)) {
            return 0;
        }
        if (level == 3) {
            return first_cpu_of(index + "/shared_cpu_list", 0);
        }
    }
}

inline std::vector<CpuInfo> read_topology(std::vector<std::size_t> const& cpus) {
    std::vector<CpuInfo> hierarchy(cpus.size());
    for (std::size_t i = 0; i < cpus.size(); ++i) {
        auto cpu = cpus[i];
        auto topology = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        auto& h = hierarchy[i].level;
        hierarchy[i].cpu = cpu;
        h[0] = cpu;
        h[1] = first_cpu_of(topology + "thread_siblings_list", cpu);
        h[2] = l3_group(cpu);
        h[3] = numa_node(cpu);
        h[4] = first_cpu_of(topology + "physical_package_id", 0);
    }
    return hierarchy;
}

inline void rank_levels(std::vector<CpuInfo>& hierarchy) {
    for (std::size_t l = 0; l + 1 < std::tuple_size_v<decltype(CpuInfo::level)>; ++l) {
        std::vector<std::vector<std::size_t>> lookup;
        for (auto& info : hierarchy) {
            auto& h = info.level;
            if (h[l + 1] >= lookup.size()) {
                lookup.resize(h[l + 1] + 1);
            }
            auto& group = lookup[h[l + 1]];
            auto it = std::find(group.begin(), group.end(), h[l]);
            if (it == group.end()) {
                group.push_back(h[l]);
                it = group.end() - 1;
            }
            h[l] = static_cast<std::size_t>(std::distance(group.begin(), it));
        }
    }
}

inline std::vector<std::size_t> order_by(std::vector<CpuInfo> hierarchy, std::array<std::size_t, 5> levels) {
    rank_levels(hierarchy);
    auto key = [&levels](CpuInfo const& c) {
        return std::tie(c.level[levels[0]], c.level[levels[1]], c.level[levels[2]], c.level[levels[3]],
                        c.level[levels[4]]);
    };
    std::sort(hierarchy.begin(), hierarchy.end(),
              [&key](CpuInfo const& a, CpuInfo const& b) { return key(a) < key(b); });
    std::vector<std::size_t> order(hierarchy.size());
    std::transform(hierarchy.begin(), hierarchy.end(), order.begin(), [](CpuInfo const& c) { return c.cpu; });
    return order;
}

}  // namespace detail

using detail::first_cpu_of;
using detail::l3_group;
using detail::numa_node;

inline bool has_l3(std::size_t cpu) {
    auto base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/";
    for (int i = 0;; ++i) {
        int level = 0;
        if (!(std::ifstream(base + "index" + std::to_string(i) + "/level") >> level)) {
            return false;
        }
        if (level == 3) {
            return true;
        }
    }
}

inline std::size_t core_of(std::size_t cpu) {
    return first_cpu_of("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list", cpu);
}

inline std::size_t package_of(std::size_t cpu) {
    return first_cpu_of("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/physical_package_id", 0);
}

// How close two CPUs are, from closest to farthest.
enum class Relation { Smt, L3, Node, Socket, Remote };

inline constexpr std::array<Relation, 5> relations = {Relation::Smt, Relation::L3, Relation::Node, Relation::Socket,
                                                      Relation::Remote};

inline char const* name(Relation r) noexcept {
    switch (r) {
        case Relation::Smt:
            return "smt";  // the same core
        case Relation::L3:
            return "l3";  // another core of the same NUMA node sharing the L3
        case Relation::Node:
            return "node";  // the same NUMA node, another L3
        case Relation::Socket:
            return "socket";  // the same package, another NUMA node
        case Relation::Remote:
            return "remote";  // another package
    }
    return "unknown";
}

// The NUMA node is compared before the L3, since an L3 can span several NUMA nodes (e.g., Intel's sub-NUMA
// clustering splits a socket with one L3 into several nodes).
inline Relation relation(std::size_t a, std::size_t b) {
    if (core_of(a) == core_of(b)) {
        return Relation::Smt;
    }
    if (numa_node(a) != numa_node(b)) {
        return package_of(a) == package_of(b) ? Relation::Socket : Relation::Remote;
    }
    if (has_l3(a) && has_l3(b) && l3_group(a) == l3_group(b)) {
        return Relation::L3;
    }
    return Relation::Node;
}

inline bool needs_distinct_cpus(int id) noexcept {
    return id != None && id != Same;
}

// The CPUs the first num_threads threads are pinned to; empty for policy None.
inline std::vector<std::size_t> cpu_assignment(int id, std::size_t num_threads) {
    if (id < 0 || id > max_id) {
        throw std::runtime_error("Invalid affinity " + std::to_string(id));
    }
    if (id == None) {
        return {};
    }
    auto cpus = available_cpus();
    if (cpus.empty()) {
        throw std::runtime_error("No CPUs available");
    }
    if (id == Same) {
        return std::vector<std::size_t>(num_threads, cpus[0]);
    }
    if (num_threads > cpus.size()) {
        throw std::runtime_error("Affinity '" + std::string(name(id)) + "' needs " + std::to_string(num_threads) +
                                 " CPUs, but only " + std::to_string(cpus.size()) + " are available");
    }
    std::vector<std::size_t> order;
    switch (id) {
        case ThreadId:
            order = std::move(cpus);
            break;
        case CloseCaches:
            order = detail::order_by(detail::read_topology(cpus), {4, 3, 2, 1, 0});
            break;
        case FarCaches:
            order = detail::order_by(detail::read_topology(cpus), {4, 0, 1, 2, 3});
            break;
        case CloseL3FarL1:
            order = detail::order_by(detail::read_topology(cpus), {4, 3, 2, 0, 1});
            break;
        case FarL1CloseL3:
            order = detail::order_by(detail::read_topology(cpus), {4, 0, 3, 2, 1});
            break;
        case CoresFirst:
        default:
            order = detail::order_by(detail::read_topology(cpus), {0, 4, 3, 2, 1});
            break;
    }
    order.resize(num_threads);
    return order;
}

inline void pin_current_thread(std::size_t cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); rc != 0) {
        fail("Failed to pin thread", rc);
    }
}

}  // namespace mb::affinity
