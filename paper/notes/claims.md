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

## F8-M — the mechanism, measured (matrix_h3s, stats build, 7 kept rounds; 2026-07-16)

Three counters, one per link of the hypothesized loop (medians):

| ratio | arm | maint µs/pass | adv success | peak limbo (k) | RSS MB | Mops/s |
|---|---|---|---|---|---|---|
| 1:7 | ms | 79.6 | 0.09% | 129 | 578 | 5.40 |
| 1:7 | ms-fix | 21.3 | 0.22% | 929 | 227 | 10.43 |
| 1:7 | ms-retry | 168.4 | 68.6% | 36 | 645 | 2.61 |
| 1:1 | ms | 4.6 | 99.9% | 1.3 | 553 | 11.70 |
| 1:1 | ms-fix | 4.7 | 99.9% | 3.5 | 598 | 11.69 |

Verdicts:
1. **The fix wins on the cost link, not the epoch link.** Advance success
   stays <0.3% in both modes at 1:7; maintenance drops 79.6 → 21.3 µs/pass.
   Peak limbo is LARGER under the fix (backlog spikes, clears in bulk) while
   total RSS is 2.6× lower. RSS/throughput effects replicate under
   instrumentation (578→227 MB, 5.4→10.4 Mops/s).
2. **Advancement is neither necessary nor sufficient.** ms-retry's probes
   succeed (68.6% at 1:7, smallest limbo peaks) and it still posts the worst
   RSS and throughput with 168 µs passes. The binding quantity is maintenance
   cost in the consumers' pop path.
3. **The 1:1 boundary is resolved: not reclamation.** Both modes show ≥99.8%
   advance success, ~4.6 µs passes, ≤3.5k peak limbo — reclamation is healthy
   — yet ~550–600 MB RSS. Remaining candidate: live-queue growth (producer
   outruns the consumer, which pays retire+maintain per pop). Not directly
   instrumented (no live-node counter); stated as exoneration + inference.

## F9 — saturation knees (matrix_load + matrix_load2, 5+6 kept rounds; 2026-07-16)

p50 medians (µs) at 4:4 ×1 cap 1024, offered rate sweep 0.25–24M msg/s;
scheduled-time payloads mean post-knee values measure backlog (seconds scale):

- Knees: **ms and mutex 4–6M; vyukov 6–8M; casticket ~8–12M; faa 8–12M** —
  same order as saturated-mode throughput. FAA p50 at 8M offered = 0.4 µs
  while casticket = 24.7 µs and every other arm ≥ ms-scale.
- **Mutex uniquely degrades gracefully pre-knee**: p50 0.8→3.8→14.2→52.5 µs
  across 0.25/1/2/4M. Reservation designs run flat (≤1 µs) then cliff.
- Below saturation (≤4M) every design delivers sub-ms p99; ordering there is
  scheduler-noise dominated (large IQRs) — the knee, not the sub-knee tail,
  is the informative statistic.

