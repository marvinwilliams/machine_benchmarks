// Throughput of atomic read-modify-write operations: all threads increment one shared counter (contended), or each
// thread its own counter in a separate cache line (uncontended), for a fixed time.
//
// Operations (acquire-release ordering):
//   faa: fetch_add
//   cas: load, then compare_exchange_weak until the increment succeeds; failed attempts are counted

#include "mb/benchmark.hpp"
#include "mb/json.hpp"
#include "mb/parse.hpp"
#include "mb/settings.hpp"
#include "mb/threads.hpp"

#include <cxxopts.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct alignas(mb::padding) Counter {
    std::atomic<std::uint64_t> value{0};
};

enum class Op { Faa, Cas };
enum class Mode { Shared, Private };

Op parse_op(std::string const& s) {
    if (s == "faa") {
        return Op::Faa;
    }
    if (s == "cas") {
        return Op::Cas;
    }
    throw std::runtime_error("Invalid operation '" + s + "' (faa, cas)");
}

Mode parse_mode(std::string const& s) {
    if (s == "shared") {
        return Mode::Shared;
    }
    if (s == "private") {
        return Mode::Private;
    }
    throw std::runtime_error("Invalid mode '" + s + "' (shared, private)");
}

char const* name(Op op) {
    return op == Op::Faa ? "faa" : "cas";
}

char const* name(Mode mode) {
    return mode == Mode::Shared ? "shared" : "private";
}

constexpr std::size_t batch = 1024;

// Runs batches of increments for at least min_time seconds; returns the elapsed time.
template <Op op>
double run(std::atomic<std::uint64_t>& c, double min_time, std::uint64_t& ops, std::uint64_t& failures) noexcept {
    auto start = mb::clock_type::now();
    double elapsed = 0;
    do {
        for (std::size_t i = 0; i < batch; ++i) {
            if constexpr (op == Op::Faa) {
                c.fetch_add(1, std::memory_order_acq_rel);
            } else {
                auto v = c.load(std::memory_order_relaxed);
                while (!c.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    ++failures;
                }
            }
        }
        ops += batch;
        elapsed = mb::seconds_since(start);
    } while (elapsed < min_time);
    return elapsed;
}

struct Settings {
    mb::ThreadSettings threads;
    std::string op_list = "faa,cas";
    std::string mode_list = "shared,private";
    double min_time = 0.2;
    int reps = 5;

    // Set by resolve().
    std::vector<Op> ops;
    std::vector<Mode> modes;

    void register_cmd_options(cxxopts::Options& cmd) {
        threads.register_cmd_options(cmd);
        // clang-format off
        cmd.add_options()
            ("o,ops", "Operations: faa, cas", cxxopts::value<std::string>(op_list)->default_value(op_list), "LIST")
            ("m,modes", "Modes: shared, private", cxxopts::value<std::string>(mode_list)->default_value(mode_list),
                "LIST")
            ("min-time", "Minimum duration of a repetition in seconds",
                cxxopts::value<double>(min_time)->default_value("0.2"), "SECONDS")
            ("r,reps", "Repetitions per operation and mode", cxxopts::value<int>(reps)->default_value("5"), "NUMBER");
        // clang-format on
    }

    void resolve() {
        threads.resolve();
        for (auto const& o : mb::split_list(op_list)) {
            ops.push_back(parse_op(o));
        }
        for (auto const& m : mb::split_list(mode_list)) {
            modes.push_back(parse_mode(m));
        }
        if (reps <= 0) {
            throw std::runtime_error("--reps must be positive");
        }
    }
};

struct Result {
    Mode mode;
    Op op;
    std::size_t rep;
    std::vector<std::uint64_t> ops, failures;
    std::vector<double> seconds;
};

void run_benchmark(Settings const& settings) {
    auto const threads = settings.threads.threads();
    auto const min_time = settings.min_time;
    settings.threads.apply_memory_policy();

    std::vector<Result> results;
    for (auto mode : settings.modes) {
        for (auto op : settings.ops) {
            for (std::size_t r = 0; r < static_cast<std::size_t>(settings.reps); ++r) {
                results.push_back({mode, op, r, std::vector<std::uint64_t>(threads),
                                   std::vector<std::uint64_t>(threads), std::vector<double>(threads)});
            }
        }
    }

    std::unique_ptr<Counter> shared;
    mb::SpinBarrier barrier(threads);
    mb::run_threads(threads, settings.threads.cpus, [&](std::size_t t) {
        // Every counter is allocated (and first touched) by the thread that uses it first.
        auto own = std::make_unique<Counter>();
        if (t == 0) {
            shared = std::make_unique<Counter>();
        }
        barrier.wait();
        for (auto& result : results) {
            auto& c = result.mode == Mode::Shared ? shared->value : own->value;
            std::uint64_t n = 0;
            std::uint64_t failures = 0;
            barrier.wait();
            auto elapsed = result.op == Op::Faa ? run<Op::Faa>(c, min_time, n, failures)
                                                : run<Op::Cas>(c, min_time, n, failures);
            result.ops[t] = n;
            result.failures[t] = failures;
            result.seconds[t] = elapsed;
        }
        barrier.wait();
    });

    mb::Json json(std::cout);
    json.begin_object();
    mb::write_header(json, "atomics");
    settings.threads.write(json);
    json.entry("min_time_s", min_time);
    json.key("results").begin_array();
    for (auto const& result : results) {
        double rate = 0;
        for (std::size_t t = 0; t < threads; ++t) {
            rate += static_cast<double>(result.ops[t]) / result.seconds[t];
        }
        json.begin_object();
        json.entry("mode", name(result.mode));
        json.entry("op", name(result.op));
        json.entry("rep", result.rep);
        json.entry("mops", rate / 1e6);
        json.entry("ops", result.ops);
        json.entry("failures", result.failures);
        json.entry("seconds", result.seconds);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    return mb::run<Settings>(argc, argv,
                             "Throughput of atomic increments on one shared counter or on one counter per thread.",
                             run_benchmark);
}
