# Claims ledger — the source of truth

Every claim the paper will make, its status, and the data that backs it.
Nothing enters main.tex without a row here. Status: HYPOTHESIS → MEASURED
(with CSV pointer) → VERIFIED (stable across ≥2 full matrix runs).

## Central hypotheses (set before measurement — keeps us honest)

| # | Claim | Status | Evidence |
|---|-------|--------|----------|
| H1 | **Historical a priori hypothesis:** the FAA/ticket queue's irrevocable reservation would degrade disproportionately under oversubscription relative to MS and the CAS ring. **Taxonomy correction:** Vyukov explicitly lacks a formal lock-free guarantee too, so only the MS algorithmic core is a formal progress-class contrast; FAA versus Vyukov is a reservation-structure comparison. | **MEASURED — DIRECTION INVERTED, TAXONOMY CORRECTED** (see F1) | matrix_v1.csv (k=5); summary_v1.md; `include/mpmc/vyukov_queue.hpp`; Vyukov source note |
| H2 | Requested macOS interactive/background QoS policy shifts throughput rankings on this M2 system. The historical harness did not record QoS-call success or actual core residency, so it cannot identify P/E asymmetry as the cause. | **MEASURED — CONFIRMED AS A REQUESTED-QoS EFFECT** (see F2) | matrix_v2.csv QoS rows |
| H3 | This MS+EBR implementation trades the absence of a strict capacity bound for large full-trial peak RSS in consumer-heavy shapes. RSS does not identify the contribution of live nodes, retired limbo, metadata, or allocator behavior, and the experiment does not isolate allocation or pointer-chasing costs. | **MEASURED WITH IMPLEMENTATION-SCOPE CAVEATS** | matrix_v2.csv peak RSS; matrix_h3.csv within-build remediation |
| H4 | On a fanless machine, environment drift and fixed trial order can bias comparisons. Shape-wise interleaving and calibration keep arms close in time and screen drift, but fixed queue order does not eliminate position bias. | MEASURED, with limitation — calib spans 97–172 ms in v1; see C1 and v2 update | fig_calib_v1/v2; matrix_v1.csv; matrix_v2.csv |
| H5 | Cross-platform stress tests, TSan, and sequence invariants are a validation floor, not a proof of linearizability or progress. | SUPPORTED AS VALIDATION PRACTICE | CI matrix; stress tests |

## Findings first observed in v1 and checked in later datasets

| # | Finding | Numbers (medians) | Status |
|---|---------|-------------------|--------|
| **F1** | **Capacity-mediated oversubscription contrast:** at capacity 1024, FAA/ticket improves while the CAS-reservation ring collapses. Both bounded protocols can block after reservation; this is not a formal lock-free-versus-blocking comparison. Parent controls show that one yield policy does not rescue Vyukov and FAA is not necessary for the ticket arm's rising profile. Cross-family evidence is consistent with ticket/turn coordination but does not isolate it. | 4P:4C throughput ×1→×4: v1 FAA 16.0→31.2 (+94%), Vyukov 8.6→1.5 (−83%); v2 FAA 16.5→29.2, Vyukov 7.8→1.7; v3 FAA 16.5→29.7, Vyukov 7.9→1.7. Complete-sample v3 p99.9: CAS/ticket 14.1 ms, FAA 14.2, next-best mutex 69.5. | **REPLICATED DIRECTIONALLY**. At cap 64 both ticket arms fall to 2.6; at cap 8192 they remain above cap 64 but below cap 1024. Three capacities show a boundary, not an iff law. |
| **F2** | **Requested QoS policy reorders the ranking.** Canonical v2 all-int → all-bg: FAA 16.71→14.69 (−12%), mutex 19.11→3.62 (−81%), Vyukov 8.09→0.46 (−94%). Under background QoS FAA is ~4× the mutex and ~32× Vyukov. The historical rows do not record call success, and this cannot be relabeled an E-core result without residency traces. | matrix_v2.csv QoS rows; v1 replication table appended below | MEASURED |
| **F3** | **The bounded mutex leads every tested MPMC arm at exactly the 4:4 and 2:6 ×1 shapes.** At 2:2, FAA and Vyukov still exceed the mutex; FAA also leads at 1:7, 6:2, and 7:1. The 1:1 ranking is not representative of all ratios, but neither is a universal mutex win. | matrix_v2.csv, trials 1–7, capacity 1024 | MEASURED |
| **F4** | At the only shape where SPSC is legal, its v2 median is 373.6 Mops/s versus 127.7 for the best MPMC arm (FAA), a 2.93× specialization gap in this harness rather than a universal “price of MPMC.” | matrix_v2.csv, trials 1--7, capacity 1024 | MEASURED |

