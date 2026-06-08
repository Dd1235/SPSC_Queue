// spsc_queue.hpp -- a lock-free, bounded single-producer/single-consumer queue.
//
// Header-only, C++17. One producer thread calls the push API; one consumer
// thread calls the pop API. No other sharing is allowed. Because each index
// has exactly one writer, we never need a compare-and-swap -- plain atomic
// loads/stores with acquire/release ordering are sufficient and the fast path
// is wait-free (bounded steps, no loops).
//
// See learn/ for the full derivation; the short version lives next to the code.
#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace spsc {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
// Apple M2 uses 128-byte cache lines; x86 pulls the adjacent line on prefetch,
// so 128 is the safe "don't let two hot fields share a line" granularity.
inline constexpr std::size_t kCacheLineSize = 128;
#endif

template <class T>
class SPSCQueue {
public:
    // `capacity` is the number of elements the queue can hold at once. We
    // allocate one extra slot internally so that "full" and "empty" are
    // distinguishable without a separate contended counter (see push/pop).
    explicit SPSCQueue(std::size_t capacity)
        : capacity_(capacity),
          ring_(capacity + 1),
          slots_(static_cast<T*>(::operator new(ring_ * sizeof(T),
                                                std::align_val_t{alignof(T)}))) {}

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
    std::size_t capacity_;  // elements the user can store
    std::size_t ring_;      // physical slot count = capacity_ + 1
    T* slots_;              // raw aligned storage for `ring_` objects of T

    // Producer-owned hot data on its own cache line; consumer-owned on the
    // next. This is the whole false-sharing story -- the two threads never
    // dirty the same line.
    alignas(kCacheLineSize) std::atomic<std::size_t> writeIdx_{0};
    std::size_t readIdxCache_{0};   // producer's cached view of readIdx_

    alignas(kCacheLineSize) std::atomic<std::size_t> readIdx_{0};
    std::size_t writeIdxCache_{0};  // consumer's cached view of writeIdx_

    // Keep neighbouring heap allocations off the consumer's line.
    char pad_[kCacheLineSize - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)]{};
};

}  // namespace spsc
