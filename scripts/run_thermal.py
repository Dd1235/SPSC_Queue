#!/usr/bin/env python3
"""Sustained-load thermal characterization on the fanless study machine.

Question: does each design's throughput RANKING survive sustained heat, and
how much absolute throughput does a passively cooled SoC shed per design?
Server studies cannot ask this; a fanless client machine makes it a finding.

Protocol (deliberately different from run_matrix's interleaving):
  - Per design, run BACK-TO-BACK 10 s process trials for --minutes minutes
    with no cooldown: consecutive heat soak is the treatment.
  - Between designs, idle for --cooldown seconds so every design starts from
    a comparable cold state (the spin-calibration probe in each row verifies
    this; a design starting warm shows elevated calib in its first trials).
  - Process-per-trial is retained, so each row is an isolated process and the
    within-design time series is the thermal signal.

Output: paper/data/matrix_thermal.csv -- standard bench rows plus a
soak_trial index; elapsed wall position reconstructs from unix_time.
The `trial` column records the soak index (0 = cold start), so dataset_utils'
round-completeness selection does NOT apply to this file; analyze it as a
time series, not as repeated measures.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "build/benchmarks/bench_mpmc"

QUEUES = ["faa", "vyukov", "ms", "mutex"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=5.0,
                    help="sustained minutes per design")
    ap.add_argument("--trial-seconds", type=float, default=10.0)
    ap.add_argument("--cooldown", type=int, default=180,
                    help="idle seconds between designs (cold start each)")
    ap.add_argument("--out", default="paper/data/matrix_thermal.csv")
    ap.add_argument("--queues", default=",".join(QUEUES))
    args = ap.parse_args()

    if not BENCH.is_file():
        sys.exit(f"benchmark not built: {BENCH}")
    queues = [q.strip() for q in args.queues.split(",") if q.strip()]
    out = ROOT / args.out
    out.parent.mkdir(parents=True, exist_ok=True)
    per_design = int(args.minutes * 60 / args.trial_seconds)

    for qi, queue in enumerate(queues):
        if qi > 0:
            print(f"cooldown {args.cooldown}s before {queue}...", flush=True)
            time.sleep(args.cooldown)
        print(f"=== {queue}: {per_design} x {args.trial_seconds:.0f}s back-to-back ===",
              flush=True)
        for soak in range(per_design):
            r = subprocess.run(
                [str(BENCH), "--queue", queue, "--producers", "4",
                 "--consumers", "4", "--capacity", "1024",
                 "--seconds", str(args.trial_seconds), "--trial", str(soak),
                 "--csv", str(out)],
                capture_output=True, text=True, timeout=180,
            )
            if r.returncode != 0:
                sys.exit(f"bench failed ({queue} soak={soak}): {r.stderr.strip()}")
            line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
            print(f"  soak {soak:2d}: {line}", flush=True)
    print(f"thermal series complete -> {out}")


if __name__ == "__main__":
    main()
