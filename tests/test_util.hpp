// Tiny zero-dependency test helpers shared by the test executables.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace tu {

inline int g_failures = 0;

}  // namespace tu

// Non-fatal check: records a failure and keeps going.
#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);    \
            ++::tu::g_failures;                                                               \
        }                                                                                     \
    } while (0)

// Fatal check: aborts the test immediately (used when continuing is pointless).
#define REQUIRE(cond)                                                                         \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

// Return 0 if all CHECKs passed, 1 otherwise. Call at the end of main().
#define TEST_SUMMARY()                                                                        \
    ((::tu::g_failures == 0)                                                                  \
         ? (std::printf("OK\n"), 0)                                                           \
         : (std::fprintf(stderr, "%d failure(s)\n", ::tu::g_failures), 1))

namespace tu {

// A value type that counts live instances so tests can prove the queue
// constructs/destroys exactly the right objects (object-lifetime correctness).
struct Tracked {
    static inline std::atomic<long> alive{0};
    static inline std::atomic<long> ctors{0};
    static inline std::atomic<long> dtors{0};

    int v;

    explicit Tracked(int x = 0) : v(x) {
        ++alive;
        ++ctors;
    }
    Tracked(const Tracked& o) : v(o.v) {
        ++alive;
        ++ctors;
    }
    Tracked(Tracked&& o) noexcept : v(o.v) {
        o.v = -1;
        ++alive;
        ++ctors;
    }
    Tracked& operator=(const Tracked& o) {
        v = o.v;
        return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept {
        v = o.v;
        o.v = -1;
        return *this;
    }
    ~Tracked() {
        --alive;
        ++dtors;
    }

    static void reset() {
        alive = 0;
        ctors = 0;
        dtors = 0;
    }
};

// A move-only payload to prove the queue never requires copyability.
struct MoveOnly {
    int v;
    explicit MoveOnly(int x = 0) : v(x) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
};

}  // namespace tu
