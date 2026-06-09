# spsc-queue

A header-only, **lock-free, bounded single-producer/single-consumer ring buffer**
in C++17 — *wait-free on the common path*, with cache-line-aware layout and a
cached-index optimization to cut cross-core coherence traffic.

Benchmarked against `std::mutex` and a packed/uncached variant of itself, and
validated with ThreadSanitizer, AddressSanitizer/UBSan, and a multi-million-op
stress harness.

```cpp
#include "spsc/spsc_queue.hpp"

spsc::SPSCQueue<int> q(1024);     // capacity = 1024

// producer thread
if (!q.try_push(42)) { /* full: caller decides to spin / drop / yield */ }

// consumer thread
int v;
if (q.try_pop(v)) { /* got 42 */ }
```

---

## Why this design

A bounded SPSC queue is the simplest interesting lock-free data structure, and
every decision in it is a talking point:

- **Bounded + array-backed** → no node allocation, no memory-reclamation problem
  (no hazard pointers / RCU / epoch GC).
- **Exactly one producer and one consumer** → each index has a single writer, so
  **no CAS is needed**. Plain atomic load/store with the right memory ordering
  suffices, and `try_push` / `try_pop` are **wait-free** (bounded steps, no loops).
- **Non-blocking** → the `try_*` calls return `false` instead of waiting; the
  caller owns the back-pressure policy.

### The three memory-ordering rules

```cpp
writeIdx_.load(relaxed)    // read your OWN index  -> relaxed (you're the only writer)
writeIdx_.store(release)   // publish your index   -> release (after touching the slot)
writeIdx_.load(acquire)    // read the OTHER index -> acquire (synchronize-with their release)
```

The producer constructs `slots_[w]` *sequenced-before* `writeIdx_.store(release)`.
The consumer's `writeIdx_.load(acquire)` reads that value → *synchronizes-with*
the release → the construction *happens-before* the consumer reads the slot. No
torn or stale reads. The symmetric release/acquire on `readIdx_` stops the
producer from overwriting a slot the consumer is still reading. That is the whole
correctness proof.

### Cache-line layout (the false-sharing story)

```
[ cold immutable: capacity_, ring_, slots_ ] --- line 0 (shared read-only)
[ alignas(128) writeIdx_, readIdxCache_     ] --- line 1 (producer writes)
[ alignas(128) readIdx_,  writeIdxCache_    ] --- line 2 (consumer writes)
```

