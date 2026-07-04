# Related work — reading list and positioning

Positioning in one line: prior queue evaluations overwhelmingly report symmetric
x86 server results; we characterize the same classic design space on asymmetric
client ARM (P/E cores, no pinning, fanless thermals) with a reproducible
methodology.

## Core algorithm papers (must cite)

- **Michael & Scott, PODC 1996** — "Simple, Fast, and Practical Non-Blocking and
  Blocking Concurrent Queue Algorithms." The MS queue we implement.
- **Michael, 2004** — hazard pointers (the SMR alternative we did not choose).
- **Fraser, 2004 (PhD thesis)** — epoch-based reclamation (what we did choose).
- **Vyukov** — bounded MPMC queue (1024cores.net write-up; cite as web/tech note).
- **Morrison & Afek, PPoPP 2013** — LCRQ (FAA-based state of the art; our
  future-work pointer and the origin of the FAA-beats-CAS observation).
- **Yang & Mellor-Crummey, PPoPP 2016** — wait-free queue (taxonomy context).
- **Thompson et al.** — LMAX Disruptor technical paper (ticket/sequencer lineage).

## Methodology (must cite)

- **Hoefler & Belli, SC 2015** — "Scientific Benchmarking of Parallel Computing
  Systems": median/percentile reporting, repeatability rules we follow.
- **Tene** — "How NOT to Measure Latency" (coordinated omission; our latency mode
  measures from scheduled send time).
- **Fleming & Wallace, CACM 1986** — "How not to lie with statistics" (geomean
  rules if we aggregate).

## Libraries compared/referenced

- moodycamel::ConcurrentQueue (industrial MPMC reference in the harness);
  moodycamel ReaderWriterQueue + rigtorp SPSCQueue (already benchmarked in the
  SPSC phase); boost.lockfree (mention, not benchmarked); folly MPMCQueue
  (mention).

## Apple-silicon / asymmetric-scheduling context (to firm up during Phase D)

- Apple platform docs on QoS classes (the only sanctioned placement control on
  macOS — no thread affinity API on Apple silicon).
- Prior characterizations of big.LITTLE / heterogeneous scheduling effects on
  synchronization (search: energy-aware scheduling + lock contention on
  heterogeneous cores; also Apple M1/M2 microarchitecture studies, e.g. the
  community reverse-engineering reports, cited cautiously).

## Gap we claim

Queue microbenchmark papers standardize on dedicated, symmetric, pinned x86
cores. Client silicon breaks all three assumptions (asymmetric P/E, no pinning,
thermal limits). We quantify how much of the textbook ranking survives.
