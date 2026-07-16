// ebr.hpp -- minimal epoch-based reclamation (EBR) for node-based lock-free
// structures (here: the Michael-Scott queue).
//
// The problem it solves: in a node-based lock-free queue, a thread that pops a
// node cannot immediately `delete` it -- another thread may hold a raw pointer
// to that node (read before the pop's CAS won). EBR defers the free until no
// thread can still hold such a pointer.
//
// How it works (the classic 3-epoch scheme):
//   - A global epoch counter advances 0,1,2,...
//   - Every operation on the data structure pins the current epoch for its
//     duration (Guard RAII). Pinned threads block epoch advancement.
//   - retire(p) stamps the node with the current epoch instead of freeing it.
//   - A node retired in epoch e is freed once the global epoch reaches e+2:
//     advancing e->e+1 requires every pinned thread to be at e, so by e+2 no
//     thread that could have seen the node is still inside an operation.
//
// Deliberate simplifications (documented for the paper's methodology notes):
//   - Fixed-size thread registry (kMaxThreads slots), claimed on first use per
//     thread, released at thread exit.
//   - Per-thread retire lists are flat vectors scanned during maintenance; a
//     threshold batches the scans. Simplicity over peak reclamation speed --
//     maintenance is still part of the measured dequeue/makespan path whenever
//     a retiring thread reaches the threshold.
//   - A stalled/preempted pinned thread delays reclamation (memory grows) but
//     never corrupts. This is the well-known EBR tradeoff vs hazard pointers.
//   - Threads that exit with unreclaimed nodes hand them to a mutex-protected
//     orphan list (cold path only).
//
// REMEDIATION STUDY (paper finding F8) -- three runtime-selectable modes so
// one binary A/B/Cs them honestly (set_reclaim_mode, default kLegacy):
//
//   kLegacy  -- the study's original code: one advance attempt per maintenance
//               and an O(limbo) compaction pass to free expired entries.
//               Measured pathology: 742 MB peak RSS at 1P:7C in 2 s.
//   kPrefix  -- the successful measured variant at the consumer-heavy shapes:
//               per-thread limbo epochs are nondecreasing, so expired entries
//               form a prefix; free until the first survivor and erase once.
//               With a vector, an erase shifts survivors, so a freeing pass
//               remains O(limbo), but a no-expiration pass is O(1) instead of
//               rewriting the whole list. The instrumented replication
//               (matrix h3s; counters below) measured the mechanism: at 1:7
//               the fix cuts maintenance from 79.6 to 21.3 us/pass while the
//               advance success rate stays <0.3% in BOTH modes -- it wins by
//               making stuck-epoch passes cheap, not by unsticking the epoch.
//               At 1:1 the counters exonerate reclamation entirely (>=99.8%
//               advance success, ~4.6 us passes, tiny limbo) -- the residual
//               ~550 MB there is producer-outruns-consumer live-queue growth,
//               not reclamation lag.
//   kRetry   -- the "obvious" heavier remediation (defer maintenance past the
//               unpin + retry advancement until two successes + amortized
//               pin-path advances). The measured bundle is slower and uses
//               more memory at consumer-heavy shapes. The counters partially
//               de-bundle it: its advancement probes SUCCEED (69% advance
//               success at 1:7, smallest limbo peaks) and it still loses, so
//               the harm is pass cost (168 us), not failed advancement; the
//               split between probe cost and registry-scan coherence traffic
//               stays unmeasured.
//
// MECHANISM INSTRUMENTATION (SPSC_QUEUE_STATS builds only): peak limbo size,
// maintenance pass count/duration, and epoch-advance attempt/success counts.
// Purpose: measure the scan-cost feedback loop directly instead of inferring
// it ("consistent with") from RSS and throughput alone. Compiled only into
// the stats twin binary so the headline numbers stay unperturbed.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef SPSC_QUEUE_STATS
#include <chrono>
#endif