The producer's hot data and the consumer's hot data live on **different cache
lines**, so the two cores never invalidate each other's line over MESI. We align
to **128 bytes**, and deliberately *don't* use
`std::hardware_destructive_interference_size`: that constant disagrees across
toolchains (`64` on libc++/libstdc++, `256` on GCC's default x86 tuning), which
would make the queue's layout differ per compiler — GCC even warns that a public
header should define its own constant instead. 128 covers the Apple M2's 128-byte
lines and x86's adjacent-line prefetch, and is stable everywhere
(override with `-DSPSC_CACHE_LINE_SIZE=...`).

### The cached-index optimization

The expensive part of `try_push` is reading the *consumer's* index — that atomic
is written by the other core, so the load is a cross-core coherence miss. We keep
a private, non-atomic cached copy and **only refresh it (pay the real load) when
the cache says the queue is full**. On a queue that isn't pinned at a boundary,
that load is skipped almost every time. This is the difference between "fast on a
microbenchmark" and "fast under real contention." (`try_pop` is symmetric.)

### Other details

- **`+1` slot:** the ring holds `capacity + 1` slots so `empty (read == write)`
  and `full (next(write) == read)` are distinguishable without a third,
  contended counter.
- **Raw storage + placement new:** the buffer is uninitialized aligned storage,
  not a `T[]`. This works for non-default-constructible `T`, constructs nothing
  eagerly, and destroys elements precisely — a clean object-lifetime story.

---

## API

```cpp
template <class T, bool Padded = true, bool Cached = true>
class SPSCQueue {
    explicit SPSCQueue(std::size_t capacity);

    // producer thread only
    bool try_push(const T& v);
    bool try_push(T&& v);
    template <class... Args> bool try_emplace(Args&&... args);

    // consumer thread only
    bool try_pop(T& out);
    T*   front();   // peek, nullptr if empty
    void pop();     // pop after front()

    std::size_t capacity() const;
    std::size_t size_approx() const;   // racy: metrics/debug only
};
```

The `Padded` and `Cached` template flags default to the production configuration.
They exist only so the benchmarks can A/B those two engineering decisions against
a single source of truth — not a forked copy of the code.

`size_approx()` is intentionally approximate: in a concurrent structure the size
can change the instant you read it, so it is safe for metrics but must never gate
correctness.

---

## Benchmarks

Apple M2, Apple clang 17, `-O3`, threads **not** pinned (macOS does not expose
usable affinity on Apple silicon); best of several runs.

| Benchmark | Result |
| --- | --- |
| Throughput (`uint64_t`, capacity 1024) | **~380 Mops/s** (~2.6 ns/op) |
| vs `std::mutex` + `std::queue` | **~13–15× faster** |
| False sharing — padded vs packed | **~3× penalty** when packed (2.8–4×) |
| Cached index — on vs off (capacity 1024) | **~2.2× faster** with caching |
| Ping-pong round-trip latency | **~100 ns mean** (one-way ~50 ns), p50 ~83 ns |

The cached-index win is workload-dependent: at *tiny* capacity (16) the queue
hovers at the full/empty boundary, the cache keeps missing, and caching can cost
a few percent — being able to explain that crossover is the point.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/benchmarks/bench_throughput
./build/benchmarks/bench_vs_mutex
./build/benchmarks/bench_false_sharing
./build/benchmarks/bench_cached_index
./build/benchmarks/bench_latency
```

---

## Demos

Two runnable demos in the two textbook SPSC domains, each measuring a *different*
property. Both model priority inversion (a thread preempted while holding the lock)
and compare a `std::mutex` channel against the lock-free queue on the identical
workload. Details in [demo/README.md](demo/README.md).

**Real-time audio — hear the difference.** [`demo/audio_demo.cpp`](demo/audio_demo.cpp)
simulates an audio callback that must render a 256-frame buffer every 5.33 ms while a
control thread streams parameters through the queue. A missed deadline becomes
silence — an audible click.

```sh
cmake --build build --target audio_demo && (cd build/demo && ./audio_demo 4)
afplay build/demo/audio_mutex.wav      # clicks / dropouts
afplay build/demo/audio_lockfree.wav   # clean
```

> M2: mutex drops **~20 buffers (~107 ms of silence)**, worst-case wait **~14 ms**;
> lock-free drops **0**, worst-case wait **~11 µs**.

**Market data / HFT — the tail latency.** [`demo/marketdata_demo.cpp`](demo/marketdata_demo.cpp)
simulates a feed handler streaming ticks to a strategy thread that keeps a minimal
top-of-book. The metric that decides money in trading is the *tail* of
tick-to-strategy latency.

```sh
cmake --build build --target marketdata_demo && ./build/demo/marketdata_demo 4
```

> M2: p50 ~2 µs for **both**, but **p99.9 ~10 ms (mutex) vs ~0.3 ms (lock-free)** —
> a ~20–70× tail difference — plus ~3000 dropped ticks vs 0. (A full order book /
> matching engine is deliberately out of scope — the demo measures the *queue*.)

---

## Tests & validation

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure

# ThreadSanitizer (understands acquire/release; catches a wrong `relaxed`)
cmake -S . -B build-tsan -DSPSC_ENABLE_TSAN=ON && cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

# AddressSanitizer + UBSan (ring bounds, object lifetime)
cmake -S . -B build-asan -DSPSC_ENABLE_ASAN=ON && cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

- **Functional:** FIFO order, full/empty edges, wraparound, object lifetime
  (placement-new/destructor accounting), move-only and non-default-constructible
  types, every template configuration.
- **Stress:** one producer streams `0..N-1`, one consumer asserts it receives
  exactly `0,1,2,...,N-1` in order — a single invariant that catches dropped,
  duplicated, **and** reordered items at once. Run up to 5M ops, tiny and large
  capacities, with and without scheduler yields.
- Tests can't *prove* the absence of a race; sound reasoning (the proof above)
  plus a model checker (Relacy / CDSChecker / GenMC) get closest. See the source
  comments and `learn/` for the full treatment.

---

## What exists already (and what I'd actually ship)

There is **no lock-free queue in the C++ standard library** (true through C++23);
`<atomic>` gives you the primitives, nothing more. Production-grade options:

- **`boost::lockfree::spsc_queue`** — the portable, well-tested default.
- **`folly::ProducerConsumerQueue`** (Meta) — ~100 lines, the classic readable SPSC.
- **`moodycamel::ReaderWriterQueue`** — popular header-only SPSC.
- **`rigtorp::SPSCQueue`** (Erik Rigtorp) — the reference implementation of the
  cached-index optimization used here; widely cited in low-latency circles.
- **LMAX Disruptor** — the ring-buffer framework that popularized cache-line
  padding, the single-writer principle, and "mechanical sympathy."

I would not ship my own — I'd reach for Folly or rigtorp. I reimplemented it to
internalize the acquire/release reasoning and the cache-line engineering those
libraries embody.

---

## Deliberately not built

- **Blocking variant** (consumer sleeps when empty) → condition variable or
  futex/eventfd; note it is then no longer lock-free.
- **MPMC** → bounded Vyukov queue with per-slot sequence numbers (needs CAS,
  has to handle ABA; lock-free but not wait-free).
- Batch push/pop, a byte-ring for I/O, huge pages.

---

## Build requirements

C++17, CMake ≥ 3.16. Header-only: to use the queue, just add `include/` to your
include path and `#include "spsc/spsc_queue.hpp"` — no library to link.

## License

MIT — see [LICENSE](LICENSE).
