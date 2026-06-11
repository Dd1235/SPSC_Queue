// marketdata_demo.cpp -- the "why lock-free" story in a trading pipeline.
//
// Models the canonical HFT hand-off: a feed-handler thread (producer) normalizes a
// market-data stream and passes ticks to a strategy thread (consumer) that keeps a
// top-of-book and reacts. The metric that matters in this world is not average
// throughput -- it's the *tail* of tick-to-strategy latency (p99/p99.9) and dropped
// ticks, because a stale or missed quote is a missed/﻿bad trade.
//
// The same workload runs over a std::mutex channel and over the lock-free
// SPSCQueue. To be fair to the mutex, its channel is *non-blocking* (try_lock,
// drop-on-contention) -- the realistic real-time pattern -- so it never parks a
// thread. Even so, when the feed handler is preempted while holding the lock, the
// strategy can't drain: its queued ticks just age, so the tail of tick-to-strategy
// latency blows up and it blows past any reaction deadline. The lock-free queue
// has no lock to contend, so the strategy keeps draining and the tail stays flat.
//
// Modeling note (the honest part): real latency spikes come from the OS preempting
// a thread while it holds the lock (priority inversion), or a page fault / slow op
// under the lock. We model that by having the feed handler occasionally hold the
// channel busy for a few ms. With a mutex that busy time blocks the strategy; with
// the lock-free queue there is no lock, so it doesn't. Same workload, fixed RNG
// seed -> a fair A/B.
//
// We deliberately model only *feed-handler* preemption, not strategy preemption.
// If we stalled the strategy too, the mutex would drop the slow ticks (try_lock
// fails) while the lock-free queue would keep and deliver them late -- which makes
// the lock-free latency look *worse* purely by survivorship bias (the mutex's tail
// is "good" only because it threw the slow ticks away). Measuring feed-handler
// preemption keeps the latency comparison apples-to-apples. (A small number of
// drops still occurs from ordinary try_lock contention; we report it.)
//
// This demo deliberately stops at a *minimal top-of-book* (best bid/ask + a trivial
// signal). A full limit order book, matching engine, or exchange-protocol parser
// (ITCH/OUCH/FIX) is a separate, much larger project and would not tell you
// anything more about the queue.
#include "spsc/spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

// ---- parameters ----------------------------------------------------------
static constexpr int kNumSymbols = 8;
static constexpr int kBurst = 8;        // ticks generated per producer step
static constexpr double kStepMs = 0.2;  // -> ~40k ticks/s
static constexpr std::size_t kCapacity = 4096;
static constexpr double kProdStallEveryMs = 150.0;  // feed handler preemption cadence
static constexpr double kStallMinMs = 6.0;
static constexpr double kStallMaxMs = 12.0;
static constexpr double kSlaUs = 50.0;  // "react within 50 us" deadline

enum class Kind : std::uint8_t { Quote, Trade };

struct Tick {
    std::uint16_t symbol = 0;
    Kind kind = Kind::Quote;
    float bid = 0, ask = 0;  // valid for Quote
    float price = 0;         // valid for Trade
    std::uint32_t size = 0;
    std::uint64_t seq = 0;   // per-symbol, for gap/drop detection
    Clock::time_point ts{};  // produced-at, for latency
};

