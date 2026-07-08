# Artifact: MPMC Queue Tradeoffs on Asymmetric Client Silicon

This repository is the complete artifact for the study: implementations,
harness, raw datasets, figure pipeline, and paper source. One command
regenerates the dataset and every figure.

## Requirements

- macOS on Apple silicon for the headline experiments (QoS steering and the
  P/E-core axes are macOS-specific; the harness builds and runs on Linux with
  those axes as no-ops).
- CMake >= 3.16, a C++17 compiler, Python 3 with `pandas` + `matplotlib`.
- Network access at configure time for the `moodycamel::ConcurrentQueue`
  reference arm (pinned v1.0.4 via FetchContent); everything else is
  self-contained.
- `tectonic` to build the paper PDF (optional).

## One-command reproduction (~40 min on an M2 Air)

```sh
scripts/reproduce.sh mytag           # full matrix, k=10, ~35 min + figures
```

Outputs: `paper/data/matrix_mytag.csv`, `paper/data/summary_mytag.md`,
`paper/assets/fig_*_mytag.{pdf,png}`.

## Step-by-step (what reproduce.sh does)

| Step | Command | Expected time |
|---|---|---|
| Build | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_BENCH_THIRDPARTY=ON && cmake --build build -j` | ~1 min |
| Correctness gate | `ctest --test-dir build` | ~2 s (plain); TSan/ASan builds optional, see below |
| Full matrix | `python3 scripts/run_matrix.py --trials 10 --seconds 2 --out paper/data/matrix_X.csv` | ~35 min |
| Focus matrix (controls) | `python3 scripts/run_matrix.py --focus h1 --trials 8 --seconds 2 --out ...` | ~25 min |
| Mechanism dataset | same, with `--bench build/benchmarks/bench_mpmc_stats` | ~15 min at k=4 |
| Figures + tables | `python3 scripts/make_plots.py --csv ... --tag X` | ~5 s |
| Paper | `tectonic paper/main.tex` | ~10 s |

## Datasets in this repository

| File | What it is |
|---|---|
| `paper/data/matrix_v1.csv` | first full matrix (k=5) — replication check |
| `paper/data/matrix_v2.csv` | canonical full matrix (k=8 complete rounds) |
| `paper/data/matrix_v3.csv` | focus matrix incl. the two control arms (k=8) |
| `paper/data/matrix_v3s.csv` | instrumented-build mechanism dataset |
| `paper/data/summary_*.md` | aggregated tables per dataset |

Every row is self-describing (full configuration + trial + environment
columns). Trial 0 of each configuration is warmup and dropped in aggregation.
`paper/notes/claims.md` maps every claim in the paper to its dataset.

## Correctness validation

```sh
cmake -S . -B build-tsan -DSPSC_ENABLE_TSAN=ON -DSPSC_BUILD_BENCHMARKS=OFF -DSPSC_BUILD_DEMOS=OFF
cmake --build build-tsan -j && ctest --test-dir build-tsan   # ~10 s
# same with -DSPSC_ENABLE_ASAN=ON in build-asan
```

The MPMC stress tests check exact-count, exactly-once, and per-producer FIFO
invariants across thread mixes for every queue including both control
variants.

## Cross-platform directional check

`.github/workflows/study-matrix.yml` (manual trigger) runs a reduced matrix on
a GitHub Apple-silicon runner and an x86 runner and uploads CSVs as artifacts.
These runners are few-core and virtualized; the paper uses them only for the
"does the inversion reproduce off-M2?" question.

## Notes for evaluators

- The M2 Air is fanless: the matrix scripts interleave queues round-robin
  within each trial round and log a pinned calibration probe per trial
  (`calib_ns`) so you can screen your own run for thermal drift
  (`fig_calib_*`).
- On macOS, run from a normal foreground shell: worker threads inherit the
  launcher's QoS class (documented in the paper's methodology as confounder
  C2); the binary resets its main thread to `QOS_CLASS_DEFAULT` after
  calibration to defend against this.
- moodycamel at capacity 64 under 4x oversubscription can exceed the per-run
  timeout (documented configuration incompatibility); the script logs and
  skips.
