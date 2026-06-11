// Single-threaded correctness: API contract, FIFO order, full/empty edges,
// wraparound, object lifetime, and move-only / non-default-constructible types.
#include "spsc/spsc_queue.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <string>

using spsc::SPSCQueue;

static void test_basic_fifo_and_edges() {
    SPSCQueue<int> q(3);
    CHECK(q.capacity() == 3);
    CHECK(q.size_approx() == 0);
    CHECK(q.front() == nullptr);

    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(q.try_push(3));
    CHECK(!q.try_push(4));  // full: capacity is 3
    CHECK(q.size_approx() == 3);

    int out = 0;
    CHECK(q.try_pop(out) && out == 1);
    CHECK(q.try_pop(out) && out == 2);
    CHECK(q.try_pop(out) && out == 3);
    CHECK(!q.try_pop(out));  // empty
    CHECK(q.size_approx() == 0);
}

static void test_front_pop_peek() {
    SPSCQueue<int> q(4);
    q.try_push(10);
    q.try_push(20);
    int* f = q.front();
    REQUIRE(f != nullptr);
    CHECK(*f == 10);
    q.pop();
    f = q.front();
    REQUIRE(f != nullptr);
    CHECK(*f == 20);
    q.pop();
    CHECK(q.front() == nullptr);
}

// Push/pop a single element many times so the cursors wrap around the ring
// repeatedly -- catches off-by-one in the "+1 slot" wraparound logic.
static void test_wraparound() {
    SPSCQueue<int> q(2);
    for (int i = 0; i < 100000; ++i) {
        REQUIRE(q.try_push(i));
        int out = -1;
        REQUIRE(q.try_pop(out));
        CHECK(out == i);
    }
}

static void test_emplace_inplace() {
    SPSCQueue<std::string> q(2);
    CHECK(q.try_emplace(5, 'x'));  // string(5, 'x')
    std::string s;
    CHECK(q.try_pop(s) && s == "xxxxx");
}

// The queue holds raw storage and uses placement new + manual destruction.
// Verify it constructs and destroys exactly the right number of objects,
// including elements left behind when the queue is destroyed.
static void test_object_lifetime() {
    tu::Tracked::reset();
    {
        SPSCQueue<tu::Tracked> q(8);
        for (int i = 0; i < 5; ++i) CHECK(q.try_push(tu::Tracked(i)));
        tu::Tracked out(-1);
        CHECK(q.try_pop(out) && out.v == 0);
        CHECK(q.try_pop(out) && out.v == 1);
        // 3 elements (2,3,4) remain live inside the queue here.
        CHECK(tu::Tracked::alive.load() == 3 + 1 /*out*/);
    }
    // Queue destructor must have destroyed the 3 leftovers; only nothing lives.
    CHECK(tu::Tracked::alive.load() == 0);
    CHECK(tu::Tracked::ctors.load() == tu::Tracked::dtors.load());
}

static void test_move_only() {
    SPSCQueue<tu::MoveOnly> q(4);
    CHECK(q.try_push(tu::MoveOnly(7)));
    CHECK(q.try_emplace(8));
    tu::MoveOnly m(0);
    CHECK(q.try_pop(m) && m.v == 7);
    tu::MoveOnly* f = q.front();
    REQUIRE(f != nullptr);
    CHECK(f->v == 8);
    q.pop();
}

// Bulk APIs: exactness, partial transfers at the full/empty edges, wrap-split
// runs, and interop with the single-element ops.
static void test_bulk_basics() {
    SPSCQueue<int> q(5);
    int in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int out[8] = {0};

    CHECK(q.try_push_bulk(in, 8) == 5);  // partial: only capacity fits
    CHECK(q.try_push_bulk(in, 1) == 0);  // full
    CHECK(q.size_approx() == 5);

    CHECK(q.try_pop_bulk(out, 3) == 3);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3);

    // Next push run crosses the ring wrap boundary.
    CHECK(q.try_push_bulk(in, 3) == 3);
    CHECK(q.try_pop_bulk(out, 8) == 5);  // partial drain: only 5 present
    CHECK(out[0] == 4 && out[1] == 5 && out[2] == 1 && out[3] == 2 && out[4] == 3);
    CHECK(q.try_pop_bulk(out, 1) == 0);  // empty

    // Interop: bulk push, single pop, and vice versa.
    CHECK(q.try_push_bulk(in, 2) == 2);
    int v = 0;
    CHECK(q.try_pop(v) && v == 1);
    CHECK(q.try_push(9));
    CHECK(q.try_pop_bulk(out, 8) == 2);
    CHECK(out[0] == 2 && out[1] == 9);
}

// Repeated bulk push/pop with co-prime sizes so the runs continually shift
// against the wrap boundary -- catches segment-split off-by-ones.
static void test_bulk_wrap_sweep() {
    SPSCQueue<std::uint64_t> q(7);
    std::uint64_t next_in = 0, next_out = 0;
    std::uint64_t buf[4];
    for (int iter = 0; iter < 10000; ++iter) {
        std::uint64_t in[3];
        for (int i = 0; i < 3; ++i) in[i] = next_in + i;
        next_in += q.try_push_bulk(in, 3);
        std::size_t got = q.try_pop_bulk(buf, 4);
        for (std::size_t i = 0; i < got; ++i) {
            CHECK(buf[i] == next_out);
            ++next_out;
        }
    }
    CHECK(next_in >= next_out);
}

// Exercise every compile-time configuration through the same smoke path so a
// broken if-constexpr branch is caught.
template <bool Padded, bool Cached> static void smoke_config() {
    SPSCQueue<int, Padded, Cached> q(4);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(q.try_push(i));
        int out = -1;
        REQUIRE(q.try_pop(out));
        CHECK(out == i);
    }
}

int main() {
    test_basic_fifo_and_edges();
    test_front_pop_peek();
    test_wraparound();
    test_emplace_inplace();
    test_object_lifetime();
    test_move_only();
    test_bulk_basics();
    test_bulk_wrap_sweep();
    smoke_config<true, true>();
    smoke_config<false, true>();
    smoke_config<true, false>();
    smoke_config<false, false>();
    return TEST_SUMMARY();
}
