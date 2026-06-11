// Single-threaded correctness: API contract, FIFO order, full/empty edges,
// wraparound, object lifetime, and move-only / non-default-constructible types.
#include "spsc/spsc_queue.hpp"
#include "test_util.hpp"

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
    smoke_config<true, true>();
    smoke_config<false, true>();
    smoke_config<true, false>();
    smoke_config<false, false>();
    return TEST_SUMMARY();
}
