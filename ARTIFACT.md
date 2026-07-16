# Artifact: MPMC Queue Tradeoffs on an Apple M2 System

This repository contains the implementations, one-process-per-trial harness,
raw datasets, analysis/figure pipeline, claim ledger, and paper source. The
reported collection platform was a Mac14,2 MacBook Air (Apple M2, 4 performance
+ 4 efficiency cores, 8 GB), macOS 15.6.1 (24G90), with Apple clang 17. The
historical CSV rows do not encode OS/build fields; the exact OS values were
recorded when the same machine was inspected during artifact finalization.

## Requirements

- macOS on Apple silicon for the headline protocol. QoS policy effects are
  macOS-specific; the harness does not observe or claim exact P/E-core
  residency. Linux runs the same harness with QoS calls as no-ops and is a
  directional check only.
- CMake >= 3.16 and a C++17 Clang/GCC toolchain.
- Python 3.10+ with the figure dependencies used for the committed assets:

  ```sh
  python3 -m venv .venv
  . .venv/bin/activate
  python3 -m pip install -r requirements-paper.txt
  ```

- Network access on the first third-party configure. The reference sources are
  pinned by immutable Git commit in `benchmarks/CMakeLists.txt`.
- `tectonic` only if you also want to compile `paper/main.tex` (optional).

## One-command fresh reproduction

Run from a normal foreground shell on an otherwise idle machine:

```sh
scripts/reproduce.sh mytag
```

The default performs 10 processes per configuration: trial 0 is warm-up and 9
measured trials are retained. Set `TRIALS=11` for 10 retained trials or
`SECONDS_PER=3` to change process duration. A tag is write-once: the script
refuses existing outputs instead of silently appending a second experiment.

Outputs are:

- `paper/data/matrix_mytag.csv` (raw configuration rows),
- `paper/data/summary_mytag.md` (validated aggregate tables),
- `paper/data/manifest_mytag.json` (privacy-safe hardware/toolchain/revision
  provenance and output hashes), and
- only the `paper/assets/fig_*_mytag.{pdf,png}` figures supported by that
  dataset profile.

The current 1,270-process default matrix takes roughly 55–75 minutes on the
artifact machine, depending mainly on drain time. The
manifest records only whitelisted model/chip/core/memory/OS/compiler fields; it
never captures serial numbers, hardware UUIDs, hostnames, or raw
`system_profiler` output.

## Step-by-step

```sh
# Configure and build the study binary with the pinned moodycamel arm.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSPSC_BENCH_THIRDPARTY=ON -DSPSC_BUILD_DEMOS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Consolidated current full profile: 1 warm-up + 9 measured trials.
python3 scripts/run_matrix.py --trials 10 --seconds 2 \
  --out paper/data/matrix_mytag.csv

# Validate complete rounds/samples, then aggregate and plot.
python3 scripts/make_plots.py \
  --csv paper/data/matrix_mytag.csv --tag mytag

# Optional paper PDF.
tectonic paper/main.tex
```

Useful focused profiles:

```sh
# F1 controls (the h1 profile defaults to all comparison arms).
python3 scripts/run_matrix.py --focus h1 --trials 8 --seconds 2 \
  --out paper/data/matrix_controls.csv

# F8 reclamation A/B/C. This profile defaults to ms,ms-fix,ms-retry,mutex;
# SPSC is added automatically at the valid 1:1 shape.
python3 scripts/run_matrix.py --focus h3 --trials 8 --seconds 2 \
  --out paper/data/matrix_reclamation.csv

# Mechanism counters for the h1 shapes.
python3 scripts/run_matrix.py --focus h1 \
  --bench build/benchmarks/bench_mpmc_stats --trials 5 --seconds 1.5 \
  --out paper/data/matrix_mechanism.csv

# Offered-load curve (tailored six-arm default).
python3 scripts/run_matrix.py --focus load --trials 6 --seconds 2 \
  --out paper/data/matrix_load_repro.csv

# Industrial cross-checks (rigtorp MPMCQueue; xenium Michael-Scott with
# hazard pointers). Requires SPSC_BENCH_THIRDPARTY=ON.
python3 scripts/run_matrix.py --focus ind --trials 9 --seconds 2 \
  --out paper/data/matrix_ind_repro.csv

# Observed cluster residency + energy per op (macOS; powermetrics needs root:
# run `sudo -v` first, or the script itself with sudo).
python3 scripts/run_power.py --out paper/data/matrix_power_repro.csv

# Sustained-load thermal series on the fanless machine (~30 min).
python3 scripts/run_thermal.py --out paper/data/matrix_thermal_repro.csv

# 95% bootstrap percentile intervals quoted next to headline claims.
python3 scripts/bootstrap_ci.py
```

Interrupted runs may be continued with `--resume`. Resume discards an
incomplete trailing trial and restarts that whole round, preserving within-round
comparability. `--overwrite` is explicit and destructive. Any unexpected
process failure or missing row makes `run_matrix.py` exit nonzero.

The committed paper datasets used a fixed queue order for exact protocol
reproduction. New exploratory studies can add `--rotate-queues` to
deterministically rotate order across shapes and rounds; do not mix the two
protocols in one CSV.