**Caveat C1 (calibration decomposition):** calib_ns spans 97–172 ms (~1.77×),
large enough that scheduler placement was a plausible confounder. The
calibration loop runs on the fresh process's main thread with *default* QoS, so
some of the "drift" is scheduling rather than thermal throttling. Phase F assigns
USER_INTERACTIVE; without residency/frequency/temperature data, do not label
either range as a particular core class or as purely thermal.

**Caveat C2 (launcher QoS inheritance — v1 latency absolutes are confounded):**
v2's explicit `QOS_CLASS_DEFAULT` reset after calibration lifted worker threads
out of QoS inherited from the background launcher shell. Result: ×1
p99.9 dropped 10–30x (v1: 7–10 ms; v2: 0.27–0.66 ms). v1 cross-queue
comparisons are at most directional; **all absolute latency numbers cite v2 or
v3 only**. Paper methodology gets a paragraph:
"on macOS, worker QoS silently inherits from the launcher; reset it explicitly."

**H3 CONFIRMED as an end-to-end implementation metric (v2 RSS column):** MS
process peak RSS 376 MB (1:1),
742 MB (1:7), 680 MB (2:6) in 2-second runs vs 1–2 MB for every bounded queue.
The asymmetry (7 MB at 7:1 vs 742 MB at 1:7) implicates the dequeue-side EBR
path. RSS does not separate live nodes, retired limbo, vector/allocator capacity,
or transient TLS-to-orphan handoff. It is read after drain and thread exit, so
the paper reports full-trial process peak RSS rather than steady-state queue
footprint.

**Edge finding (moody, capacity argument 64, x4):**
moodycamel::ConcurrentQueue completed the v2 warmup but no measured round before
the 120 s timeout (drain/poison phase). Its constructor argument is a
preallocation estimate, not a strict capacity. Report as a harness/configuration
incompatibility, not a capacity result or score.

**C1 update (v2):** after requesting USER_INTERACTIVE for the calibration probe, calib spread
narrowed 97–172 → 93–148 ms. Scheduling explained part; the residual can mix
DVFS, temperature, and placement. Per-queue calibration medians differ ~1.6%,
but queue order is fixed, so calibration screens rather than eliminates bias.

**F2 round-level outlier:** one of seven v2 all-background rounds was a severe
collapse: FAA completed zero real messages; Vyukov, mutex, and moodycamel
reported 0.092, 0.174, and 0.253 Mops/s; MS remained at 2.65. FAA led the other
six paired rounds. Median/IQR claims describe the central response, not
round-level reliability under background scheduling.

| **F5** | **FAA has the lowest finite-window producer-count CoV at plotted 4:4/capacity-1024 conditions.** v3 medians: FAA 0.0017 (×1), 0.0076 (×4); CAS/ticket 0.014/0.010. Other ×4 arms span 0.073–0.150. This is empirical work balance, not starvation freedom or fairness by construction; fast threads may claim more tickets and startup is skewed. | matrix_v3.csv fair_cov, capacity 1024 | MEASURED |
| **F6-lite** | Role-specific QoS sensitivity is design-specific: MS is lower with producers background than consumers background (2.1 vs 4.3); moody shows the opposite (4.5 vs 2.1). Allocation/recycling explanations are plausible but unmeasured. | v2 QoS prod-bg/cons-bg rows | MEASURED |
| **F7-lite** | The contention cliff: 1:1 -> 2:2 erases ~79% of FAA/Vyukov throughput (127.7->26.3, 118.4->25.2) but only ~33% of the mutex's — sharpens F3's "1:1 mirage" framing. | v2 ratio table | MEASURED |

**H4 SCREEN (v2):** there is no monotone matrix-long calibration decline and
per-queue calibration medians differ ~1.6%. This reduces concern about drift,
but fixed queue order remains an internal-validity threat; do not claim the
interleaving protocol removed thermal bias.

## F1 evidence from parent controls (matrix v3, k=8 total; 2026-07-07)

Each control changes one property relative to its own parent. There is no
single-variable bridge between the Vyukov and ticket protocols:

| Arm | Claim | Completion | Spin policy | x1 -> x4 Mops/s | p99.9 @x4 |
|---|---|---|---|---|---|
| vyukov | CAS | shared-cursor | raw retry | 7.88 -> 1.66 | 70.6 ms |
| **vyukov-b** | CAS | shared-cursor | **+yield** | 8.00 -> 1.68 | 75.0 ms |
| **casticket** | **CAS** | ticket/turn | spin+yield | 7.06 -> 19.33 | 14.1 ms |
| faa | FAA | ticket/turn | spin+yield | 16.46 -> 29.68 | 14.2 ms |

