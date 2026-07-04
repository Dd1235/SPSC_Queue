// faa_queue.hpp -- a fetch-add "ticket" bounded MPMC queue (disruptor-style
// claiming): every producer/consumer atomically takes a unique ticket with one
// fetch_add, then waits for its slot's turn counter.
//
// Cell protocol (n = capacity, ticket t, round r = t / n):
//   turn == 2r      -> slot ready for the WRITER holding ticket t
//   turn == 2r + 1  -> slot ready for the READER holding ticket t
//   writer: wait turn==2r,  construct, turn.store(2r + 1, release)
//   reader: wait turn==2r+1, consume,  turn.store(2r + 2, release)
//
// The progress-guarantee fine print -- this queue exists in the study to make
// exactly this contrast measurable:
//   - The CLAIM is wait-free: one fetch_add, no retry loop, no CAS.
//   - COMPLETION is blocking-on-slot: tickets are irrevocable, so a claimant
//     preempted between claim and publish blocks the thread whose ticket maps
//     to the same slot next (and, transitively, FIFO successors). Under
//     dedicated cores this is invisible; under oversubscription it is the
//     mechanism we expect to blow up tail latency (paper hypothesis H1).
//
// Because tickets cannot be un-taken, there is no honest try_push/try_pop:
// push() blocks while full, pop() blocks while empty (spin, then yield).
// Harness/tests terminate consumers with poison-pill values, never by racing a
// counter. Bounded, array-backed -> no reclamation problem. Capacity rounds up
// to a power of two.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <utility>

namespace mpmc {

template <class T> class FAAQueue {
    struct Cell {
        std::atomic<std::size_t> turn;
        alignas(T) unsigned char storage[sizeof(T)];
        T* value() { return std::launder(reinterpret_cast<T*>(storage)); }
    };

    static std::size_t next_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    static void wait_for(std::atomic<std::size_t>& turn, std::size_t want) {
        int spins = 0;
        while (turn.load(std::memory_order_acquire) != want) {
            if (++spins >= 1024) {  // be polite once it's clearly not imminent
                std::this_thread::yield();
                spins = 0;
            }
        }
    }

public:
    explicit FAAQueue(std::size_t capacity)
        : n_(next_pow2(capacity < 2 ? 2 : capacity)), mask_(n_ - 1), shift_(log2_(n_)),
          cells_(new Cell[n_]) {
        for (std::size_t i = 0; i < n_; ++i)
            cells_[i].turn.store(0, std::memory_order_relaxed);
        pushTicket_.store(0, std::memory_order_relaxed);
        popTicket_.store(0, std::memory_order_relaxed);
    }

    ~FAAQueue() {
        // Single-threaded by contract: live entries are tickets [pop, push).
        const std::size_t head = popTicket_.load(std::memory_order_relaxed);
        const std::size_t tail = pushTicket_.load(std::memory_order_relaxed);
        for (std::size_t t = head; t != tail; ++t) cells_[t & mask_].value()->~T();
        delete[] cells_;
    }

    FAAQueue(const FAAQueue&) = delete;
    FAAQueue& operator=(const FAAQueue&) = delete;

    std::size_t capacity() const noexcept { return n_; }

    // Any thread. Blocks (spin+yield) while the queue is full.
    void push(const T& v) { emplace_impl(v); }
    void push(T&& v) { emplace_impl(std::move(v)); }

    // Any thread. Blocks (spin+yield) while the queue is empty.
    void pop(T& out) {
        const std::size_t t = popTicket_.fetch_add(1, std::memory_order_relaxed);
        Cell& c = cells_[t & mask_];
        const std::size_t round = t >> shift_;
        wait_for(c.turn, 2 * round + 1);  // reader's turn
        out = std::move(*c.value());
        c.value()->~T();
        c.turn.store(2 * round + 2, std::memory_order_release);  // writer, next lap
    }

private:
    template <class U> void emplace_impl(U&& v) {
        const std::size_t t = pushTicket_.fetch_add(1, std::memory_order_relaxed);
        Cell& c = cells_[t & mask_];
        const std::size_t round = t >> shift_;
        wait_for(c.turn, 2 * round);  // writer's turn
        ::new (c.storage) T(std::forward<U>(v));
        c.turn.store(2 * round + 1, std::memory_order_release);  // reader's turn
    }

    static std::size_t log2_(std::size_t p) {
        std::size_t s = 0;
        while ((std::size_t(1) << s) < p) ++s;
        return s;
    }

    std::size_t n_;
    std::size_t mask_;
    std::size_t shift_;
    Cell* cells_;
    alignas(128) std::atomic<std::size_t> pushTicket_;
    alignas(128) std::atomic<std::size_t> popTicket_;
    char pad_[128 - sizeof(std::atomic<std::size_t>)];
};

}  // namespace mpmc
