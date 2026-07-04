# Claims ledger — the source of truth

Every claim the paper will make, its status, and the data that backs it.
Nothing enters main.tex without a row here. Status: HYPOTHESIS → MEASURED
(with CSV pointer) → VERIFIED (stable across ≥2 full matrix runs).

## Central hypotheses (set before measurement — keeps us honest)

| # | Claim | Status | Evidence |
|---|-------|--------|----------|
| H1 | Progress guarantees are invisible under dedicated cores (≤8 threads) but dominate tail latency under oversubscription: the FAA/ticket queue's blocking-on-slot design degrades disproportionately vs Michael–Scott (lock-free) at ×2/×4 oversubscription. | HYPOTHESIS | — |
| H2 | P/E-core asymmetry (steered via macOS QoS) shifts the throughput ranking of the designs; results measured on symmetric x86 servers do not transfer unchanged to client ARM. | HYPOTHESIS | — |
| H3 | Node-based MS pays allocation + pointer-chasing costs that array queues avoid, but its unboundedness converts back-pressure stalls into memory growth — a different failure mode, not a faster/slower verdict. | HYPOTHESIS | — |
| H4 | On a fanless machine, uncontrolled trial ordering biases cross-queue comparisons measurably (thermal drift); round-robin interleaving bounds this. | HYPOTHESIS (methodology) | — |
| H5 | The ARM weak memory model makes memory-ordering bugs *observable* that x86 masks; TSan + per-producer sequence invariants are the validation floor. | SUPPORTED (SPSC phase precedent) | learn/03 §7; CI matrix |

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
