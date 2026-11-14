// Latency of dependent loads: every thread chases pointers through its own working set, visiting its cache lines in
// a random cyclic order, so neither the prefetchers nor memory-level parallelism hide the latency. With more than
// one thread, this is the latency under the load of the other threads.

#include "mb/benchmark.hpp"
#include "mb/json.hpp"
#include "mb/memory.hpp"
#include "mb/parse.hpp"
#include "mb/settings.hpp"
#include "mb/threads.hpp"

#include <cxxopts.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Settings {
    mb::ThreadSettings threads;
    std::string size_list = "4KiB:1GiB:4";
    std::string page_mode = "default";
    std::string line_size = "64";
    double min_time = 0.1;
    int reps = 5;
    std::size_t warmup = std::size_t{1} << 22;
    double warmup_time = 0.2;
    std::uint64_t seed = 1;

    // Set by resolve().
    std::vector<std::size_t> sizes;
    mb::Pages pages = mb::Pages::Default;
    std::size_t line = 64;

    void register_cmd_options(cxxopts::Options& cmd) {
        threads.register_cmd_options(cmd);
        // clang-format off
        cmd.add_options()
            ("s,sizes", "Working set sizes per thread: sizes and ranges min:max:steps (steps sizes per doubling)",
                cxxopts::value<std::string>(size_list)->default_value(size_list), "LIST")
            ("pages", "Pages: default, small, thp, hugetlb",
                cxxopts::value<std::string>(page_mode)->default_value(page_mode), "MODE")
            ("line", "Distance between consecutive pointers (the cache line size)",
                cxxopts::value<std::string>(line_size)->default_value(line_size), "BYTES")
            ("min-time", "Minimum duration of a repetition in seconds",
                cxxopts::value<double>(min_time)->default_value("0.1"), "SECONDS")
            ("r,reps", "Repetitions per size", cxxopts::value<int>(reps)->default_value("5"), "NUMBER")
            ("warmup", "Loads before the first repetition: one round through the working set, but at most this many",
                cxxopts::value<std::size_t>(warmup)->default_value("4194304"), "NUMBER")
            ("warmup-time", "Busy-wait before the first measurement in seconds",
                cxxopts::value<double>(warmup_time)->default_value("0.2"), "SECONDS")
            ("seed", "Seed of the random orders", cxxopts::value<std::uint64_t>(seed)->default_value("1"), "NUMBER");
        // clang-format on
    }

    void resolve() {
        threads.resolve();
        pages = mb::parse_pages(page_mode);
        line = mb::parse_size(line_size);
        if (line < sizeof(void*) || line % sizeof(void*) != 0) {
            throw std::runtime_error("--line must be a positive multiple of the pointer size");
        }
        sizes = mb::parse_sizes(size_list, line);
        if (reps <= 0) {
            throw std::runtime_error("--reps must be positive");
        }
    }
};

std::byte* next(std::byte* p) noexcept {
    return *reinterpret_cast<std::byte* const*>(p);
}

// Links the lines of data into a single cycle in random order (Sattolo's algorithm). Writes every line in address
// order, which also places the pages under first touch.
std::byte* build_chain(std::byte* data, std::size_t lines, std::size_t line_bytes, std::uint64_t seed) {
    std::vector<std::size_t> successor(lines);
    std::iota(successor.begin(), successor.end(), std::size_t{0});
    std::mt19937_64 rng(seed);
    for (std::size_t i = lines - 1; i > 0; --i) {
        std::uniform_int_distribution<std::size_t> dist(0, i - 1);
        std::swap(successor[i], successor[dist(rng)]);
    }
    for (std::size_t i = 0; i < lines; ++i) {
        *reinterpret_cast<std::byte**>(data + i * line_bytes) = data + successor[i] * line_bytes;
    }
    return data;
}

[[gnu::noinline]] std::byte* chase(std::byte* p, std::size_t loads) noexcept {
#pragma GCC unroll 16
    for (std::size_t k = 0; k < loads; ++k) {
        p = next(p);
    }
    return p;
}

constexpr std::size_t batch = 4096;

void run_benchmark(Settings const& settings) {
    auto const threads = settings.threads.threads();
    auto const& sizes = settings.sizes;
    auto const reps = static_cast<std::size_t>(settings.reps);
    settings.threads.apply_memory_policy();

    // ns_per_load[(size * reps + rep) * threads + thread]
    std::vector<double> ns_per_load(sizes.size() * reps * threads);
    mb::SpinBarrier barrier(threads);
    mb::run_threads(threads, settings.threads.cpus, [&](std::size_t t) {
        mb::spin_for(settings.warmup_time);
        for (std::size_t s = 0; s < sizes.size(); ++s) {
            auto const lines = sizes[s] / settings.line;
            mb::Buffer buffer(sizes[s], settings.pages);
            auto* p = build_chain(buffer.data(), lines, settings.line, settings.seed * 1000003 + t * 1009 + s);
            p = chase(p, std::min(lines, settings.warmup));
            for (std::size_t r = 0; r < reps; ++r) {
                barrier.wait();
                auto start = mb::clock_type::now();
                std::size_t loads = 0;
                double elapsed = 0;
                do {
                    p = chase(p, batch);
                    // Consumes the end of the chain, so the batch's loads happen before the clock is read.
                    mb::do_not_optimize(p);
                    loads += batch;
                    elapsed = mb::seconds_since(start);
                } while (elapsed < settings.min_time);
                ns_per_load[(s * reps + r) * threads + t] = elapsed * 1e9 / static_cast<double>(loads);
            }
            barrier.wait();
            if (t == 0) {
                std::clog << "size " << sizes[s] << ": " << ns_per_load[(s * reps) * threads] << " ns\n";
            }
        }
    });

    mb::Json json(std::cout);
    json.begin_object();
    mb::write_header(json, "latency");
    settings.threads.write(json);
    json.entry("pages", mb::name(settings.pages));
    json.entry("line_bytes", settings.line);
    json.entry("min_time_s", settings.min_time);
    json.entry("seed", settings.seed);
    json.key("results").begin_array();
    for (std::size_t s = 0; s < sizes.size(); ++s) {
        for (std::size_t r = 0; r < reps; ++r) {
            auto first = ns_per_load.begin() + static_cast<std::ptrdiff_t>((s * reps + r) * threads);
            json.begin_object();
            json.entry("size_bytes", sizes[s]);
            json.entry("rep", r);
            json.entry("ns_per_load", std::vector<double>(first, first + static_cast<std::ptrdiff_t>(threads)));
            json.end_object();
        }
    }
    json.end_array();
    json.end_object();
    std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    return mb::run<Settings>(argc, argv,
                             "Latency of dependent loads: every thread chases pointers through its own working set, "
                             "visiting its cache lines in a random cyclic order.",
                             run_benchmark);
}
