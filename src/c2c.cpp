// Core-to-core latency: two threads pinned to CPUs a and b pass a counter in a single cache line back and forth
// (a writes an odd value, b answers with the next even one). The round trip time covers two transfers of the line.
// Every pair is labeled with how close the CPUs are (same core, same L3, same NUMA node, same package, or remote).
// With --relations, only a few pairs per relation of one base CPU are measured instead of all pairs.

#include "mb/benchmark.hpp"
#include "mb/json.hpp"
#include "mb/parse.hpp"
#include "mb/settings.hpp"
#include "mb/threads.hpp"
#include "mb/topology.hpp"

#include <cxxopts.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Settings {
    std::string cpu_list;
    int from = -1;
    bool relations = false;
    int per_relation = 1;
    int iterations = 2000;
    int samples = 5;
    int warmup = 200;

    // Set by resolve().
    std::vector<std::pair<std::size_t, std::size_t>> pairs;

    void register_cmd_options(cxxopts::Options& cmd) {
        // clang-format off
        cmd.add_options()
            ("cpus", "CPUs to measure (default: all available)", cxxopts::value<std::string>(cpu_list), "LIST")
            ("from", "Only measure the pairs of this CPU with the CPUs of --cpus (the base CPU with --relations)",
                cxxopts::value<int>(from)->default_value("-1"), "CPU")
            ("relations", "Measure only --per-relation partners of the base CPU per relation (smt, l3, node, "
                "socket, remote); the base defaults to the second CPU of the cores-first order",
                cxxopts::value<bool>(relations))
            ("k,per-relation", "Partners per relation, spread over the candidates",
                cxxopts::value<int>(per_relation)->default_value("1"), "NUMBER")
            ("n,iterations", "Round trips per sample", cxxopts::value<int>(iterations)->default_value("2000"), "NUMBER")
            ("r,samples", "Samples per pair", cxxopts::value<int>(samples)->default_value("5"), "NUMBER")
            ("warmup", "Round trips before the first sample", cxxopts::value<int>(warmup)->default_value("200"),
                "NUMBER");
        // clang-format on
    }

    void resolve() {
        if (iterations <= 0 || samples <= 0 || warmup < 0 || per_relation <= 0) {
            throw std::runtime_error("--iterations, --samples and --per-relation must be positive");
        }
        auto available = mb::affinity::available_cpus();
        auto cpus = cpu_list.empty() ? available : mb::parse_cpu_list(cpu_list);
        auto check = [&](std::size_t cpu) {
            if (std::find(available.begin(), available.end(), cpu) == available.end()) {
                throw std::runtime_error("CPU " + std::to_string(cpu) + " is not available");
            }
        };
        std::for_each(cpus.begin(), cpus.end(), check);
        if (from >= 0) {
            check(static_cast<std::size_t>(from));
        }
        if (relations) {
            auto base = static_cast<std::size_t>(from);
            if (from < 0) {
                auto order = mb::affinity::cpu_assignment(mb::affinity::CoresFirst, available.size());
                base = order.size() > 1 ? order[1] : order[0];
            }
            auto const k = static_cast<std::size_t>(per_relation);
            for (auto rel : mb::affinity::relations) {
                std::vector<std::size_t> candidates;
                for (auto cpu : cpus) {
                    if (cpu != base && mb::affinity::relation(base, cpu) == rel) {
                        candidates.push_back(cpu);
                    }
                }
                // The midpoints of k equal parts of the candidates, to spread the partners over the topology.
                auto const m = candidates.size();
                for (std::size_t j = 0; j < std::min(k, m); ++j) {
                    pairs.emplace_back(base, candidates[(2 * j + 1) * m / (2 * std::min(k, m))]);
                }
            }
        } else if (from >= 0) {
            for (auto cpu : cpus) {
                if (cpu != static_cast<std::size_t>(from)) {
                    pairs.emplace_back(from, cpu);
                }
            }
        } else {
            for (std::size_t i = 0; i < cpus.size(); ++i) {
                for (std::size_t j = i + 1; j < cpus.size(); ++j) {
                    pairs.emplace_back(cpus[i], cpus[j]);
                }
            }
        }
    }
};

struct alignas(mb::padding) Line {
    std::atomic<std::uint64_t> value{0};
};

// Round trip times in ns, one per sample.
std::vector<double> measure(std::size_t a, std::size_t b, std::size_t warmup, std::size_t samples,
                            std::size_t iterations) {
    std::vector<double> rtt(samples);
    std::unique_ptr<Line> line;
    mb::SpinBarrier barrier(2);
    auto const rounds = warmup + samples * iterations;
    mb::run_threads(2, {a, b}, [&](std::size_t t) {
        if (t == 0) {
            // Allocated (and first touched) by the thread on a.
            line = std::make_unique<Line>();
        }
        barrier.wait();
        auto& v = line->value;
        if (t == 0) {
            mb::clock_type::time_point start;
            for (std::uint64_t k = 0; k < rounds; ++k) {
                if (k >= warmup && (k - warmup) % iterations == 0) {
                    start = mb::clock_type::now();
                }
                v.store(2 * k + 1, std::memory_order_release);
                while (v.load(std::memory_order_acquire) != 2 * k + 2) {
                }
                if (k >= warmup && (k - warmup + 1) % iterations == 0) {
                    rtt[(k - warmup) / iterations] = mb::seconds_since(start) * 1e9 / static_cast<double>(iterations);
                }
            }
        } else {
            for (std::uint64_t k = 0; k < rounds; ++k) {
                while (v.load(std::memory_order_acquire) != 2 * k + 1) {
                }
                v.store(2 * k + 2, std::memory_order_release);
            }
        }
        barrier.wait();
    });
    return rtt;
}

void run_benchmark(Settings const& settings) {
    auto const iterations = static_cast<std::size_t>(settings.iterations);
    auto const samples = static_cast<std::size_t>(settings.samples);
    auto const warmup = static_cast<std::size_t>(settings.warmup);
    std::vector<std::vector<double>> results;
    results.reserve(settings.pairs.size());
    for (auto [a, b] : settings.pairs) {
        results.push_back(measure(a, b, warmup, samples, iterations));
    }

    mb::Json json(std::cout);
    json.begin_object();
    mb::write_header(json, "c2c");
    json.entry("iterations", iterations);
    json.entry("warmup", warmup);
    json.key("results").begin_array();
    for (std::size_t p = 0; p < settings.pairs.size(); ++p) {
        auto [a, b] = settings.pairs[p];
        json.begin_object();
        json.entry("cpu_a", a);
        json.entry("cpu_b", b);
        json.entry("relation", mb::affinity::name(mb::affinity::relation(a, b)));
        json.entry("round_trip_ns", results[p]);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    return mb::run<Settings>(argc, argv,
                             "Core-to-core latency: two pinned threads pass a counter in one cache line back and "
                             "forth. Measures all pairs of the given CPUs, the given CPU against all others, or "
                             "(--relations) a few partners of one CPU per topological relation.",
                             run_benchmark);
}