namespace mpmc::ebr {

inline constexpr std::size_t kMaxThreads = 32;
inline constexpr std::size_t kRetireThreshold = 64;   // maintenance cadence
inline constexpr std::size_t kLimboHighWater = 4096;  // kRetry pressure trigger
inline constexpr std::uint64_t kIdle = ~0ull;

enum ReclaimMode : int { kLegacy = 0, kPrefix = 1, kRetry = 2 };

namespace detail {

struct alignas(128) Slot {
    std::atomic<std::uint64_t> epoch{kIdle};  // kIdle = not inside an operation
    std::atomic<bool> used{false};
};

struct Retired {
    void* ptr;
    void (*deleter)(void*);
    std::uint64_t epoch;
};

struct Domain {
    std::atomic<std::uint64_t> globalEpoch{0};
    std::atomic<int> reclaimMode{kLegacy};  // see header: kLegacy/kPrefix/kRetry
    Slot slots[kMaxThreads];
    std::mutex orphanMu;           // cold path: thread exit + orphan drain
    std::vector<Retired> orphans;  // guarded by orphanMu

#ifdef SPSC_QUEUE_STATS
    // F8 mechanism counters (see header note). Relaxed: diagnostic only.
    std::atomic<std::uint64_t> statLimboPeak{0};
    std::atomic<std::uint64_t> statMaintPasses{0};
    std::atomic<std::uint64_t> statMaintNs{0};
    std::atomic<std::uint64_t> statAdvanceAttempts{0};
    std::atomic<std::uint64_t> statAdvanceSuccesses{0};

    void stat_limbo_peak(std::uint64_t v) {
        std::uint64_t cur = statLimboPeak.load(std::memory_order_relaxed);
        while (cur < v && !statLimboPeak.compare_exchange_weak(cur, v,
                                                               std::memory_order_relaxed)) {
        }
    }
#endif

    static Domain& instance() {
        static Domain d;
        return d;
    }

    // Advance the global epoch iff every pinned thread is on the current one.
    // Loads are seq_cst to order against the seq_cst pin stores in Guard.
    bool try_advance() {
#ifdef SPSC_QUEUE_STATS
        statAdvanceAttempts.fetch_add(1, std::memory_order_relaxed);
#endif
        const std::uint64_t e = globalEpoch.load(std::memory_order_seq_cst);
        for (std::size_t i = 0; i < kMaxThreads; ++i) {
            const std::uint64_t pinned = slots[i].epoch.load(std::memory_order_seq_cst);
            if (pinned != kIdle && pinned != e) return false;  // straggler
        }
        std::uint64_t expected = e;
        const bool ok = globalEpoch.compare_exchange_strong(
            expected, e + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
#ifdef SPSC_QUEUE_STATS
        if (ok) statAdvanceSuccesses.fetch_add(1, std::memory_order_relaxed);
#endif
        return ok;
    }
};

// Per-thread registration: claims a Slot on first use, releases at thread exit.
struct ThreadCtx {
    Domain& dom = Domain::instance();
    std::size_t slotIdx = kMaxThreads;
    int pinDepth = 0;
    std::uint64_t pinCount = 0;
    bool maintainPending = false;  // remediation: run maintain() after unpin
    std::vector<Retired> limbo;
    std::size_t sinceMaintain = 0;

    ThreadCtx() {
        for (std::size_t i = 0; i < kMaxThreads; ++i) {
            bool expected = false;
            if (dom.slots[i].used.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
                slotIdx = i;
                return;
            }
        }
        throw std::runtime_error("ebr: thread registry exhausted (raise kMaxThreads)");
    }

    ~ThreadCtx() {
        // Whatever we could not yet free is handed to the orphan list; some
        // later thread's maintenance (or flush_all_unsafe) frees it.
        if (!limbo.empty()) {
            std::lock_guard<std::mutex> g(dom.orphanMu);
            for (const Retired& r : limbo) dom.orphans.push_back(r);
        }
        if (slotIdx < kMaxThreads) {
            dom.slots[slotIdx].epoch.store(kIdle, std::memory_order_release);
            dom.slots[slotIdx].used.store(false, std::memory_order_release);
        }
    }

