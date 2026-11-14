#pragma once

#include "mb/json.hpp"
#include "mb/memory.hpp"
#include "mb/parse.hpp"
#include "mb/topology.hpp"

#include <cxxopts.hpp>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mb {

// Thread count, pinning and NUMA placement, with the same options as the MultiQueue experiments.
struct ThreadSettings {
    int num_threads = 1;
    int affinity = affinity::default_id;
    int memory = memory_policy::default_id;
    std::string cpu_list;

    // Set by resolve().
    std::vector<std::size_t> cpus;  // cpus[i]: the CPU thread i is pinned to; empty if unpinned
    std::vector<std::size_t> memory_nodes;

    void register_cmd_options(cxxopts::Options& cmd) {
        // clang-format off
        cmd.add_options()
            ("j,threads", "Number of threads", cxxopts::value<int>(num_threads)->default_value("1"), "NUMBER")
            ("a,affinity", "CPU affinity ("
                "0: None, "
                "1: Thread Id, "
                "2: Same, "
                "3: Close caches, "
                "4: Far caches, "
                "5: Close L3 Far L1, "
                "6: Far L1 Close L3, "
                "7: Cores first (L3, NUMA, SMT))"
                , cxxopts::value<int>(affinity)->default_value("7"), "NUMBER")
            ("memory", "Memory placement ("
                "0: First touch, "
                "1: Interleave over the NUMA nodes of the pinned CPUs, "
                "2: Interleave over all NUMA nodes)"
                , cxxopts::value<int>(memory)->default_value("1"), "NUMBER")
            ("cpus", "Pin thread i to the i-th CPU of this list (overrides --affinity)",
                cxxopts::value<std::string>(cpu_list), "LIST");
        // clang-format on
    }

    void resolve() {
        if (num_threads <= 0) {
            throw std::runtime_error("Number of threads must be positive");
        }
        auto const n = static_cast<std::size_t>(num_threads);
        if (cpu_list.empty()) {
            cpus = affinity::cpu_assignment(affinity, n);
        } else {
            cpus = parse_cpu_list(cpu_list);
            if (cpus.size() < n) {
                throw std::runtime_error("--cpus lists fewer CPUs than threads");
            }
            cpus.resize(n);
            auto available = affinity::available_cpus();
            for (auto cpu : cpus) {
                if (std::find(available.begin(), available.end(), cpu) == available.end()) {
                    throw std::runtime_error("CPU " + std::to_string(cpu) + " is not available");
                }
            }
            affinity = -1;
        }
        memory_nodes = memory_policy::nodes(memory, cpus);
    }

    [[nodiscard]] std::size_t threads() const noexcept {
        return static_cast<std::size_t>(num_threads);
    }

    void apply_memory_policy() const {
        memory_policy::apply(memory_nodes);
    }

    void write(Json& json) const {
        json.entry("num_threads", num_threads);
        json.entry("affinity", affinity);
        json.entry("memory_policy", memory);
        json.entry("cpus", cpus);
        json.entry("memory_nodes", memory_nodes);
    }
};

inline std::string hostname() {
    char buf[256] = {};
    gethostname(buf, sizeof(buf) - 1);
    return buf;
}

// Common header entries of every benchmark's output.
inline void write_header(Json& json, char const* benchmark) {
    json.entry("benchmark", benchmark);
    json.entry("host", hostname());
    json.entry("compiler", __VERSION__);
    json.entry("base_page_size", base_page_size());
    json.entry("huge_page_size", huge_page_size());
}

}  // namespace mb
