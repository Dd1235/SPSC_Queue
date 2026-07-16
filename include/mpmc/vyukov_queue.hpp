// vyukov_queue.hpp -- Dmitry Vyukov's bounded MPMC queue, the canonical
// array-based multi-producer/multi-consumer design (1024cores.net).
//
// Shape: a power-of-two ring of cells, each carrying an atomic *sequence
// number* that encodes whose turn the cell is on. Two free-running cursors
// (enqueuePos_, dequeuePos_) are claimed by CAS. The per-cell sequence is what
// coordinates producers and consumers -- and what defeats ABA: a cell at the
// same array index in a later lap carries a different sequence, so a stale
// claimant can never mistake it for its own turn.
//
// Cell protocol (n = capacity, pos free-running):
//   seq == pos           -> cell free for the producer claiming `pos`
//   seq == pos + 1       -> cell holds data for the consumer claiming `pos`
//   producer: claim pos by CAS, construct, then seq.store(pos + 1, release)
//   consumer: claim pos by CAS, consume,   then seq.store(pos + n, release)
//
// Progress: mutex-free and reservation-based, but NOT formally lock-free. A
// thread advances a shared cursor before it publishes/frees the corresponding
// cell; if it is preempted in between, successors can eventually be unable to
// make progress. CAS losers also have unbounded individual retries. Bounded,
// array-backed -> no memory reclamation.
//
// Notes for the study:
//   - Capacity is rounded UP to a power of two (mask indexing); report the
//     effective capacity, not the requested one. Zero and unrepresentable
//     capacities are rejected before allocation.
//   - Unlike our SPSC queue there is no spare slot and no index caching; the
//     per-cell sequence plays both roles.
//   - Values are destroyed at pop time (precise lifetime -- contrast with the
//     MS queue's EBR-deferred destruction).
//   - The cursor is reserved before the payload operation, so construction on
//     push and move assignment on pop must not throw. The relevant API member
//     enforces that requirement when instantiated.
//
// CONTROL VARIANT (F1-attribution): Backoff=true inserts the SAME spin-then-
// yield policy the FAA queue uses (1024 spins, then yield) into every retry
// path -- CAS failure and behind-the-cursor re-reads -- keeping the algorithm
// otherwise identical. Vyukov vs Vyukov<Backoff> isolates spin policy;
// FAAQueue<CasClaim> isolates the ticket parent's claim primitive. They bound
// two within-parent explanations but do not isolate the broader cross-family
// ticket/turn versus cell-protocol difference.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace mpmc {

template <class T, bool Backoff = false> class VyukovQueue {
    struct Cell {
        std::atomic<std::size_t> seq;
        alignas(T) unsigned char storage[sizeof(T)];
        T* value() { return std::launder(reinterpret_cast<T*>(storage)); }
    };

    static std::size_t checked_capacity(std::size_t v) {
        if (v == 0) throw std::invalid_argument("VyukovQueue capacity must be positive");
        if (v < 2) v = 2;

        constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
        std::size_t p = 1;
        while (p < v) {
            if (p > max / 2) throw std::length_error("VyukovQueue capacity is too large");
            p <<= 1;
        }
        if (p > max / sizeof(Cell))
            throw std::length_error("VyukovQueue capacity is too large");
        return p;
    }

    // Mirror of FAAQueue::wait_for's politeness, applied per retry iteration.
    struct Politeness {
        int spins = 0;
        void lost_a_round() {
            if constexpr (Backoff) {
                if (++spins >= 1024) {
                    std::this_thread::yield();
                    spins = 0;
                }
            }
        }
    };

public:
    explicit VyukovQueue(std::size_t capacity)
        : n_(checked_capacity(capacity)), mask_(n_ - 1), cells_(new Cell[n_]) {
        for (std::size_t i = 0; i < n_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    ~VyukovQueue() {
        // Single-threaded by contract: successful, completed enqueues occupy
        // exactly [dequeuePos_, enqueuePos_). Destroy that range directly so
        // teardown does not impose unrelated default-construction or move-
        // assignment requirements on T.
        std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        const std::size_t end = enqueuePos_.load(std::memory_order_relaxed);
        while (pos != end) {
            cells_[pos & mask_].value()->~T();
            ++pos;
        }
        delete[] cells_;
    }

    VyukovQueue(const VyukovQueue&) = delete;
    VyukovQueue& operator=(const VyukovQueue&) = delete;

    std::size_t capacity() const noexcept { return n_; }  // effective (rounded up)

    // Mechanism counters (F1 attribution): compiled only with SPSC_QUEUE_STATS.
    // primary = cursor CAS failures; secondary = behind-the-cursor re-reads.
    std::uint64_t stat_retries() const noexcept {
#ifdef SPSC_QUEUE_STATS
        return statRetries_.load(std::memory_order_relaxed);
#else
        return 0;
#endif
    }
    std::uint64_t stat_secondary() const noexcept {
#ifdef SPSC_QUEUE_STATS
        return statSecondary_.load(std::memory_order_relaxed);
#else
        return 0;
#endif
    }

    bool try_push(const T& v) { return emplace_impl(v); }
    bool try_push(T&& v) { return emplace_impl(std::move(v)); }

    // Any thread. False when empty.
    bool try_pop(T& out) {
        static_assert(std::is_nothrow_move_assignable_v<T>,
                      "VyukovQueue::try_pop requires nothrow move assignment");
        std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        Politeness be;
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const std::intptr_t dif =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (dif == 0) {
                // Data is ready for the consumer of `pos`: try to claim it.
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed)) {
                    out = std::move(*c.value());
                    c.value()->~T();
                    // Free the cell for the producer one lap ahead.
                    c.seq.store(pos + n_, std::memory_order_release);
                    return true;
                }
                // CAS failed: `pos` was refreshed by compare_exchange; retry.
                count_retry();
                be.lost_a_round();
            } else if (dif < 0) {
                return false;  // cell not yet produced -> queue empty at our pos
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);  // we're behind
                count_behind();
                be.lost_a_round();
            }
        }
    }

private:
    template <class U> bool emplace_impl(U&& v) {
        static_assert(std::is_nothrow_constructible_v<T, U&&>,
                      "VyukovQueue::try_push requires nothrow construction");
        std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        Politeness be;
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const std::intptr_t dif =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                // Cell free for the producer of `pos`: try to claim it.
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed)) {
                    ::new (c.storage) T(std::forward<U>(v));
                    // Publish to the consumer of `pos`.
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
                count_retry();
                be.lost_a_round();  // CAS lost
            } else if (dif < 0) {
                return false;  // cell still holds last lap's data -> full
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
                count_behind();
                be.lost_a_round();
            }
        }
    }

    void count_retry() noexcept {
#ifdef SPSC_QUEUE_STATS
        statRetries_.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    void count_behind() noexcept {
#ifdef SPSC_QUEUE_STATS
        statSecondary_.fetch_add(1, std::memory_order_relaxed);
#endif
    }
#ifdef SPSC_QUEUE_STATS
    std::atomic<std::uint64_t> statRetries_{0};
    std::atomic<std::uint64_t> statSecondary_{0};
#endif

    std::size_t n_;
    std::size_t mask_;
    Cell* cells_;
    alignas(128) std::atomic<std::size_t> enqueuePos_;
    alignas(128) std::atomic<std::size_t> dequeuePos_;
    char pad_[128 - sizeof(std::atomic<std::size_t>)];
};

}  // namespace mpmc