    // kLegacy: the study's original O(limbo) compaction free.
    void free_expired_legacy(std::uint64_t e) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < limbo.size(); ++i) {
            if (limbo[i].epoch + 2 <= e) {
                limbo[i].deleter(limbo[i].ptr);
            } else {
                limbo[kept++] = limbo[i];
            }
        }
        limbo.resize(kept);
    }

    // kPrefix/kRetry: limbo epochs are nondecreasing (stamped at retire time),
    // so expired entries form a PREFIX. When none are expired this examines one
    // entry and avoids legacy's full scan/rewrite. When entries are freed,
    // vector::erase still shifts every survivor: O(freed + survivors), not just
    // O(freed).
    void free_expired_prefix(std::uint64_t e) {
        std::size_t i = 0;
        while (i < limbo.size() && limbo[i].epoch + 2 <= e) {
            limbo[i].deleter(limbo[i].ptr);
            ++i;
        }
        if (i > 0) limbo.erase(limbo.begin(), limbo.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void free_expired(std::uint64_t e) {
        if (dom.reclaimMode.load(std::memory_order_relaxed) == kLegacy)
            free_expired_legacy(e);
        else
            free_expired_prefix(e);
    }

    void maintain() {
#ifdef SPSC_QUEUE_STATS
        const auto statT0 = std::chrono::steady_clock::now();
#endif
        const int mode = dom.reclaimMode.load(std::memory_order_relaxed);
        if (mode == kRetry && limbo.size() > kLimboHighWater) {
            // The heavy variant (kept as a measured negative result): retry
            // the advance until two succeed so this batch becomes freeable.
            int advanced = 0;
            for (int i = 0; i < 64 && advanced < 2; ++i) {
                if (dom.try_advance()) ++advanced;
                for (volatile int spin = 0; spin < 64; ++spin) {
                }
            }
        } else {
            dom.try_advance();  // one attempt (kLegacy and kPrefix)
        }
        free_expired(dom.globalEpoch.load(std::memory_order_acquire));
        // Opportunistically drain orphans without ever blocking the hot path.
        const std::uint64_t e = dom.globalEpoch.load(std::memory_order_acquire);
        if (dom.orphanMu.try_lock()) {
            std::lock_guard<std::mutex> g(dom.orphanMu, std::adopt_lock);
            std::size_t okept = 0;
            for (std::size_t i = 0; i < dom.orphans.size(); ++i) {
                if (dom.orphans[i].epoch + 2 <= e) {
                    dom.orphans[i].deleter(dom.orphans[i].ptr);
                } else {
                    dom.orphans[okept++] = dom.orphans[i];
                }
            }
            dom.orphans.resize(okept);
        }
#ifdef SPSC_QUEUE_STATS
        dom.statMaintPasses.fetch_add(1, std::memory_order_relaxed);
        dom.statMaintNs.fetch_add(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - statT0)
                                           .count()),
            std::memory_order_relaxed);
#endif
    }
};

inline ThreadCtx& tls() {
    thread_local ThreadCtx ctx;
    return ctx;
}

}  // namespace detail

