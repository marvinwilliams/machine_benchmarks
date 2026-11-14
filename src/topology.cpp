// Prints the CPU topology, the pinning orders and the caches of the machine as JSON, to label the results of the
// other benchmarks (e.g., which CPU pairs of c2c share a core, an L3, a NUMA node or a package).

#include "mb/benchmark.hpp"
#include "mb/json.hpp"
#include "mb/memory.hpp"
#include "mb/settings.hpp"
#include "mb/topology.hpp"

#include <cxxopts.hpp>

#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::string read_line(std::string const& path) {
    std::string line;
    std::ifstream in(path);
    std::getline(in, line);
    return line;
}

struct CacheKey {
    int level;
    std::string type;
    bool operator<(CacheKey const& o) const {
        return std::tie(level, type) < std::tie(o.level, o.type);
    }
};

struct CacheInfo {
    std::string size;
    std::string line_size;
    std::string ways;
    std::size_t cpus_per_instance = 0;
    std::set<std::string> instances;  // distinct shared CPU lists
};

struct Settings {
    void register_cmd_options(cxxopts::Options&) {
    }

    void resolve() {
    }
};

void print_topology(Settings const&) {
    auto cpus = mb::affinity::available_cpus();

    std::map<CacheKey, CacheInfo> caches;
    for (auto cpu : cpus) {
        auto base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index";
        for (int i = 0;; ++i) {
            auto index = base + std::to_string(i) + "/";
            int level = 0;
            if (!(std::ifstream(index + "level") >> level)) {
                break;
            }
            auto& info = caches[{level, read_line(index + "type")}];
            info.size = read_line(index + "size");
            info.line_size = read_line(index + "coherency_line_size");
            info.ways = read_line(index + "ways_of_associativity");
            auto shared = read_line(index + "shared_cpu_list");
            info.cpus_per_instance = mb::parse_cpu_list(shared).size();
            info.instances.insert(shared);
        }
    }

    mb::Json json(std::cout);
    json.begin_object();
    mb::write_header(json, "topology");
    json.key("cpus").begin_array();
    for (auto cpu : cpus) {
        auto topology = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        json.begin_object();
        json.entry("cpu", cpu);
        json.entry("core", mb::affinity::first_cpu_of(topology + "thread_siblings_list", cpu));
        json.entry("l3", mb::affinity::l3_group(cpu));
        json.entry("numa_node", mb::affinity::numa_node(cpu));
        json.entry("package", mb::affinity::first_cpu_of(topology + "physical_package_id", 0));
        json.end_object();
    }
    json.end_array();
    json.key("orders").begin_object();
    for (int id = mb::affinity::ThreadId; id <= mb::affinity::max_id; ++id) {
        if (id == mb::affinity::Same) {
            continue;
        }
        json.entry(mb::affinity::name(id), mb::affinity::cpu_assignment(id, cpus.size()));
    }
    json.end_object();
    json.key("caches").begin_array();
    for (auto const& [key, info] : caches) {
        json.begin_object();
        json.entry("level", key.level);
        json.entry("type", key.type);
        json.entry("size", info.size);
        json.entry("line_size", info.line_size);
        json.entry("ways", info.ways);
        json.entry("cpus_per_instance", info.cpus_per_instance);
        json.entry("instances", info.instances.size());
        json.end_object();
    }
    json.end_array();
    json.entry("numa_nodes_with_memory", mb::memory_policy::nodes_with_memory());
    json.entry("smt_active", read_line("/sys/devices/system/cpu/smt/active"));
    json.entry("thp_enabled", read_line("/sys/kernel/mm/transparent_hugepage/enabled"));
    json.entry("available_memory_bytes", mb::available_memory());
    json.end_object();
    std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    return mb::run<Settings>(argc, argv, "Prints the CPU topology, pinning orders and caches as JSON.",
                             print_topology);
}
