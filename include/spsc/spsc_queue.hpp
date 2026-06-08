// spsc_queue.hpp -- a lock-free, bounded single-producer/single-consumer queue.
//
// Header-only, C++17. One producer thread calls the push API; one consumer
// thread calls the pop API. No other sharing is allowed. Because each index
// has exactly one writer, we never need a compare-and-swap -- plain atomic
// loads/stores with acquire/release ordering are sufficient and the fast path
// is wait-free (bounded steps, no loops).
//
// The two extra template flags (Padded, Cached) default to the production
// configuration. They exist so the benchmarks can A/B the two engineering
// decisions -- cache-line padding and the cached-index optimization -- against
// a single source of truth rather than a forked copy of the code.
//
// See learn/ for the full derivation; the short version lives next to the code.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace spsc {

namespace detail {
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kStdInterference = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kStdInterference = 64;
#endif
}  // namespace detail

// std::hardware_destructive_interference_size is an ABI-frozen compile-time
// constant -- it is 64 on libc++ and libstdc++ even on Apple silicon, whose
// cache lines are actually 128 bytes. x86 also pulls the adjacent line on
// prefetch. Under-padding silently reintroduces false sharing, so we floor the
// value at 128: the safe "two hot fields never share a line" granularity.
inline constexpr std::size_t kCacheLineSize =
    detail::kStdInterference > 128 ? detail::kStdInterference : 128;

template <class T, bool Padded = true, bool Cached = true>
class SPSCQueue {
public:
    // `capacity` is the number of elements the queue can hold at once. We
    // allocate one extra slot internally so that "full" and "empty" are
    // distinguishable without a separate contended counter (see push/pop).
    explicit SPSCQueue(std::size_t capacity)
        : capacity_(capacity),
          ring_(capacity + 1),
          slots_(static_cast<T*>(::operator new(ring_ * sizeof(T),
                                                std::align_val_t{alignof(T)}))) {
        assert(capacity >= 1 && "capacity must be at least 1");
    }

