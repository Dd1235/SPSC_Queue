// Single-threaded contracts for the MPMC queues: FIFO order, empty behavior,
// interleaved push/pop, and object lifetime through epoch-based reclamation.
// Templated so each queue added in later phases reuses the same checks.
#include "mpmc/faa_queue.hpp"
#include "mpmc/ms_queue.hpp"
#include "mpmc/vyukov_queue.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

// Capacity chosen so the bounded queues never hit full in these two helpers
// (MS ignores the capacity argument entirely).
template <class Q> static void test_fifo_and_empty() {
    Q q(256);
    int out = -1;
    CHECK(!q.try_pop(out));  // empty at start
    for (int i = 0; i < 200; ++i) CHECK(q.try_push(i));
    for (int i = 0; i < 200; ++i) {
        REQUIRE(q.try_pop(out));
        CHECK(out == i);  // strict FIFO
    }
    CHECK(!q.try_pop(out));  // drained
}

template <class Q> static void test_interleaved() {
    Q q(2048);
    int out = -1;
    for (int round = 0; round < 1000; ++round) {
        CHECK(q.try_push(2 * round));
        CHECK(q.try_push(2 * round + 1));
        REQUIRE(q.try_pop(out));
        CHECK(out == round);  // one behind: pops trail pushes by one element
    }
    // 1000 elements remain (pushed 2000, popped 1000).
    int remaining = 0;
    while (q.try_pop(out)) ++remaining;
    CHECK(remaining == 1000);
}

// The MS queue defers node (and value) destruction via EBR. After the queue is
// destroyed and the domain flushed, every constructed Tracked must be gone.
static void test_ms_lifetime() {
    tu::Tracked::reset();
    {
        mpmc::MSQueue<tu::Tracked> q;
        for (int i = 0; i < 100; ++i) q.try_push(tu::Tracked(i));
        tu::Tracked out(-1);
        for (int i = 0; i < 60; ++i) {
            REQUIRE(q.try_pop(out));
            CHECK(out.v == i);
        }
        // 40 live in the queue; popped nodes may still sit in EBR limbo.
    }
    mpmc::ebr::flush_all_unsafe();
    CHECK(tu::Tracked::alive.load() == 0);
    CHECK(tu::Tracked::ctors.load() == tu::Tracked::dtors.load());
}

// Vyukov-specific contracts: exact full detection at (power-of-two) capacity,
// capacity rounding, wrap across many laps, and precise value lifetime
// (destroyed at pop, unlike the MS queue's EBR-deferred destruction).
static void test_vyukov_bounded() {
    mpmc::VyukovQueue<int> q(64);
    CHECK(q.capacity() == 64);
    int out = -1;
    for (int i = 0; i < 64; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(999));  // full at exactly capacity
    REQUIRE(q.try_pop(out));
    CHECK(out == 0);
    CHECK(q.try_push(64));  // one slot freed, one accepted
    CHECK(!q.try_push(999));

    mpmc::VyukovQueue<int> q2(100);
    CHECK(q2.capacity() == 128);  // rounds up to power of two

    for (int lap = 0; lap < 50'000; ++lap) {  // many wraps of a small ring
        mpmc::VyukovQueue<int>* qq = &q2;
        CHECK(qq->try_push(lap));
        REQUIRE(qq->try_pop(out));
        CHECK(out == lap);
    }
}

template <class Q> static void test_bounded_capacity_validation() {
    Q one(1);
    CHECK(one.capacity() == 2);  // minimum effective ring remains two cells

    bool rejectedZero = false;
    try {
        Q q(0);
        (void)q;
    } catch (const std::invalid_argument&) {
        rejectedZero = true;
    }
    CHECK(rejectedZero);

    bool rejectedOverflow = false;
    try {
        Q q(std::numeric_limits<std::size_t>::max());
        (void)q;
    } catch (const std::length_error&) {
        rejectedOverflow = true;
    }
    CHECK(rejectedOverflow);
}

static void test_vyukov_lifetime() {
    tu::Tracked::reset();
    {
        mpmc::VyukovQueue<tu::Tracked> q(16);
        for (int i = 0; i < 10; ++i) CHECK(q.try_push(tu::Tracked(i)));
        tu::Tracked out(-1);
        for (int i = 0; i < 4; ++i) {
            REQUIRE(q.try_pop(out));
            CHECK(out.v == i);
        }
        CHECK(tu::Tracked::alive.load() == 6 + 1 /*out*/);  // precise: dead at pop
    }
    CHECK(tu::Tracked::alive.load() == 0);  // destructor drained the rest
    CHECK(tu::Tracked::ctors.load() == tu::Tracked::dtors.load());
}

