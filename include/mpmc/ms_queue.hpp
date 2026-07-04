// ms_queue.hpp -- the Michael-Scott lock-free MPMC queue (PODC '96), the
// canonical node-based design, with epoch-based reclamation from ebr.hpp.
//
// Shape: a singly linked list with a permanent dummy at the head. head_ always
// points at the dummy; the first real element lives in head_->next. Producers
// CAS-append at tail_ (helping a lagging tail_ forward); consumers CAS head_
// one node forward and retire the old dummy.
//
// Progress: lock-free (a CAS loser retries, but some thread always advanced).
// Not wait-free -- under contention an individual thread can retry unboundedly.
//
// Properties to state precisely in the paper:
//   - UNBOUNDED: try_push never reports full; memory grows if consumers lag.
//     One heap allocation per push (the classic cost of node-based designs).
//   - T must be copy-constructible: a consumer copies the value out BEFORE its
//     head CAS (per the original algorithm), so several consumers may copy the
//     same value concurrently and only the CAS winner keeps it. Values are
//     destroyed with their node at reclamation time, not at pop time (deferred
//     destruction -- irrelevant for PODs, a documented caveat for RAII types).
#pragma once

#include "ebr.hpp"

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace mpmc {

template <class T> class MSQueue {
    static_assert(std::is_copy_constructible_v<T>,
                  "MSQueue requires copy-constructible T (consumers copy before CAS)");

    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool hasValue = false;  // immutable after construction; read at reclaim

        Node() = default;  // dummy
        explicit Node(const T& v) : hasValue(true) { ::new (storage) T(v); }
        ~Node() {
            if (hasValue) value()->~T();
        }
        T* value() { return std::launder(reinterpret_cast<T*>(storage)); }
    };

public:
    // `capacity` is accepted for interface parity with the bounded queues and
    // ignored -- the queue is unbounded.
    explicit MSQueue(std::size_t /*capacity*/ = 0) {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~MSQueue() {
        // Single-threaded by contract: walk and delete the whole list. Nodes
        // previously retired live in the EBR domain (flush separately).
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            Node* next = n->next.load(std::memory_order_relaxed);
            delete n;
            n = next;
        }
    }

    MSQueue(const MSQueue&) = delete;
    MSQueue& operator=(const MSQueue&) = delete;

    // Any thread. Always succeeds (unbounded); bool for interface parity.
    bool try_push(const T& v) {
        Node* n = new Node(v);
        ebr::Guard g;
        for (;;) {
            Node* t = tail_.load(std::memory_order_acquire);
            Node* next = t->next.load(std::memory_order_acquire);
            if (t != tail_.load(std::memory_order_acquire)) continue;  // stale snapshot
            if (next == nullptr) {
                // Tail really is last: try to link our node.
                if (t->next.compare_exchange_weak(next, n, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
                    // Swing tail; failure is fine (someone helped already).
                    tail_.compare_exchange_strong(t, n, std::memory_order_release,
                                                  std::memory_order_relaxed);
                    return true;
                }
            } else {
                // Tail lagging behind the real last node: help it forward.
                tail_.compare_exchange_strong(t, next, std::memory_order_release,
                                              std::memory_order_relaxed);
            }
        }
    }

    // Any thread. False when empty.
    bool try_pop(T& out) {
        ebr::Guard g;
        for (;;) {
            Node* h = head_.load(std::memory_order_acquire);
            Node* t = tail_.load(std::memory_order_acquire);
            Node* next = h->next.load(std::memory_order_acquire);
            if (h != head_.load(std::memory_order_acquire)) continue;  // stale snapshot
            if (h == t) {
                if (next == nullptr) return false;  // truly empty
                // Tail lagging: help, then retry.
                tail_.compare_exchange_strong(t, next, std::memory_order_release,
                                              std::memory_order_relaxed);
            } else {
                // Copy BEFORE the CAS (original MS): losers may copy too; only
                // the winner keeps its copy. Safe: nodes are never freed while
                // any thread is pinned (EBR), and nobody mutates the value.
                T copied = *next->value();
                if (head_.compare_exchange_weak(h, next, std::memory_order_release,
                                                std::memory_order_relaxed)) {
                    out = std::move(copied);
                    ebr::retire(h);  // old dummy; freed >= 2 epochs from now
                    return true;
                }
            }
        }
    }

private:
    alignas(128) std::atomic<Node*> head_;  // consumers CAS
    alignas(128) std::atomic<Node*> tail_;  // producers CAS (+ helpers)
    char pad_[128 - sizeof(std::atomic<Node*>)];
};

}  // namespace mpmc