static void sleep_ms(double ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ---- the two channels, same interface ------------------------------------
// push(): enqueue a tick, false if it could not be taken (dropped).
// try_pop(): dequeue, false if nothing available right now.
// stall(): model being preempted while holding the channel for `ms`.

struct MutexChannel {
    std::mutex m;
    std::queue<Tick> q;
    std::size_t cap;
    explicit MutexChannel(std::size_t c) : cap(c) {}
    bool push(const Tick& t) {
        std::unique_lock<std::mutex> g(m, std::try_to_lock);
        if (!g.owns_lock()) return false;   // contended -> drop (don't block the feed)
        if (q.size() >= cap) return false;  // full -> drop (back-pressure)
        q.push(t);
        return true;
    }
    bool try_pop(Tick& out) {
        std::unique_lock<std::mutex> g(m, std::try_to_lock);
        if (!g.owns_lock()) return false;  // contended -> looks empty this attempt
        if (q.empty()) return false;
        out = q.front();
        q.pop();
        return true;
    }
    void stall(double ms) {
        std::lock_guard<std::mutex> g(m);
        sleep_ms(ms);
    }
};

struct LockFreeChannel {
    spsc::SPSCQueue<Tick> q;
    explicit LockFreeChannel(std::size_t c) : q(c) {}
    bool push(const Tick& t) { return q.try_push(t); }
    bool try_pop(Tick& out) { return q.try_pop(out); }
    void stall(double ms) { sleep_ms(ms); }  // preempted, holding nothing
};

struct Result {
    std::uint64_t produced = 0, processed = 0, dropped = 0, signals = 0;
    double seconds = 0;
    double p50 = 0, p99 = 0, p999 = 0, maxLat = 0;  // tick->strategy latency, microseconds
    double slaPct = 0;                              // % of ticks delivered under kSlaUs
};

template <class Channel> static Result run(double seconds) {
    Channel ch(kCapacity);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> produced{0}, dropped{0};

    std::thread feed([&] {
        std::mt19937 rng(12345);  // fixed seed = fair A/B
        std::uniform_real_distribution<float> walk(-0.05f, 0.05f);
        std::uniform_real_distribution<double> stallDist(kStallMinMs, kStallMaxMs);
        std::uniform_int_distribution<int> symDist(0, kNumSymbols - 1);
        float mid[kNumSymbols];
        std::uint64_t seq[kNumSymbols];
        for (int i = 0; i < kNumSymbols; ++i) {
            mid[i] = 100.0f + i;
            seq[i] = 0;
        }
        auto t0 = Clock::now();
        double nextStall = kProdStallEveryMs;
        std::uint64_t prod = 0, drop = 0;
        while (!done.load(std::memory_order_relaxed)) {
            for (int b = 0; b < kBurst; ++b) {
                int s = symDist(rng);
                mid[s] += walk(rng);
                if (mid[s] < 1.0f) mid[s] = 1.0f;
                Tick tk;
                tk.symbol = static_cast<std::uint16_t>(s);
                tk.seq = ++seq[s];
                tk.ts = Clock::now();
                if (rng() % 8 == 0) {  // ~1 in 8 is a trade
                    tk.kind = Kind::Trade;
                    tk.price = mid[s];
                    tk.size = 100;
                } else {
                    tk.kind = Kind::Quote;
                    float half = 0.01f;
                    tk.bid = mid[s] - half;
                    tk.ask = mid[s] + half;
                }
                ++prod;
                if (!ch.push(tk)) ++drop;  // could not enqueue in time -> lost tick
            }
            double t = ms_since(t0);
            if (t >= nextStall) {
                ch.stall(stallDist(rng));
                nextStall += kProdStallEveryMs;
            }
            sleep_ms(kStepMs);
        }
        produced.store(prod, std::memory_order_relaxed);
        dropped.store(drop, std::memory_order_relaxed);
    });

    // This thread is the strategy (consumer).
    float bestBid[kNumSymbols], bestAsk[kNumSymbols];
    double ema[kNumSymbols];
    bool above[kNumSymbols], emaInit[kNumSymbols];
    for (int i = 0; i < kNumSymbols; ++i) {
        bestBid[i] = bestAsk[i] = 0;
        ema[i] = 0;
        above[i] = false;
        emaInit[i] = false;
    }
    std::vector<double> lat;
    lat.reserve(1 << 21);
    std::uint64_t processed = 0, signals = 0;

    auto start = Clock::now();
    Tick tk;
    while (ms_since(start) < seconds * 1000.0) {
        bool got = false;
        while (ch.try_pop(tk)) {
            got = true;
            lat.push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - tk.ts).count());
            ++processed;
            if (tk.kind == Kind::Quote) {
                int s = tk.symbol;
                bestBid[s] = tk.bid;
                bestAsk[s] = tk.ask;
                double mid = 0.5 * (tk.bid + tk.ask);
                if (!emaInit[s]) {
                    ema[s] = mid;
                    emaInit[s] = true;
                    above[s] = true;
                } else {
                    ema[s] = 0.05 * mid + 0.95 * ema[s];
                    bool nowAbove = mid > ema[s];
                    if (nowAbove != above[s]) {
                        ++signals;
                        above[s] = nowAbove;
                    }  // EMA crossover
                }
            }
        }
        if (!got) std::this_thread::yield();
    }
    double secs = ms_since(start) / 1000.0;
    done.store(true, std::memory_order_relaxed);
    feed.join();

    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) {
        return lat.empty() ? 0.0 : lat[static_cast<std::size_t>(p * (lat.size() - 1))];
    };
    Result r;
    r.produced = produced.load();
    r.processed = processed;
    r.dropped = dropped.load();
    r.signals = signals;
    r.seconds = secs;
    r.p50 = pct(0.50);
    r.p99 = pct(0.99);
    r.p999 = pct(0.999);
    r.maxLat = lat.empty() ? 0.0 : lat.back();
    if (!lat.empty()) {
        auto under = std::lower_bound(lat.begin(), lat.end(), kSlaUs) - lat.begin();
        r.slaPct = 100.0 * static_cast<double>(under) / static_cast<double>(lat.size());
    }
    return r;
}