// Pins the current epoch for the duration of one data-structure operation.
// Nested guards are supported (only the outermost pins/unpins).
class Guard {
public:
    Guard() : ctx_(detail::tls()) {
        if (ctx_.pinDepth++ == 0) {
            // seq_cst pin: orders against try_advance's seq_cst scan so a
            // reclaimer either sees our pin or we see the newer epoch. Pinning
            // an epoch that just advanced is safe -- merely conservative.
            const std::uint64_t e = ctx_.dom.globalEpoch.load(std::memory_order_seq_cst);
            ctx_.dom.slots[ctx_.slotIdx].epoch.store(e, std::memory_order_seq_cst);
            // Remediation (F8), amortized half: an occasional advance attempt
            // from the pin path keeps the epoch moving even when no thread is
            // retiring often enough to hit its maintenance threshold.
            if (ctx_.dom.reclaimMode.load(std::memory_order_relaxed) == kRetry &&
                (++ctx_.pinCount & 255u) == 0)
                ctx_.dom.try_advance();
        }
    }
    ~Guard() {
        if (--ctx_.pinDepth == 0) {
            ctx_.dom.slots[ctx_.slotIdx].epoch.store(kIdle, std::memory_order_release);
            if (ctx_.maintainPending) {
                // Remediation (F8): maintain UNPINNED, so try_advance is not
                // blocked by our own epoch pin and can move multiple steps.
                ctx_.maintainPending = false;
                ctx_.maintain();
            }
        }
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    detail::ThreadCtx& ctx_;
};

// Select the reclamation mode for the F8 experiment (see header comment):
// 0 = kLegacy, 1 = kPrefix, 2 = kRetry. Their observed trade-offs are reported
// by the experiment rather than encoded as correctness labels here. Set before
// threads start operating.
inline void set_reclaim_mode(int mode) {
    detail::Domain::instance().reclaimMode.store(mode, std::memory_order_relaxed);
}

// Defer destruction of `p` until it is provably unreachable (epoch + 2).
template <class T> inline void retire(T* p) {
    detail::ThreadCtx& ctx = detail::tls();
    const std::uint64_t e = ctx.dom.globalEpoch.load(std::memory_order_acquire);
    ctx.limbo.push_back({p, [](void* q) { delete static_cast<T*>(q); }, e});
#ifdef SPSC_QUEUE_STATS
    ctx.dom.stat_limbo_peak(ctx.limbo.size());
#endif
    if (++ctx.sinceMaintain >= kRetireThreshold) {
        ctx.sinceMaintain = 0;
        if (ctx.dom.reclaimMode.load(std::memory_order_relaxed) == kRetry) {
            ctx.maintainPending = true;  // kRetry: defer past our unpin
        } else {
            ctx.maintain();  // kLegacy/kPrefix: maintain inline
        }
    }
}

#ifdef SPSC_QUEUE_STATS
// F8 mechanism snapshot (stats builds only). Peak PER-THREAD limbo size,
// maintenance pass count and total duration, and advance attempt/success
// counts, accumulated since process start (or the last reset). A fresh
// process per trial means the benchmark never needs the reset; tests do.
struct MechStats {
    std::uint64_t limbo_peak;
    std::uint64_t maint_passes;
    std::uint64_t maint_ns;
    std::uint64_t advance_attempts;
    std::uint64_t advance_successes;
};

inline MechStats mech_stats() {
    detail::Domain& dom = detail::Domain::instance();
    return {dom.statLimboPeak.load(std::memory_order_relaxed),
            dom.statMaintPasses.load(std::memory_order_relaxed),
            dom.statMaintNs.load(std::memory_order_relaxed),
            dom.statAdvanceAttempts.load(std::memory_order_relaxed),
            dom.statAdvanceSuccesses.load(std::memory_order_relaxed)};
}

inline void mech_stats_reset() {
    detail::Domain& dom = detail::Domain::instance();
    dom.statLimboPeak.store(0, std::memory_order_relaxed);
    dom.statMaintPasses.store(0, std::memory_order_relaxed);
    dom.statMaintNs.store(0, std::memory_order_relaxed);
    dom.statAdvanceAttempts.store(0, std::memory_order_relaxed);
    dom.statAdvanceSuccesses.store(0, std::memory_order_relaxed);
}
#endif

// Free EVERYTHING immediately, ignoring epochs. Only legal when the caller
// guarantees no thread is concurrently inside any protected operation
// (e.g. end of a test, after all workers joined). Not part of the normal API.
inline void flush_all_unsafe() {
    detail::ThreadCtx& ctx = detail::tls();
    for (const detail::Retired& r : ctx.limbo) r.deleter(r.ptr);
    ctx.limbo.clear();
    std::lock_guard<std::mutex> g(ctx.dom.orphanMu);
    for (const detail::Retired& r : ctx.dom.orphans) r.deleter(r.ptr);
    ctx.dom.orphans.clear();
}

}  // namespace mpmc::ebr
