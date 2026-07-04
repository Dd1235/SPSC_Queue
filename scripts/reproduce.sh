#!/usr/bin/env bash
# One-command reproduction of the MPMC study's dataset and figures.
# Usage: scripts/reproduce.sh [tag]   (default tag: repro)
# Produces paper/data/matrix_<tag>.csv, paper/data/summary_<tag>.md,
# and paper/assets/fig_*_<tag>.{pdf,png}.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:-repro}"
TRIALS="${TRIALS:-10}"      # >= 10 for paper-grade statistics; trial 0 is warmup
SECONDS_PER="${SECONDS_PER:-2}"

echo "== configure + build (thirdparty ON for the moodycamel arm) =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_BENCH_THIRDPARTY=ON
cmake --build build --target bench_mpmc -j

echo "== correctness gate (fast) =="
cmake --build build -j >/dev/null
ctest --test-dir build --output-on-failure

echo "== matrix: ${TRIALS} trials x 2s (interleaved for thermal fairness) =="
python3 scripts/run_matrix.py --trials "${TRIALS}" --seconds "${SECONDS_PER}" \
    --out "paper/data/matrix_${TAG}.csv"

echo "== figures + summary =="
python3 scripts/make_plots.py --csv "paper/data/matrix_${TAG}.csv" --tag "${TAG}"

echo "done: paper/data/matrix_${TAG}.csv, paper/data/summary_${TAG}.md, paper/assets/fig_*_${TAG}.*"
