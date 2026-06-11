// Shared benchmark scaffolding: timing, a generic throughput driver, latency
// percentile helpers, and a std::mutex baseline with the same try_ API so the
// drivers are queue-agnostic.
//
// Note on CPU pinning: the spec calls for pinning producer/consumer to two
// cores. macOS does not expose usable thread affinity on Apple silicon (the
// affinity API is a hint the scheduler ignores), so we do not pin here; runs
// are repeated and the best is reported to suppress scheduler noise. On Linux
// you would pin with pthread_setaffinity_np.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

inline double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Stream `count` items producer -> consumer and return throughput in
// millions of ops/sec (one op == one push paired with one pop). `reps` runs are
// timed and the best is returned. A short warmup primes caches/branch predictors.
template <class Queue> double throughput_mops(Queue& q, std::uint64_t count, int reps = 3) {
    auto one_run = [&](std::uint64_t n) {
        auto t0 = Clock::now();
        std::thread consumer([&] {
            std::uint64_t v = 0, got = 0;
            while (got < n) {
                if (q.try_pop(v)) ++got;
            }
        });
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < n; ++i) {
                while (!q.try_push(i)) { /* spin: pure throughput */
                }
            }
        });
        producer.join();
        consumer.join();
        return seconds_since(t0);
    };

    one_run(count / 10 + 1);  // warmup
    double best = 1e300;
    for (int r = 0; r < reps; ++r) best = std::min(best, one_run(count));
    return (static_cast<double>(count) / best) / 1e6;
}

inline std::uint64_t percentile(std::vector<std::uint64_t>& samples, double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    std::size_t idx = static_cast<std::size_t>(p * (samples.size() - 1));
    return samples[idx];
}

// A bounded queue guarded by a single std::mutex -- the "obvious" baseline the
// lock-free version is supposed to beat. Same try_ API as SPSCQueue so the
// throughput driver above works unchanged.
template <class T> class MutexQueue {
public:
    explicit MutexQueue(std::size_t capacity) : cap_(capacity) {}

    bool try_push(const T& v) {
        std::lock_guard<std::mutex> g(m_);
        if (q_.size() >= cap_) return false;
        q_.push(v);
        return true;
    }
    bool try_push(T&& v) {
        std::lock_guard<std::mutex> g(m_);
        if (q_.size() >= cap_) return false;
        q_.push(std::move(v));
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    std::size_t capacity() const { return cap_; }

private:
    std::mutex m_;
    std::queue<T> q_;
    std::size_t cap_;
};

}  // namespace bench
