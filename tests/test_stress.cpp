// Multithreaded stress: one producer streams a monotonic sequence 0..N-1, one
// consumer drains it. The consumer asserts it sees exactly 0,1,2,...,N-1 in
// order -- this single invariant catches dropped items, duplicated items, and
// reordered items (i.e. a wrong memory ordering) all at once.
//
// Run under ThreadSanitizer for the strongest signal: build with
// -DSPSC_ENABLE_TSAN=ON. TSan understands acquire/release and will flag a stray
// relaxed load that the value-based check might miss on x86/ARM.
#include "spsc/spsc_queue.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <thread>

using spsc::SPSCQueue;

// Returns true on success. `yield` injects scheduler hand-offs to widen the
// interleaving window (more bug-finding power than a tight spin).
template <bool Padded, bool Cached>
static bool run_one(std::size_t capacity, std::uint64_t count, bool yield) {
    SPSCQueue<std::uint64_t, Padded, Cached> q(capacity);
    bool ordered = true;

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        std::uint64_t v = 0;
        while (expected < count) {
            if (q.try_pop(v)) {
                if (v != expected) ordered = false;
                ++expected;
            } else if (yield) {
                std::this_thread::yield();
            }
        }
    });

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!q.try_push(i)) {
                if (yield) std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    return ordered;
}

int main() {
    // Big run on the production config: lots of wraparounds, tight spin.
    CHECK((run_one<true, true>(1024, 5'000'000, false)));

    // Tiny capacity forces constant full/empty transitions (the contended path
    // where the cached-index refresh actually fires).
    CHECK((run_one<true, true>(1, 1'000'000, false)));
    CHECK((run_one<true, true>(2, 1'000'000, true)));

    // Same invariant must hold for every configuration.
    CHECK((run_one<false, true>(64, 1'000'000, false)));
    CHECK((run_one<true, false>(64, 1'000'000, false)));
    CHECK((run_one<false, false>(64, 1'000'000, true)));

    return TEST_SUMMARY();
}