int main(int argc, char** argv) {
    double seconds = (argc > 1) ? std::atof(argv[1]) : 4.0;

    std::printf("Market-data feed-handler -> trading-strategy simulation\n");
    std::printf("  %d symbols, per-symbol top-of-book + EMA-crossover signal\n", kNumSymbols);
    std::printf("  feed handler: ~%.0fk ticks/s in bursts; models OS preemption by holding\n",
                kBurst / kStepMs);
    std::printf("  the channel busy %.0f-%.0f ms every ~%.0f ms (priority inversion)\n",
                kStallMinMs, kStallMaxMs, kProdStallEveryMs);
    std::printf("  reaction deadline (SLA): %.0f us; duration: %.1f s per run\n\n", kSlaUs,
                seconds);

    std::printf("running mutex + std::queue (non-blocking, drop-on-contention) ...\n");
    Result mx = run<MutexChannel>(seconds);
    std::printf("running lock-free SPSCQueue ...\n\n");
    Result lf = run<LockFreeChannel>(seconds);

    auto row_u = [](const char* k, std::uint64_t a, std::uint64_t b) {
        std::printf("%-28s %14llu %16llu\n", k, (unsigned long long)a, (unsigned long long)b);
    };
    auto row_f = [](const char* k, double a, double b) {
        std::printf("%-28s %11.1f us %13.1f us\n", k, a, b);
    };
    std::printf("%-28s %14s %16s\n", "", "mutex+queue", "lock-free SPSC");
    row_u("ticks produced", mx.produced, lf.produced);
    row_u("ticks processed", mx.processed, lf.processed);
    row_u("dropped ticks", mx.dropped, lf.dropped);
    row_u("strategy signals fired", mx.signals, lf.signals);
    std::printf("%-28s %12.0f/s %14.0f/s\n", "throughput", mx.processed / mx.seconds,
                lf.processed / lf.seconds);
    row_f("tick->strategy p50", mx.p50, lf.p50);
    row_f("tick->strategy p99", mx.p99, lf.p99);
    row_f("tick->strategy p99.9", mx.p999, lf.p999);
    row_f("tick->strategy max", mx.maxLat, lf.maxLat);
    std::printf("%-28s %12.2f%% %14.2f%%\n", "delivered under deadline", mx.slaPct, lf.slaPct);

    std::printf(
        "\nThe number that matters in trading is the tail. When the feed handler is\n");
    std::printf(
        "preempted holding the lock, the strategy can't drain -- its queued ticks age\n");
    std::printf(
        "and miss the reaction deadline. The lock-free queue has no lock to contend,\n");
    std::printf(
        "so the strategy keeps draining: the tail stays flat and ~all ticks make the\n");
    std::printf("deadline. (steady_clock resolves ~0.04 us here, so sub-0.1 us figures are\n");
    std::printf("approximate; the tail, which is what matters, is far above that floor.)\n");
    return 0;
}
