// Bandwidth of streaming kernels: every thread repeatedly streams through its own working set. All threads start
// together and run the same number of passes; the bandwidth is the total number of bytes over the time from the
// first start to the last end.
//
// Kernels (on 64-bit words; the bytes counted per pass equal the working set size):
//   read:  sums all words
//   write: overwrites all words (the hardware may read the lines first, which is not counted)
//   copy:  b[i] = a[i] ^ x for the two halves a and b of the working set

#include "mb/benchmark.hpp"
#include "mb/json.hpp"
#include "mb/memory.hpp"
#include "mb/parse.hpp"
#include "mb/settings.hpp"
#include "mb/threads.hpp"

#include <cxxopts.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using word = std::uint64_t;

// The kernels are not inlined, so every pass is a separate call. run_pass consumes the result of read and makes
// the stores of write and copy observable, so that the compiler can neither merge nor drop passes.
[[gnu::noinline]] word kernel_read(word const* a, std::size_t n) noexcept {
    word sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += a[i];
    }
    return sum;
}

[[gnu::noinline]] void kernel_write(word* a, std::size_t n, word x) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = x + i;
    }
}

[[gnu::noinline]] void kernel_copy(word* __restrict b, word const* __restrict a, std::size_t n, word x) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = a[i] ^ x;
    }
}

enum class Kernel { Read, Write, Copy };

Kernel parse_kernel(std::string const& s) {
    if (s == "read") {
        return Kernel::Read;
    }
    if (s == "write") {
        return Kernel::Write;
    }
    if (s == "copy") {
        return Kernel::Copy;
    }
    throw std::runtime_error("Invalid kernel '" + s + "' (read, write, copy)");
}

char const* name(Kernel k) {
    switch (k) {
        case Kernel::Read:
            return "read";
        case Kernel::Write:
            return "write";
        case Kernel::Copy:
            return "copy";
    }
    return "unknown";
}

void run_pass(Kernel kernel, word* data, std::size_t words, word x) noexcept {
    switch (kernel) {
        case Kernel::Read:
            mb::do_not_optimize(kernel_read(data, words));
            break;
        case Kernel::Write:
            kernel_write(data, words, x);
            mb::clobber_memory();
            break;
        case Kernel::Copy:
            kernel_copy(data + words / 2, data, words / 2, x);
            mb::clobber_memory();
            break;
    }
}

struct Settings {
    mb::ThreadSettings threads;
    std::string size_list = "4KiB:1GiB:4";
    std::string kernel_list = "read,write,copy";
    std::string page_mode = "default";
    double min_time = 0.1;
    int reps = 5;
    double warmup_time = 0.2;

    // Set by resolve().
    std::vector<std::size_t> sizes;
    std::vector<Kernel> kernels;
    mb::Pages pages = mb::Pages::Default;

    void register_cmd_options(cxxopts::Options& cmd) {
        threads.register_cmd_options(cmd);
        // clang-format off
        cmd.add_options()
            ("s,sizes", "Working set sizes per thread: sizes and ranges min:max:steps (steps sizes per doubling)",
                cxxopts::value<std::string>(size_list)->default_value(size_list), "LIST")
            ("k,kernels", "Kernels: read, write, copy",
                cxxopts::value<std::string>(kernel_list)->default_value(kernel_list), "LIST")
            ("pages", "Pages: default, small, thp, hugetlb",
                cxxopts::value<std::string>(page_mode)->default_value(page_mode), "MODE")
            ("min-time", "Minimum duration of a repetition in seconds",
                cxxopts::value<double>(min_time)->default_value("0.1"), "SECONDS")
            ("r,reps", "Repetitions per size and kernel", cxxopts::value<int>(reps)->default_value("5"), "NUMBER")
            ("warmup-time", "Busy-wait before the first measurement in seconds",
                cxxopts::value<double>(warmup_time)->default_value("0.2"), "SECONDS");
        // clang-format on
    }

    void resolve() {
        threads.resolve();
        // Multiples of two cache lines, so that both halves of copy are whole lines.
        sizes = mb::parse_sizes(size_list, 128);
        for (auto const& k : mb::split_list(kernel_list)) {
            kernels.push_back(parse_kernel(k));
        }
        pages = mb::parse_pages(page_mode);
        if (reps <= 0) {
            throw std::runtime_error("--reps must be positive");
        }
    }
};