**Bounded verdicts:**
1. **One yielding policy does not rescue Vyukov.** Vyukov-backoff follows the
   same throughput collapse (8.00 → 1.68 versus 7.88 → 1.66). Complete-sample
   p99.9 is 75.0 versus 70.6 ms; do not call tails identical.
2. **FAA is not necessary for the ticket parent's qualitative profile.**
   CAS/ticket rises 7.06 → 19.33 and keeps a 14.1 ms tail. This supports, but
   does not isolate, the shared ticket/turn protocol because cross-family
   implementations differ in more than one property.
3. **FAA is faster within the ticket parent pair**, by 2.3x at x1 and 1.5x at
   x4; avoid the blanket “~2x everywhere” claim.
4. **Mechanism data are diagnostic only.** Canonical v3s is a complete 1.5 s
   run with one warmup + two measured rounds. At x4, Vyukov records 0.7515 CAS
   failures and 1.68695 cursor rereads per completed message; FAA records zero
   claim failures and 0.057 slot-yield events/message. A slot yield follows
   1024 loads, so event types are not unit-cost comparable. Shared atomic
   counters perturb retry paths. Median ivcsw is Vyukov 84.9k,
   Vyukov-backoff 57.0k, FAA 919.9k: differing raw switch counts do not explain
   the throughput order, but no causal coherence claim follows.

F1 paper form: *At capacity 1024 on this system, ticket arms rise under
oversubscription while the CAS-reservation ring collapses; a yield policy does
not rescue the latter, FAA is not required for the ticket profile, and capacity
64 removes the advantage. The broader ticket/turn explanation is consistent
with controls and diagnostics, not isolated causally.*

## F8 — the reclamation remediation (matrix_h3, k=8; 2026-07-13)

Three-way A/B/C at the pathological shapes (RSS MB medians | throughput Mops/s):

| ratio | legacy | prefix fix | retry variant | verdict |
|---|---|---|---|---|
| 1:7 | 626 \| 5.4 | **153 \| 9.4** | 717 \| 2.6 | fix: RSS −76%, throughput +76% |
| 2:6 | 536 \| 7.0 | **97 \| 8.9** | 771 \| 3.5 | fix: RSS −82%, throughput +28% |
| 4:4 | 24 \| 6.3 | 11 \| 5.5 | 165 \| 5.5 | fix: RSS −52%, throughput −13% |
| 1:1 | 541 \| 11.8 | 526 \| 11.3 | 552 \| 11.9 | **no RSS change — see boundary** |

- **Prefix mode:** legacy unconditionally scans/compacts O(limbo). Prefix mode
  returns O(1) if nothing expires; otherwise it visits expired entries and
  `vector::erase` shifts survivors, so it is not generally O(freed). The 1:7
  and 2:6 A/B result strongly implicates scan cost, but limbo size, maintenance
  time, and live-node count were not recorded. Describe the feedback loop as
  code-path analysis consistent with the data, not directly measured causality.
- **Tradeoff:** at 4:4 RSS falls 52% but end-to-end throughput falls 13%.
  Throughput includes post-stop drain, joins, and TLS EBR teardown, so gains are
  not exclusively in-window consumer speed. `ru_maxrss` after teardown can
  include transient orphan-vector handoff.
- **Failed `ms-retry` bundle:** prefix scan + deferred-unpinned maintenance +
  up to 64 pressure retries + an advancement/registry scan every 256 pins. It
  is worse at consumer-heavy shapes. Because it changes several mechanisms, it
  rejects the bundle but does not isolate which component caused the loss. Its
  maintenance runs only after leaving the protected region, so no
  component-level failure mechanism follows from these aggregate results.
- **Boundary:** at 1:1 legacy and prefix both remain around 530 MB. This is
  consistent with insufficient advancement attempts, not proven; leave it
  unresolved. Legacy h3 4:4 ranges 15–56 MB, which shows variability but does
  not establish a bimodal/tipping-point distribution.

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

- Michael–Scott's pointer algorithm has lock-free enqueue/dequeue. This
  implementation also allocates and performs vector-backed EBR maintenance
  with a cold-path orphan mutex, so do not assign the complete operation a pure
  formal lock-free guarantee.
- Vyukov bounded ring: mutex-free CAS reservation, **explicitly not formally
  lock-free** per its author. A thread can reserve before publishing its cell.
- FAA/ticket: wait-free ticket acquisition only; completion is blocking-on-cell
  because tickets are irrevocable.
- Moodycamel: contextual industrial arm; not globally linearizable across
  producers, and constructor capacity is a preallocation estimate.
- Baselines: bounded mutex+queue (blocking); SPSC (1:1 specialization only).