## Committed datasets and provenance

| Dataset | Role | Data-generating revision |
|---|---|---|
| `paper/data/matrix_v1.csv` | first full matrix; replication check | `250c0c18e16735db143c2e8eb1a2343aafafaab3` |
| `paper/data/matrix_v2.csv` | canonical broad matrix | `d5dd78b81a85d44695231ba7ab97ddcf13717dde` |
| `paper/data/matrix_v3.csv` | F1 control-arm matrix | `c840a933c9bbf41a2374610dbc64bc538ec71e87` |
| `paper/data/matrix_v3s.csv` | instrumented mechanism runs | `c840a933c9bbf41a2374610dbc64bc538ec71e87` |
| `paper/data/matrix_h3.csv` | reclamation remediation A/B/C | `52ac4419d11b324c63464b1d39df7023a0fa5606` |

Use the listed revision to inspect the exact historical harness. Current HEAD
contains the hardened reproduction protocol, but is not claimed to be
byte-identical to every historical data-generating revision.

The optional local `matrix_load.csv` offered-rate extension and generated load
figures are not paper claim sources. Their full data-generating revision was
not captured, so they are intentionally excluded from the provenance table.

Rows contain the full experimental configuration, trial, timestamp, and
measurements. Historical CSVs do **not** contain the complete host/compiler/Git
environment, so they are configuration-self-contained rather than fully
self-describing; the platform above and paper methodology supply that context.
Fresh `reproduce.sh` runs add the JSON manifest.

## Statistical selection and claim verification

`scripts/make_plots.py` and `scripts/verify_claims.py` share these gates:

1. Trial 0 defines the expected configuration set and is excluded from
   statistics.
2. Only the contiguous prefix of measured rounds with that exact set is kept.
   Thus the partial trial 8 in `matrix_v2.csv` is excluded automatically.
3. Capacity, offered rate, and process duration remain grouping axes. They are
   never pooled. The headline oversubscription figures explicitly use capacity
   1024 and paced-tail figures use 1 M message/s.
4. The documented incompatible arm—moodycamel, throughput, 4P:4C, x4,
   capacity 64, QoS none—is excluded consistently. The current runner skips it
   before launch.
5. A historical latency row is rejected unless its bounded sample buffers
   contain exactly the scheduled count. Summaries report these exclusions and
   the retained per-configuration sample range.
6. A CSV that mixes durations is rejected unless `--seconds` selects one. For
   `matrix_v3s.csv`, the claim verifier explicitly selects the complete 1.5 s
   run rather than pooling it with the abandoned partial 2.0 s run.

Verify the committed claim transcription with tight, display-rounding-aware
tolerances:

```sh
python3 scripts/verify_claims.py
```

The optional wider-band mode expects a complete five-file canonical profile
set named `matrix_v1.csv`, `matrix_v2.csv`, `matrix_v3.csv`, `matrix_v3s.csv`,
and `matrix_h3.csv` in another directory. It checks the predeclared aggregate
bands, but deliberately does not require the historical v2 scheduler outlier
to recur at the same trial number:

```sh
python3 scripts/verify_claims.py --data-dir /path/to/reproduction --replication
```

It does **not** apply to the single consolidated `matrix_<tag>.csv` emitted by
`reproduce.sh`; analyze that run with `make_plots.py` and its generated summary.
The historical profiles were produced at different revisions, so reproducing
all five is an explicit multi-profile exercise rather than the one-command path.

The claims ledger at `paper/notes/claims.md` maps interpretation to evidence;
the script is the executable check for its registered quantitative values.

## Correctness validation

```sh
cmake -S . -B build-tsan -DSPSC_ENABLE_TSAN=ON \
  -DSPSC_BUILD_BENCHMARKS=OFF -DSPSC_BUILD_DEMOS=OFF
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure

# Repeat with -DSPSC_ENABLE_ASAN=ON and build-asan.
```

The MPMC stress tests check exact count, exactly once, and per-producer FIFO for
the study queues and control variants. TSan and ASan/UBSan are separate builds;
CMake rejects enabling both sanitizer modes together.

## Cross-platform directional check

`.github/workflows/study-matrix.yml` is manually triggered on an Apple-silicon
macOS runner and an x86 Linux runner and uploads CSV artifacts. Hosted machines
are few-core and virtualized, so the result answers only whether the qualitative
inversion reproduces off the artifact machine; it is not a source of headline
numbers.

## Evaluator notes

- The M2 Air is fanless. Shapes are interleaved by trial round, cooldowns are
  explicit, and every process logs an interactive-QoS-biased spin-calibration
  probe (`calib_ns`) for thermal/DVFS screening. QoS is best-effort and is not
  an affinity or observed-residency mechanism.
- macOS worker threads inherit launcher QoS. Run from a normal foreground shell;
  the binary resets its main thread to `QOS_CLASS_DEFAULT` after calibration.
  Historical rows did not record QoS-call status; current HEAD aborts a trial
  if the requested policy cannot be applied.
- PDF figures embed TrueType fonts (`pdf.fonttype=42`) for publication checks.
  Plot generation is profile-aware and does not emit empty focus-inapplicable
  figures.
