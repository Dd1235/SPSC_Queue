// Single-threaded contracts for the MPMC queues: FIFO order, empty behavior,
// interleaved push/pop, and object lifetime through epoch-based reclamation.
// Templated so each queue added in later phases reuses the same checks.
#include "mpmc/ms_queue.hpp"
#include "test_util.hpp"

#include <cstdint>

template <class Q> static void test_fifo_and_empty() {
    Q q(64);
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
    Q q(64);
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

int main() {
    test_fifo_and_empty<mpmc::MSQueue<int>>();
    test_interleaved<mpmc::MSQueue<int>>();
    test_ms_lifetime();
    mpmc::ebr::flush_all_unsafe();
    return TEST_SUMMARY();
}
