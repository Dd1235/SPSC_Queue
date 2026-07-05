# Claims ledger — the source of truth

Every claim the paper will make, its status, and the data that backs it.
Nothing enters main.tex without a row here. Status: HYPOTHESIS → MEASURED
(with CSV pointer) → VERIFIED (stable across ≥2 full matrix runs).

## Central hypotheses (set before measurement — keeps us honest)

| # | Claim | Status | Evidence |
|---|-------|--------|----------|
| H1 | Progress guarantees are invisible under dedicated cores (≤8 threads) but dominate tail latency under oversubscription: the FAA/ticket queue's blocking-on-slot design degrades disproportionately vs Michael–Scott (lock-free) at ×2/×4 oversubscription. | **MEASURED — INVERTED** (see F1) | matrix_v1.csv (k=5); summary_v1.md |
| H2 | P/E-core asymmetry (steered via macOS QoS) shifts the throughput ranking of the designs; results measured on symmetric x86 servers do not transfer unchanged to client ARM. | **MEASURED — CONFIRMED, stronger than expected** (see F2) | matrix_v1.csv qos rows |
| H3 | Node-based MS pays allocation + pointer-chasing costs that array queues avoid, but its unboundedness converts back-pressure stalls into memory growth — a different failure mode, not a faster/slower verdict. | PARTIAL (MS mid-to-low throughput everywhere; memory-growth axis not yet instrumented) | summary_v1.md ratio table |
| H4 | On a fanless machine, uncontrolled trial ordering biases cross-queue comparisons measurably (thermal drift); round-robin interleaving bounds this. | MEASURED, needs decomposition — calib spans 97–172 ms across the run, but see caveat C1 (placement vs thermal) | fig_calib_v1; matrix_v1.csv calib_ns |
| H5 | The ARM weak memory model makes memory-ordering bugs *observable* that x86 masks; TSan + per-producer sequence invariants are the validation floor. | SUPPORTED (SPSC phase precedent) | learn/03 §7; CI matrix |

## FINDINGS from matrix v1 (k=5, 410/410 runs, 2026-07-05) — the paper's spine

| # | Finding | Numbers (medians) | Status |
|---|---------|-------------------|--------|
| **F1** | **H1 inverted: wait-free CLAIMING beats lock-free RETRY under preemption.** Vyukov (lock-free CAS) collapses under oversubscription while the blocking-on-slot FAA queue *improves* and posts the best tail by 3–6×. Mechanism hypothesis: preempted CAS losers burn scheduler quanta re-fighting for the cursor line across 32 threads, while FAA's fetch_add always succeeds and its spin-then-yield slot waits hand quanta back. Naively applied, the progress-guarantee taxonomy mispredicts this platform. | 4P:4C throughput ×1→×4: FAA 16.0→31.2 (+94%), Vyukov 8.6→1.5 (−83%), mutex 19.7→14.7, MS 7.3→5.4. p99.9 @×4: FAA 18 ms vs Vyukov 59 / mutex 72 / MS 81 / moody 116 ms | MEASURED (k=10 + capacity-sensitivity check required before VERIFIED — the FAA *increase* is surprising enough to demand re-verification) |
| **F2** | **E-core placement reorders the ranking; FAA is placement-immune.** all-int → all-bg: FAA 16.2→15.6 (−4%), mutex 19.8→2.65 (−87%), Vyukov 8.6→0.53 (−94%), moody 8.2→1.8, MS 7.2→2.8. On E-cores FAA is ~6× the mutex and ~30× Vyukov. | qos table appended below | MEASURED |
| **F3** | **On dedicated cores at moderate mixed ratios the mutex baseline beats every lock-free MPMC design** (4:4: mutex 19.7 vs FAA 16.0, Vyukov 8.6, moody 7.9, MS 7.3). Lock-free wins only at 1:1 (FAA 123, Vyukov 117, mutex 32) and producer-heavy 7:1. "Lock-free is faster" is a 1:1 artifact here. | summary_v1.md ratio table | MEASURED |
| **F4** | SPSC 1:1 baseline (361 Mops/s) ≈ 3× the best MPMC at 1:1 (FAA 123) — the measured price of MPMC generality on this SoC. | summary_v1.md | MEASURED |

**Caveat C1 (calibration decomposition):** calib_ns spans 97–172 ms (~1.77×),
suspiciously close to a plausible P/E-core performance ratio. The calibration
loop runs on the fresh process's main thread with *default* QoS, so some of the
"drift" may be placement (main thread on an E-core), not thermal throttling.
Phase F: pin calibration to USER_INTERACTIVE; until then, do not cite calib
drift as purely thermal.

**Phase F follow-ups:** (1) k=10 run → VERIFIED statuses; (2) FAA capacity
sensitivity (64/1024/8192) to bound the pipelining explanation of its
oversubscription gain; (3) calib QoS pin (C1); (4) per-run RSS sampling to
instrument H3's memory-growth axis.

### Appendix: QoS table (throughput Mops/s, 4P:4C, ×1, matrix v1 medians)

| qos | faa | moody | ms | mutex | vyukov |
|---|---|---|---|---|---|
| none | 16.04 | 7.87 | 7.31 | 19.65 | 8.59 |
| all-int | 16.20 | 8.17 | 7.20 | 19.80 | 8.56 |
| all-bg | 15.64 | 1.80 | 2.80 | 2.65 | 0.53 |
| prod-bg | 8.08 | 4.65 | 2.11 | 8.49 | 4.27 |
| cons-bg | 8.97 | 1.97 | 4.24 | 11.16 | 4.94 |

## Established facts usable as context (from the SPSC phase)

- SPSC baseline: ~380 Mops/s single-op, ~1.08 Gops/s batched; within ±10% of
  rigtorp/moodycamel RWQ under one harness (see repo README; M2, clang 17, -O3).
- False sharing penalty ~3×; cached-index ~2.2× (workload-dependent).

## Progress-guarantee taxonomy (Design section table; precision matters)

- Michael–Scott: lock-free enqueue+dequeue; unbounded; EBR reclamation (stalled
  thread delays frees — interacts with H1!).
- Vyukov bounded: lock-free via bounded CAS retry; not wait-free.
- FAA/ticket: wait-free ticket acquisition; blocking-on-slot completion (a
  preempted claimant blocks the successor of that slot).
- Baselines: mutex+std::queue (blocking); our SPSC (wait-free, 1:1 only).
