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
// Progress: lock-free -- a CAS loser re-reads and retries, but every failed
// CAS means some other thread advanced. NOT wait-free (unbounded individual
// retries under contention). Bounded, array-backed -> no memory reclamation.
//
// Notes for the study:
//   - Capacity is rounded UP to a power of two (mask indexing); report the
//     effective capacity, not the requested one.
//   - Unlike our SPSC queue there is no spare slot and no index caching; the
//     per-cell sequence plays both roles.
//   - Values are destroyed at pop time (precise lifetime -- contrast with the
//     MS queue's EBR-deferred destruction).
//
// CONTROL VARIANT (F1-attribution): Backoff=true inserts the SAME spin-then-
// yield policy the FAA queue uses (1024 spins, then yield) into every retry
// path -- CAS failure and behind-the-cursor re-reads -- keeping the algorithm
// otherwise identical. Vyukov vs Vyukov<Backoff> isolates spin policy;
// FAAQueue<CasClaim> isolates the claim primitive. Together they decompose
// the oversubscription inversion.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
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

    static std::size_t next_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
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
        : n_(next_pow2(capacity < 2 ? 2 : capacity)), mask_(n_ - 1), cells_(new Cell[n_]) {
        for (std::size_t i = 0; i < n_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    ~VyukovQueue() {
        // Single-threaded by contract: destroy whatever is still live.
        T out;
        if constexpr (std::is_move_assignable_v<T>) {
            while (try_pop(out)) {
            }
        }
        delete[] cells_;
    }

    VyukovQueue(const VyukovQueue&) = delete;
    VyukovQueue& operator=(const VyukovQueue&) = delete;

    std::size_t capacity() const noexcept { return n_; }  // effective (rounded up)

    bool try_push(const T& v) { return emplace_impl(v); }
    bool try_push(T&& v) { return emplace_impl(std::move(v)); }

    // Any thread. False when empty.
    bool try_pop(T& out) {
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
                be.lost_a_round();
            } else if (dif < 0) {
                return false;  // cell not yet produced -> queue empty at our pos
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);  // we're behind
                be.lost_a_round();
            }
        }
    }

private:
    template <class U> bool emplace_impl(U&& v) {
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
                be.lost_a_round();  // CAS lost
            } else if (dif < 0) {
                return false;  // cell still holds last lap's data -> full
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
                be.lost_a_round();
            }
        }
    }

    std::size_t n_;
    std::size_t mask_;
    Cell* cells_;
    alignas(128) std::atomic<std::size_t> enqueuePos_;
    alignas(128) std::atomic<std::size_t> dequeuePos_;
    char pad_[128 - sizeof(std::atomic<std::size_t>)];
};

}  // namespace mpmc
