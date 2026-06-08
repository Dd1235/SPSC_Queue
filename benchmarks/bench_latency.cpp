// Ping-pong latency: the client sends a token through one queue and waits for
// the server to echo it back through another. Each measured iteration is one
// full round trip. We report p50/p99 (round trip) -- the number that matters for
// request/response style low-latency systems.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

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

    for (int i = 0; i < kWarmup + kIters; ++i) {
        auto t0 = bench::Clock::now();
        while (!to_server.try_push(static_cast<std::uint64_t>(i))) { /* spin */ }
        std::uint64_t r = 0;
        while (!to_client.try_pop(r)) { /* spin */ }
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      bench::Clock::now() - t0)
                      .count();
        if (i >= kWarmup) samples.push_back(static_cast<std::uint64_t>(ns));
    }
    server.join();

    std::uint64_t p50 = bench::percentile(samples, 0.50);
    std::uint64_t p99 = bench::percentile(samples, 0.99);
    std::uint64_t mn = bench::percentile(samples, 0.0);

    std::printf("ping-pong round-trip latency (%d samples)\n", kIters);
    std::printf("  min: %llu ns   p50: %llu ns   p99: %llu ns\n",
                (unsigned long long)mn, (unsigned long long)p50,
                (unsigned long long)p99);
    std::printf("  one-way ~ p50/2: %.0f ns\n", p50 / 2.0);
    return 0;
}
