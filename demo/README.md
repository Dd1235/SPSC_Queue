# demo — the "why lock-free" story, made tangible

Two runnable demos in the two textbook SPSC domains — **real-time audio** and
**market data / HFT** — that turn the abstract argument ("a mutex can stall the
thread that must not stall") into numbers you can **see and hear**. Each runs the
*same* workload over a `std::mutex` channel and over the lock-free `SPSCQueue`, and
each measures a *different* property.

> **Shared modeling note (the honest part):** real latency spikes come from
> *priority inversion* — the OS preempts a thread while it holds the lock (or it
> page-faults / does a slow op under the lock), stalling whoever needs the lock next.
> Both demos model this by having a thread occasionally stay busy 6–12 ms. With a
> mutex that time is spent holding the lock, so the other side stalls; with the
> lock-free queue there is no lock, so it doesn't. Same workload, fixed RNG seed →
> a fair A/B. It's a real-time, scheduler-dependent simulation, so exact numbers
> vary run to run — but the *shape* is stable.

---

## 1. `audio_demo` — real-time audio (hear the dropouts)

An audio callback must render one 256-frame buffer every **5.33 ms** (256 frames @
48 kHz). That deadline is hard: if the callback can't fetch its control data and
render in time, the sound card gets nothing and you hear a **click** (an
"xrun"/dropout). A control thread ("the UI") streams a C-major scale with vibrato to
the audio thread through the queue; missed buffers are written as silence, so the
glitches are **audible**.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target audio_demo
cd build/demo && ./audio_demo 4          # seconds per pass (optional, default 4)

afplay audio_mutex.wav      # macOS: clicks / dropouts
afplay audio_lockfree.wav   # clean
# (Linux: aplay / paplay; or open the .wav in any player)
```

Representative result (Apple M2):

```
                              mutex+queue   lock-free SPSC
audio dropouts (xruns)                 20                0
  = silence/glitch time             107 ms              0 ms
max wait for data                14.245 ms          0.011 ms
```

The budget is 5.33 ms; the mutex's worst-case wait (**14 ms**) blows through it —
the audio thread blocked behind the held lock — producing ~107 ms of audible silence
across 20 dropouts. The lock-free run waits **11 µs** and never drops a buffer.

---

## 2. `marketdata_demo` — HFT (the tail latency)

A feed handler (producer) streams ~40k ticks/s over 8 symbols to a trading strategy
(consumer) that keeps a **minimal top-of-book** (best bid/ask) and fires a signal.
In trading the **average** latency is irrelevant — the **tail** is everything (a rare
slow tick = a trade on a stale quote). So this reports tick-to-strategy latency
percentiles and a 50 µs reaction-deadline SLA.

```sh
cmake --build build --target marketdata_demo
./build/demo/marketdata_demo 4           # seconds per run (optional, default 4)
```

Representative result (Apple M2):

```
                                mutex+queue   lock-free SPSC
dropped ticks                          3050                0
tick->strategy p50                   1.9 us           1.9 us
tick->strategy p99.9              9550.1 us         451.8 us
tick->strategy max               14638.8 us         2997.6 us
delivered under deadline            99.61%          99.86%
```

p50 is identical (~2 µs) — both are fast in the common case. But when the feed
handler is preempted holding the lock, the strategy freezes: the mutex's **p99.9 is
~10 ms vs ~0.3 ms lock-free** (a 20–70× tail difference), plus ~3000 dropped ticks.

> **Why feed-handler preemption only (a survivorship subtlety):** if we also stalled
> the strategy, the mutex would *drop* the slow ticks (try_lock fails) while the
> lock-free queue keeps and delivers them late — making the lock-free latency look
> *worse* purely because the mutex threw the slow ticks away. Modeling only the feed
> handler keeps the latency comparison apples-to-apples. A full order book / matching
> engine / protocol parser is deliberately out of scope — the demo measures the
> *queue*, not a trading system.

---

Both demos show the same core property from different angles: **with a lock, one
thread's stall stalls the other; with the lock-free queue, it can't.** Audio makes
that audible (dropouts); market data makes it measurable (tail latency).
