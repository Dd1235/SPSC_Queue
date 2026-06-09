# demo — the "why lock-free" story, made audible

A real-time audio engine simulation that turns the abstract argument ("a mutex can
block the audio thread") into something you can **count and hear**.

## What it does

An audio callback must render one 256-frame buffer every **5.33 ms** (256 frames @
48 kHz). That deadline is hard: if the callback can't fetch its control data and
render in time, the sound card is handed nothing and you hear a **click** (an
"xrun"/dropout). A control thread ("the UI") streams parameter changes — a little
C-major scale with vibrato — to the audio thread through a queue.

The exact same workload runs twice:
- **`mutex + std::queue`** control channel
- **lock-free `SPSCQueue`** control channel

It counts dropouts, measures how long the audio thread waited for its data, and
writes both outputs to WAV.

> **Modeling note (the honest part):** real audio glitches come from *priority
> inversion* — the OS preempts the control thread while it holds the lock, and the
> audio thread is stuck waiting. We model that by having the control thread
> occasionally stay busy 6–12 ms. With a mutex that time is spent holding the lock,
> so the audio thread blocks; with the lock-free queue there is no lock, so the
> audio thread is **never** blocked by the producer. That's the wait-free guarantee
> — demonstrated, not asserted.

## Run it

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target audio_demo
cd build/demo
./audio_demo 4          # 4 seconds per pass (optional arg, default 4)

afplay audio_mutex.wav      # macOS: clicks / dropouts
afplay audio_lockfree.wav   # clean
# (Linux: aplay / paplay; or open the .wav in any player)
```

## Representative result (Apple M2)

```
                              mutex+queue   lock-free SPSC
buffers rendered                      750              750
audio dropouts (xruns)                 20                0
  = silence/glitch time             107 ms              0 ms
max wait for data                14.245 ms          0.011 ms
p99 wait for data                10.100 ms          0.001 ms
```

The audio thread's budget is 5.33 ms. The mutex run's worst-case wait (**14 ms**)
blows through it — that's the audio thread blocked behind the held lock — producing
~107 ms of audible silence across 20 dropouts. The lock-free run's worst-case wait
is **11 microseconds**: it never waits on the producer at all.

That single contrast — *blocking vs. a bounded wait-free hand-off* — is the entire
reason this project exists. The numbers vary run to run (it's a real-time, scheduler-
dependent simulation), but the mutex always drops buffers and the lock-free queue
never does.

See [`../learn/11-real-world-use-cases.md`](../learn/11-real-world-use-cases.md) for
why audio is the canonical use case, and
[`../learn/02-concurrency-fundamentals.md`](../learn/02-concurrency-fundamentals.md)
for the lock-free vs. blocking distinction this demonstrates.
