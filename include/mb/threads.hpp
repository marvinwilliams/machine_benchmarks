#pragma once

#include "mb/topology.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace mb {

using clock_type = std::chrono::steady_clock;

inline double seconds_since(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

inline double seconds(clock_type::time_point start, clock_type::time_point end) {
    return std::chrono::duration<double>(end - start).count();
}

// Busy-waits, e.g. to let the clock frequency ramp up before measuring.
inline void spin_for(double seconds) noexcept {
    auto start = clock_type::now();
    while (seconds_since(start) < seconds) {
    }
}

// Keeps the compiler from optimizing away the computation of value.
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// Tells the compiler that all memory may be read (and written) here, so preceding stores cannot be removed.
inline void clobber_memory() {
    asm volatile("" : : : "memory");
}

// Large enough to keep two objects off the same cache line and off adjacent lines fetched together by
// spatial prefetchers.
inline constexpr std::size_t padding = 128;

class SpinBarrier {
    std::size_t num_threads_;
    alignas(padding) std::atomic<std::size_t> count_{0};
    alignas(padding) std::atomic<std::size_t> generation_{0};

   public:
    explicit SpinBarrier(std::size_t num_threads) noexcept : num_threads_{num_threads} {
    }

    void wait() noexcept {
        auto generation = generation_.load(std::memory_order_acquire);
        if (count_.fetch_add(1, std::memory_order_acq_rel) + 1 == num_threads_) {
            count_.store(0, std::memory_order_relaxed);
            generation_.store(generation + 1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
        }
    }
};

// Runs fn(thread_index) on num_threads threads, thread i pinned to cpus[i] (unpinned if cpus is empty).
template <typename Fn>
void run_threads(std::size_t num_threads, std::vector<std::size_t> const& cpus, Fn&& fn) {
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i] {
            if (!cpus.empty()) {
                affinity::pin_current_thread(cpus[i]);
            }
            fn(i);
        });
    }
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace mb