    ~SPSCQueue() {
        // Destruction is single-threaded: destroy whatever is still live,
        // then release the raw storage.
        std::size_t r = readIdx_.load(std::memory_order_relaxed);
        const std::size_t w = writeIdx_.load(std::memory_order_relaxed);
        while (r != w) {
            slots_[r].~T();
            if (++r == ring_) r = 0;
        }
        ::operator delete(slots_, std::align_val_t{alignof(T)});
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // ---- producer API (one thread only) ----------------------------------

    // Construct an element in place at the write cursor. Returns false (without
    // blocking) if the queue is full.
    template <class... Args>
    bool try_emplace(Args&&... args) {
        static_assert(std::is_constructible_v<T, Args&&...>,
                      "T must be constructible from the given arguments");
        // I am the only writer of writeIdx_, so a relaxed load is enough --
        // I just need its current value, no ordering against anyone.
        const std::size_t w = writeIdx_.load(std::memory_order_relaxed);
        std::size_t next = w + 1;
        if (next == ring_) next = 0;

        if (full_at(next)) return false;

        new (&slots_[w]) T(std::forward<Args>(args)...);
        // Publish: the release pairs with the consumer's acquire so that the
        // element we just constructed is visible before the new index is.
        writeIdx_.store(next, std::memory_order_release);
        return true;
    }

    // Copy/move convenience wrappers over try_emplace. Both are wait-free on
    // the fast path; they return false instead of blocking when full so the
    // caller owns the back-pressure policy (spin, yield, drop, ...).
    bool try_push(const T& v) { return try_emplace(v); }
    bool try_push(T&& v) { return try_emplace(std::move(v)); }

    // ---- consumer API (one thread only) ----------------------------------

    // Move the front element into `out` and free its slot. Returns false
    // (without blocking) if the queue is empty.
    bool try_pop(T& out) {
        // I am the only writer of readIdx_ -> relaxed load of my own cursor.
        const std::size_t r = readIdx_.load(std::memory_order_relaxed);
        if (empty_at(r)) return false;

        out = std::move(slots_[r]);
        slots_[r].~T();
        std::size_t next = r + 1;
        if (next == ring_) next = 0;
        // Release so the producer's acquire sees the slot as free only after
        // we have finished reading and destroying it.
        readIdx_.store(next, std::memory_order_release);
        return true;
    }

    // Peek at the front element without removing it; returns nullptr if empty.
    // Pair with pop(). This avoids requiring T to be move-assignable and lets
    // the consumer inspect an element before deciding to consume it. The
    // returned pointer is valid until the next pop().
    T* front() noexcept {
        const std::size_t r = readIdx_.load(std::memory_order_relaxed);
        if (empty_at(r)) return nullptr;
        return &slots_[r];
    }

    // Remove the element previously observed via front(). Undefined to call on
    // an empty queue (front() must have returned non-null).
    void pop() noexcept {
        const std::size_t r = readIdx_.load(std::memory_order_relaxed);
        assert(r != writeIdx_.load(std::memory_order_acquire) && "pop() on empty queue");
        slots_[r].~T();
        std::size_t next = r + 1;
        if (next == ring_) next = 0;
        readIdx_.store(next, std::memory_order_release);
    }

    // ---- observers --------------------------------------------------------

    std::size_t capacity() const noexcept { return capacity_; }

    // Racy snapshot for metrics/debugging only. Both threads move their
    // indices concurrently, so the result can be stale the instant it returns
    // -- never branch on it for correctness, only for monitoring.
    std::size_t size_approx() const noexcept {
        const std::size_t w = writeIdx_.load(std::memory_order_relaxed);
        const std::size_t r = readIdx_.load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : (ring_ - r + w);
    }

private:
    // Is the slot at `next` the consumer's cursor (i.e. would advancing the
    // producer collide with it)? With Cached on, we trust our local copy and
    // only pay for the contended acquire load when it claims "full".
    bool full_at(std::size_t next) {
        if constexpr (Cached) {
            if (next == readIdxCache_) {
                readIdxCache_ = readIdx_.load(std::memory_order_acquire);
                return next == readIdxCache_;
            }
            return false;
        } else {
            return next == readIdx_.load(std::memory_order_acquire);
        }
    }

    // Symmetric to full_at: would consuming at `r` outrun the producer?
    bool empty_at(std::size_t r) {
        if constexpr (Cached) {
            if (r == writeIdxCache_) {
                writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
                return r == writeIdxCache_;
            }
            return false;
        } else {
            return r == writeIdx_.load(std::memory_order_acquire);
        }
    }

    // Align hot indices to a cache line only in the padded (production)
    // configuration; otherwise pack them so the benchmark can show the
    // false-sharing penalty.
    static constexpr std::size_t kIdxAlign =
        Padded ? kCacheLineSize : alignof(std::atomic<std::size_t>);
    static constexpr std::size_t kTrailingPad =
        Padded ? (kCacheLineSize - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)) : 1;

    std::size_t capacity_;  // elements the user can store
    std::size_t ring_;      // physical slot count = capacity_ + 1
    T* slots_;              // raw aligned storage for `ring_` objects of T

    // Producer-owned hot data on its own cache line; consumer-owned on the
    // next. This is the whole false-sharing story -- the two threads never
    // dirty the same line.
    alignas(kIdxAlign) std::atomic<std::size_t> writeIdx_{0};
    [[maybe_unused]] std::size_t readIdxCache_{0};   // producer's cached view of readIdx_

    alignas(kIdxAlign) std::atomic<std::size_t> readIdx_{0};
    [[maybe_unused]] std::size_t writeIdxCache_{0};  // consumer's cached view of writeIdx_

    // Keep neighbouring heap allocations off the consumer's line.
    char pad_[kTrailingPad]{};
};

}  // namespace spsc
