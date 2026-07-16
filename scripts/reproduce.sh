#!/usr/bin/env bash
# One-command reproduction of the MPMC study's dataset and figures.
# Usage: scripts/reproduce.sh [tag]   (default tag: repro)
# Produces paper/data/matrix_<tag>.csv, paper/data/summary_<tag>.md,
# paper/data/manifest_<tag>.json, and applicable paper figures.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:-repro}"
TRIALS="${TRIALS:-10}"      # process runs/config; trial 0 is warmup (9 retained)
SECONDS_PER="${SECONDS_PER:-2}"
BUILD_DIR="${BUILD_DIR:-build}"
PYTHON="${PYTHON:-python3}"

if [[ ! "${TAG}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo "invalid tag '${TAG}': use letters, digits, dot, underscore, or hyphen" >&2
    exit 2
fi
if [[ ! "${TRIALS}" =~ ^[0-9]+$ ]] || (( TRIALS < 2 )); then
    echo "TRIALS must be at least 2 because trial 0 is warmup" >&2
    exit 2
fi
if [[ ! "${SECONDS_PER}" =~ ^[0-9]+([.][0-9]+)?$ ]] || \
   [[ "${SECONDS_PER}" =~ ^0+([.]0+)?$ ]]; then
    echo "SECONDS_PER must be a positive number" >&2
    exit 2
fi

DATASET="paper/data/matrix_${TAG}.csv"
SUMMARY="paper/data/summary_${TAG}.md"
MANIFEST="paper/data/manifest_${TAG}.json"

if [[ -e "${DATASET}" || -e "${SUMMARY}" || -e "${MANIFEST}" ]] || \
   compgen -G "paper/assets/fig_*_${TAG}.*" >/dev/null; then
    echo "tag '${TAG}' already has outputs; choose a new tag to avoid pooling runs" >&2
    exit 2
fi

command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }
command -v "${PYTHON}" >/dev/null || { echo "Python interpreter not found: ${PYTHON}" >&2; exit 2; }
"${PYTHON}" -c 'import pandas, matplotlib' 2>/dev/null || {
    echo "Python plotting dependencies are missing; install requirements-paper.txt" >&2
    exit 2
}

echo "== configure + build (thirdparty ON for the moodycamel arm) =="
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
    -DSPSC_BENCH_THIRDPARTY=ON -DSPSC_BUILD_DEMOS=OFF
cmake --build "${BUILD_DIR}" --target bench_mpmc --parallel

echo "== correctness gate (fast) =="
cmake --build "${BUILD_DIR}" --parallel >/dev/null
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "== matrix: ${TRIALS} process runs/config ($((TRIALS - 1)) measured) x ${SECONDS_PER}s =="
"${PYTHON}" scripts/run_matrix.py --bench "${BUILD_DIR}/benchmarks/bench_mpmc" \
    --trials "${TRIALS}" --seconds "${SECONDS_PER}" --out "${DATASET}"

echo "== figures + summary =="
"${PYTHON}" scripts/make_plots.py --csv "${DATASET}" --tag "${TAG}"

echo "== provenance manifest =="
"${PYTHON}" scripts/write_manifest.py --tag "${TAG}" --trials "${TRIALS}" \
    --seconds "${SECONDS_PER}" --build-dir "${BUILD_DIR}" --dataset "${DATASET}" \
    --output "${MANIFEST}"

echo "done: ${DATASET}, ${SUMMARY}, ${MANIFEST}, paper/assets/fig_*_${TAG}.*"