struct Measurement {
    std::size_t passes = 0;
    std::vector<mb::clock_type::time_point> start, end;
};

void run_benchmark(Settings const& settings) {
    auto const threads = settings.threads.threads();
    auto const& sizes = settings.sizes;
    auto const& kernels = settings.kernels;
    auto const reps = static_cast<std::size_t>(settings.reps);
    auto const min_time = settings.min_time;
    settings.threads.apply_memory_policy();

    // measurements[(size * kernels + kernel) * reps + rep]
    std::vector<Measurement> measurements(sizes.size() * kernels.size() * reps);
    for (auto& m : measurements) {
        m.start.resize(threads);
        m.end.resize(threads);
    }
    std::vector<double> calibration(threads);
    mb::SpinBarrier barrier(threads);
    mb::run_threads(threads, settings.threads.cpus, [&](std::size_t t) {
        mb::spin_for(settings.warmup_time);
        for (std::size_t s = 0; s < sizes.size(); ++s) {
            auto const words = sizes[s] / sizeof(word);
            mb::Buffer buffer(sizes[s], settings.pages);
            auto* data = buffer.as<word>();
            for (std::size_t i = 0; i < words; ++i) {
                data[i] = i;
            }
            for (std::size_t k = 0; k < kernels.size(); ++k) {
                // Calibrate the number of passes to the slowest thread.
                barrier.wait();
                auto start = mb::clock_type::now();
                std::size_t calls = 0;
                double elapsed = 0;
                do {
                    run_pass(kernels[k], data, words, calls);
                    ++calls;
                    elapsed = mb::seconds_since(start);
                } while (elapsed < 0.01 * min_time);
                calibration[t] = elapsed / static_cast<double>(calls);
                barrier.wait();
                auto slowest = *std::max_element(calibration.begin(), calibration.end());
                auto passes = static_cast<std::size_t>(std::max(1.0, std::ceil(min_time / slowest)));
                for (std::size_t r = 0; r < reps; ++r) {
                    auto& m = measurements[(s * kernels.size() + k) * reps + r];
                    barrier.wait();
                    m.start[t] = mb::clock_type::now();
                    for (std::size_t i = 0; i < passes; ++i) {
                        run_pass(kernels[k], data, words, i);
                    }
                    m.end[t] = mb::clock_type::now();
                    m.passes = passes;
                }
            }
            barrier.wait();
            if (t == 0) {
                std::clog << "size " << sizes[s] << " done\n";
            }
        }
    });

    mb::Json json(std::cout);
    json.begin_object();
    mb::write_header(json, "bandwidth");
    settings.threads.write(json);
    json.entry("pages", mb::name(settings.pages));
    json.entry("word_bytes", sizeof(word));
    json.entry("min_time_s", min_time);
    json.key("results").begin_array();
    for (std::size_t s = 0; s < sizes.size(); ++s) {
        for (std::size_t k = 0; k < kernels.size(); ++k) {
            for (std::size_t r = 0; r < reps; ++r) {
                auto const& m = measurements[(s * kernels.size() + k) * reps + r];
                auto first = *std::min_element(m.start.begin(), m.start.end());
                auto last = *std::max_element(m.end.begin(), m.end.end());
                auto bytes_per_thread = static_cast<double>(m.passes) * static_cast<double>(sizes[s]);
                std::vector<double> thread_gbps(threads);
                for (std::size_t t = 0; t < threads; ++t) {
                    thread_gbps[t] = bytes_per_thread / mb::seconds(m.start[t], m.end[t]) / 1e9;
                }
                auto wall = mb::seconds(first, last);
                json.begin_object();
                json.entry("size_bytes", sizes[s]);
                json.entry("kernel", name(kernels[k]));
                json.entry("rep", r);
                json.entry("passes", m.passes);
                json.entry("seconds", wall);
                json.entry("gbps", bytes_per_thread * static_cast<double>(threads) / wall / 1e9);
                json.entry("thread_gbps", thread_gbps);
                json.end_object();
            }
        }
    }
    json.end_array();
    json.end_object();
    std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    return mb::run<Settings>(
        argc, argv, "Bandwidth of streaming kernels: every thread repeatedly streams through its own working set.",
        run_benchmark);
}