// Destruction must not require a default constructor or assignment operator:
// neither operation is needed to tear down the live cells in the ring.
struct NonDefaultNonAssignable {
    static inline std::atomic<int> alive{0};

    int value;
    explicit NonDefaultNonAssignable(int v) : value(v) { ++alive; }
    NonDefaultNonAssignable(const NonDefaultNonAssignable& other) noexcept
        : value(other.value) {
        ++alive;
    }
    NonDefaultNonAssignable(NonDefaultNonAssignable&& other) noexcept : value(other.value) {
        ++alive;
    }
    NonDefaultNonAssignable& operator=(const NonDefaultNonAssignable&) = delete;
    NonDefaultNonAssignable& operator=(NonDefaultNonAssignable&&) = delete;
    ~NonDefaultNonAssignable() { --alive; }
};

static void test_vyukov_destructor_type_requirements() {
    NonDefaultNonAssignable::alive = 0;
    {
        NonDefaultNonAssignable a(1), b(2);
        {
            mpmc::VyukovQueue<NonDefaultNonAssignable> q(4);
            CHECK(q.try_push(a));
            CHECK(q.try_push(b));
            CHECK(NonDefaultNonAssignable::alive.load() == 4);
        }
        CHECK(NonDefaultNonAssignable::alive.load() == 2);
    }
    CHECK(NonDefaultNonAssignable::alive.load() == 0);
}

// FAA queue: blocking push/pop are safe single-threaded as long as we never
// exceed capacity or pop empty. FIFO, laps, and precise lifetime.
static void test_faa_basics() {
    mpmc::FAAQueue<int> q(64);
    CHECK(q.capacity() == 64);
    int out = -1;
    for (int i = 0; i < 64; ++i) q.push(i);  // exactly capacity: must not block
    for (int i = 0; i < 64; ++i) {
        q.pop(out);
        CHECK(out == i);
    }
    for (int lap = 0; lap < 50'000; ++lap) {
        q.push(lap);
        q.pop(out);
        CHECK(out == lap);
    }

    tu::Tracked::reset();
    {
        mpmc::FAAQueue<tu::Tracked> tq(16);
        for (int i = 0; i < 10; ++i) tq.push(tu::Tracked(i));
        tu::Tracked t(-1);
        for (int i = 0; i < 4; ++i) {
            tq.pop(t);
            CHECK(t.v == i);
        }
        CHECK(tu::Tracked::alive.load() == 6 + 1 /*t*/);
    }
    CHECK(tu::Tracked::alive.load() == 0);
    CHECK(tu::Tracked::ctors.load() == tu::Tracked::dtors.load());
}

static void test_ebr_registry_exhaustion() {
    // Reserve the main thread's slot, fill every remaining slot with a pinned
    // worker, then prove one extra registrant gets an exception rather than an
    // out-of-bounds slot index in release builds.
    mpmc::ebr::Guard mainGuard;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> workers;
    workers.reserve(mpmc::ebr::kMaxThreads - 1);
    for (std::size_t i = 1; i < mpmc::ebr::kMaxThreads; ++i) {
        workers.emplace_back([&] {
            mpmc::ebr::Guard guard;
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
    }
    while (ready.load(std::memory_order_acquire) != mpmc::ebr::kMaxThreads - 1)
        std::this_thread::yield();

    bool rejected = false;
    std::thread extra([&] {
        try {
            mpmc::ebr::Guard guard;
            (void)guard;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
    });
    extra.join();
    release.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    CHECK(rejected);
}

int main() {
    test_fifo_and_empty<mpmc::MSQueue<int>>();
    test_interleaved<mpmc::MSQueue<int>>();
    test_ms_lifetime();
    mpmc::ebr::set_reclaim_mode(1);  // F8 fix: lifetime must stay exact
    test_ms_lifetime();
    mpmc::ebr::set_reclaim_mode(0);

    test_fifo_and_empty<mpmc::VyukovQueue<int>>();
    test_interleaved<mpmc::VyukovQueue<int>>();
    test_fifo_and_empty<mpmc::VyukovQueue<int, true>>();  // backoff variant
    test_interleaved<mpmc::VyukovQueue<int, true>>();
    test_bounded_capacity_validation<mpmc::VyukovQueue<int>>();
    test_vyukov_bounded();
    test_vyukov_lifetime();
    test_vyukov_destructor_type_requirements();

    test_bounded_capacity_validation<mpmc::FAAQueue<int>>();
    test_faa_basics();
    test_ebr_registry_exhaustion();

    mpmc::ebr::flush_all_unsafe();
    return TEST_SUMMARY();
}
