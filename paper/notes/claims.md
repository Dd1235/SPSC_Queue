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
| **F1** | **H1 inverted, capacity-mediated: wait-free CLAIMING beats lock-free RETRY under preemption — given queue capacity to absorb scheduling gaps.** Vyukov (lock-free CAS) collapses under oversubscription while the blocking-on-slot FAA queue *improves* and posts the best tail by 3–6×. Mechanism hypothesis: preempted CAS losers burn scheduler quanta re-fighting for the cursor line across 32 threads, while FAA's fetch_add always succeeds and its spin-then-yield slot waits hand quanta back. Naively applied, the progress-guarantee taxonomy mispredicts this platform. | 4P:4C throughput ×1→×4: FAA 16.0→31.2 (+94%), Vyukov 8.6→1.5 (−83%), mutex 19.7→14.7, MS 7.3→5.4. p99.9 @×4: FAA 18 ms vs Vyukov 59 / mutex 72 / MS 81 / moody 116 ms | **VERIFIED** (v2, k=8: throughput replicates within ~6%; FAA 16.5→29.2, Vyukov 7.8→1.7. Capacity control REFINES it: at cap 64 FAA collapses to 2.57 — blocking-on-slot re-emerges exactly as originally predicted when slack vanishes; at cap 8192 FAA 20.4, non-monotone. State F1 as capacity-conditional.) |
| **F2** | **E-core placement reorders the ranking; FAA is placement-immune.** all-int → all-bg: FAA 16.2→15.6 (−4%), mutex 19.8→2.65 (−87%), Vyukov 8.6→0.53 (−94%), moody 8.2→1.8, MS 7.2→2.8. On E-cores FAA is ~6× the mutex and ~30× Vyukov. | qos table appended below | MEASURED |
| **F3** | **On dedicated cores at moderate mixed ratios the mutex baseline beats every lock-free MPMC design** (4:4: mutex 19.7 vs FAA 16.0, Vyukov 8.6, moody 7.9, MS 7.3). Lock-free wins only at 1:1 (FAA 123, Vyukov 117, mutex 32) and producer-heavy 7:1. "Lock-free is faster" is a 1:1 artifact here. | summary_v1.md ratio table | MEASURED |
| **F4** | SPSC 1:1 baseline (361 Mops/s) ≈ 3× the best MPMC at 1:1 (FAA 123) — the measured price of MPMC generality on this SoC. | summary_v1.md | MEASURED |

**Caveat C1 (calibration decomposition):** calib_ns spans 97–172 ms (~1.77×),
suspiciously close to a plausible P/E-core performance ratio. The calibration
loop runs on the fresh process's main thread with *default* QoS, so some of the
"drift" may be placement (main thread on an E-core), not thermal throttling.
Phase F: pin calibration to USER_INTERACTIVE; until then, do not cite calib
drift as purely thermal.

**Caveat C2 (launcher QoS inheritance — v1 latency absolutes are confounded):**
v2's explicit `QOS_CLASS_DEFAULT` reset after calibration lifted worker threads
out of QoS inherited from the background launcher shell. Result: dedicated-core
p99.9 dropped 10–30x (v1: 7–10 ms; v2: 0.27–0.66 ms). v1 cross-queue
*comparisons* stay valid (all arms equally penalized, interleaved), but **all
absolute latency numbers cite v2 only**. Paper methodology gets a paragraph:
"on macOS, worker QoS silently inherits from the launcher; reset it explicitly."

**H3 CONFIRMED with mechanism (v2 RSS column):** MS peak RSS 376 MB (1:1),
742 MB (1:7), 680 MB (2:6) in 2-second runs vs 1–2 MB for every bounded queue.
The asymmetry (7 MB at 7:1 vs 742 MB at 1:7) localizes the mechanism: EBR epoch
advancement requires every pinned thread to sit at the current epoch, so
reclamation starves as the count of concurrently pinned consumers grows.
Unboundedness converts back-pressure into memory growth, and EBR converts
consumer parallelism into reclamation lag.

**Edge finding (moody, cap 64, x4):** moodycamel::ConcurrentQueue exceeded the
120 s timeout in 7/8 rounds at capacity 64 under x4 oversubscription (wedged in
the drain/poison phase — its block-pool semantics interact badly with a tiny
capacity hint and saturated implicit producers). Reported as configuration
incompatibility, not scored.

**C1 update (v2):** after pinning the probe to USER_INTERACTIVE, calib spread
narrowed 97–172 → 93–148 ms. Placement explained part; the residual ~1.6x is
genuine thermal/DVFS. Calib remains an environment-health screen; cross-queue
fairness rests on interleaving.

| **F5** | **Fairness for free: FAA ticketing is near-perfectly fair; sub-queue designs trade fairness for throughput.** Per-producer push-count CoV: FAA <=0.008 everywhere (incl. x4); at 4:4 x4 others degrade (MS 0.14, Vyukov 0.12, moody 0.13, mutex 0.08); moody structurally unfair at producer-heavy shapes (0.18-0.19 at 6:2/7:1 — per-producer sub-queues). | v2 fair_cov column | MEASURED (v2; v1 spot-check pending if needed) |
| **F6-lite** | Which role tolerates E-cores is design-specific: MS collapses with producers demoted (2.1 vs 4.3 — allocation is producer-side); moody the opposite (4.5 vs 2.1 — consumer-side block recycling). One paragraph in Results, not a headline. | v2 qos prod-bg/cons-bg rows | MEASURED |
| **F7-lite** | The contention cliff: 1:1 -> 2:2 erases ~79% of FAA/Vyukov throughput (127.7->26.3, 118.4->25.2) but only ~33% of the mutex's — sharpens F3's "1:1 mirage" framing. | v2 ratio table | MEASURED |

**H4 SUPPORTED (v2):** per-queue median throughput varies <=±5% across the 8
trial rounds with no monotone decline; median per-config CoV 2.0% (p90 6.4%,
worst 20.7% = moody 7:1, its unfairness makes it noisy). The interleaving +
cooldown protocol held.

**Phase F remaining:** (1) ~~results prose~~ DONE 2026-07-07 (main.tex fully drafted from v2); (2) ARTIFACT.md; (3) venue deadline check; (4) human rewrite pass; (5) optional: fairness figure for F5, cross-machine invitation via artifact.

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