**Edge finding (moody, latency ≥8M):** past its knee, moody probabilistically
strands 1–162 messages (pills overtake older data across sub-queues — its
documented ordering semantics violate the drain protocol's assumption); the
exact-accounting invariant aborts (exit 3) in 6/42 arm-rounds. Excluded
symmetrically (dataset_utils + run_matrix documented skip); its
exact-accounting curve ends at 6M, unsaturated. The hardened invariant turned
a silently biased tail into a loud, documented exclusion.

## F10 — industrial ticket queue: wait policy is existential (matrix_ind, k=8; 2026-07-16)

rigtorp::MPMCQueue = independently engineered FAA-ticket/turn queue whose slot
waits BUSY-SPIN (ours spin-1024-then-yield). Same structure, different wait
policy (medians, cap 1024):

| shape | faa | rigtorp | verdict |
|---|---|---|---|
| 1:1 ×1 | 125.6 | 120.3 | structures agree at low contention (−4%) |
| 4:4 ×1 | 16.4 | 9.6 | busy-spin −41% even on dedicated cores |
| 4:4 ×2 | 19.2 | 0.12 | collapse |
| 4:4 ×4 | **29.6** | **0.04** | ~820× apart; rigtorp worse than Vyukov (1.66) |
| paced 1M ×1 p50 | 0.4 µs | 0.4 µs | below saturation they match |
| paced 1M ×4 p50 | 0.9 ms | **25.7 s** | can't sustain 1M offered at ×4 |

**Verdict: interaction, not main effects.** vyukov-b showed politeness is
irrelevant in the CAS-retry structure; rigtorp shows it is EXISTENTIAL in the
ticket structure — irrevocable tickets mean only yielding waiters let a
preempted claimant complete its turn. Slot discipline + yielding waits =
robust; slot discipline + busy spin = worst arm measured; shared-cursor retry
± politeness = mediocre either way. Neither structure nor policy alone
predicts. (Fairness to rigtorp: its docs target low-latency low-contention
use; our 375 ns paced p50 confirms that regime.)

## F11 — reclamation-scheme cross-check: HP doesn't save unboundedness (matrix_ind, k=8)

xenium michael_scott_queue + hazard_pointer (bounded garbage by construction)
vs our MS+EBR-legacy, same rounds:

| shape | ms (EBR) RSS | xenium (HP) RSS | ms tput | xenium tput |
|---|---|---|---|---|
| 1:1 | 653 | 421 | 11.10 | 11.34 |
| 2:6 | 662 | 548 | 5.85 | 3.32 |
| 1:7 | 944 | **760** | 4.75 | 2.60 |

- **The memory failure mode survives a scheme swap.** HP bounds garbage to
  O(threads×K+batch), so ~760 MB at 1:7 must be dominated by LIVE nodes —
  independent confirmation of F8-M's conclusion that unboundedness + rate
  mismatch, not reclamation mechanics, is first-order. (Our prefix fix's
  153–227 MB at 1:7 remains the best MS result; the fixable part was
  EBR-specific, the floor is not.)
- **HP's per-traversal cost lands on consumers:** throughput parity at 1:1
  (+2%), −45% at 1:7 — cost grows with consumer parallelism, the textbook
  HP-vs-EBR tradeoff measured on client silicon (Hart et al. one level down).
- Bonus corner: at cap64 ×4 the unbounded arms win (ms 4.94, xenium 3.64 vs
  faa 2.46, mutex 4.54, vyukov 0.88) — no backpressure to fight.

## T — sustained-load thermal series (matrix_thermal, 4 designs × 30 × 10 s; 2026-07-16)

Protocol: per design, thirty back-to-back 10 s process trials at 4:4 ×1 cap
1024 (heat IS the treatment); 180 s cooldown between designs; fixed order
faa→vyukov→ms→mutex; each design's early-soak calib returning to ~95–96 ms
bounds order carryover. Median of last 5 soaks vs median of first 3 (cold):

| design | hot/cold throughput | calib cold→hot (ms) |
|---|---|---|
| faa | **101.9%** | 95.5 → 95.9 (flat) |
| mutex | 91.4% | 96.0 → 108.2 |
| vyukov | 90.6% | 95.4 → 124.4 (ends 213) |
| ms | 81.3% | 96.1 → 116.9 |

- **T1 (decay):** design-dependent, 0–19% over ~5 min. The FAA ticket queue
  is thermally flat AND leaves the probe cold — its yielding slot waits keep
  the SoC inside its passive budget; the spin/retry designs heat the chip and
  shed 9–19%.
- **T2 (ranking):** the 4:4 ordering (mutex > faa > vyukov > ms) shows no
  crossings across the soak — sustained heat does not reorder this shape.
- **T3 (attribution):** ms decay (−19%) is commensurate with its calib climb
  (+22%); vyukov decays less (−9%) than its calib suggests (+30%) — spin
  throughput is less frequency-sensitive than the probe; faa flat/flat.
- Artifacts reported, not hidden: 3 of 120 soaks are isolated near-zero
  trials (external interference); last-5 medians are robust to them.
  Ambient/charger uncontrolled (lid open, on charger, indoor ambient).

## P — observed clusters and energy (matrix_power, powermetrics, 6 kept rounds; 2026-07-17)

Protocol: run_power.py brackets each 2 s 4:4 trial with a powermetrics
sampler (250 ms, cpu_power); medians below. Machine-wide instrument: cluster
residency/power include the sampler and system activity — an attribution
limit, stated wherever quoted.

| policy | queue | Mops/s | E/P resid % | P-freq MHz | pkg mW | nJ/op |
|---|---|---|---|---|---|---|
| none | faa | 17.1 | 62/66 | 1606 | 1721 | **99.7** |
| none | mutex | 17.4 | 62/67 | 1726 | 1939 | 111.5 |
| none | vyukov | 8.1 | 63/68 | 1716 | 1997 | 246.2 |
| none | ms | 6.8 | 63/67 | 1662 | 3033 | 441.2 |
| all-bg | faa | 16.5 | 67/62 | 1454 | **837** | **52.6** |
| all-bg | vyukov | 0.55 | 64/63 | 1531 | 896 | **1616.4** |

- **P1 — REFUTED AS STATED, reframed.** Background QoS does NOT empty the
  P-cluster: machine-wide residency stays ~60–67% on BOTH clusters under
  all-bg. What changes is frequency (P −9–20% for 4/5 designs; ms flat,
  E −13%) and package power (roughly halves for every design,
  1.7–3.0 W → 0.84–1.2 W). The F2 ranking reversal is at
  least partly a frequency/power-budget effect; per-thread placement remains
  unobserved. The paper's refusal to call F2 an "E-core result" is now
  measured, not just cautious.
- **P2 — energy per op is a decisive axis.** Default policy: faa 100 nJ/op,
  mutex 112, vyukov 246, moody 292, ms 441. Under all-bg, faa improves
  99.7→52.6 nJ/op (−47%) at −4% throughput — the only design to gain while
  keeping throughput; ms also edges down (441→392, −11%, at −52%
  throughput); vyukov worsens 6.6× (246→1616); mutex 2.4×; moody +10%.
- **P3 — power ranking mirrors the thermal soak.** ms is the power hog
  (3.0 W default; highest at every policy); faa the lightest (1.7 W).
  Third instrument agreeing with T (probe) and F10 (wait policy): spinning
  and reclamation churn burn the budget; yielding hands it back.
- Caveats: package-level attribution (otherwise-idle assumption; sampler
  included), cluster-level machine-wide residency, k=6, nJ/op uses bench
  elapsed (includes drain).

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
