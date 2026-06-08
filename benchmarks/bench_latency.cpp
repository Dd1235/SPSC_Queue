// Ping-pong latency: the client sends a token through one queue and waits for
// the server to echo it back through another. Each measured iteration is one
// full round trip.
//
// Caveat that this benchmark surfaces honestly: steady_clock on this machine
// quantizes at ~tens of nanoseconds, which is the same order as a round trip.
// Per-sample percentiles are therefore coarse multiples of one tick, so we also
// report the timer resolution and a resolution-independent mean (total elapsed
// time / iterations). Treat the mean as the precise figure and p50/p99 as the
// tail shape.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

// Smallest non-zero gap between two consecutive clock reads -> effective timer
// resolution. Sampled many times and the minimum kept.
static std::uint64_t timer_resolution_ns() {
    std::uint64_t best = ~0ull;
    for (int i = 0; i < 100000; ++i) {
        auto a = bench::Clock::now();
        auto b = bench::Clock::now();
        auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        if (d > 0 && static_cast<std::uint64_t>(d) < best) best = d;
    }
    return best;
}

int main() {
    constexpr int kWarmup = 50'000;
    constexpr int kIters = 200'000;

    spsc::SPSCQueue<std::uint64_t> to_server(64);
    spsc::SPSCQueue<std::uint64_t> to_client(64);

    std::thread server([&] {
        for (int i = 0; i < kWarmup + kIters; ++i) {
            std::uint64_t v = 0;
            while (!to_server.try_pop(v)) { /* spin */ }
            while (!to_client.try_push(v)) { /* spin */ }
        }
    });

    std::vector<std::uint64_t> samples;
    samples.reserve(kIters);

    auto t_measure_start = bench::Clock::now();
    for (int i = 0; i < kWarmup + kIters; ++i) {
        auto t0 = bench::Clock::now();
        while (!to_server.try_push(static_cast<std::uint64_t>(i))) { /* spin */ }
        std::uint64_t r = 0;
        while (!to_client.try_pop(r)) { /* spin */ }
        if (i == kWarmup) t_measure_start = bench::Clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      bench::Clock::now() - t0)
                      .count();
        if (i >= kWarmup) samples.push_back(static_cast<std::uint64_t>(ns));
    }
    double total_s = bench::seconds_since(t_measure_start);
    server.join();

    std::uint64_t p50 = bench::percentile(samples, 0.50);
    std::uint64_t p99 = bench::percentile(samples, 0.99);
    double mean_ns = total_s * 1e9 / kIters;

    std::printf("ping-pong round-trip latency (%d samples)\n", kIters);
    std::printf("  timer resolution : ~%llu ns (percentiles are quantized to this)\n",
                (unsigned long long)timer_resolution_ns());
    std::printf("  mean round-trip  : %.0f ns   (one-way ~ %.0f ns)\n",
                mean_ns, mean_ns / 2.0);
    std::printf("  p50: %llu ns   p99: %llu ns\n",
                (unsigned long long)p50, (unsigned long long)p99);
    return 0;
}
