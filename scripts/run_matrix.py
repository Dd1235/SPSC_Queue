#!/usr/bin/env python3
"""Run the MPMC study's experiment matrix.

Drives benchmarks/bench_mpmc (one process per trial -> one CSV row) with the
thermal discipline the fanless M2 requires:

  * ROUND-ROBIN interleaving: within each trial round we iterate SHAPES
    (mode/ratio/qos/oversubscription) and run EVERY queue back-to-back inside a
    shape. Cross-queue comparisons -- the thing the paper plots -- are therefore
    taken under near-identical thermal state; slow drift penalizes all queues
    equally instead of whichever happened to run last.
  * cooldown gaps between trials and rounds;
  * per-trial spin-calibration is recorded by the binary itself (calib_ns
    column) so plots can screen for thermal drift after the fact;
  * each trial runs under a hard timeout and failures are logged, not fatal.

Trial 0 of every config is warmup by convention and dropped by make_plots.py.

Usage:
  python3 scripts/run_matrix.py --out paper/data/matrix_v1.csv \
      [--bench build/benchmarks/bench_mpmc] [--trials 5] [--seconds 2] [--smoke]
"""

import argparse
import itertools
import subprocess
import sys
import time
from pathlib import Path

QUEUES = ["ms", "vyukov", "vyukov-b", "faa", "casticket", "mutex", "moody"]  # +spsc at 1:1


def shapes(smoke: bool, focus: str = ""):
    """Yield dicts of shape parameters (everything except queue + trial)."""
    if focus == "h3":
        # F8 reclamation A/B/C: the RSS-pathology ratios, dedicated cores.
        for pc in ((1, 1), (4, 4), (1, 7), (2, 6)):
            yield dict(mode="throughput", producers=pc[0], consumers=pc[1],
                       oversubscribe=1, qos="none")
        return
    if focus == "load":
        # F9: latency-vs-offered-load curves (saturation knees), 4P:4C x1.
        for rate in (250_000, 500_000, 1_000_000, 2_000_000, 4_000_000):
            yield dict(mode="latency", producers=4, consumers=4, oversubscribe=1,
                       qos="none", rate=rate)
        return
    if focus == "h1":
        # The F1-attribution subset: oversubscription sweep + capacity control +
        # the 1:1/2:2 cliff + P/E extremes + paced tails. ~8 arms x 12 shapes.
        for f in (1, 2, 4):
            yield dict(mode="throughput", producers=4, consumers=4, oversubscribe=f,
                       qos="none")
        for cap in (64, 8192):
            yield dict(mode="throughput", producers=4, consumers=4, oversubscribe=4,
                       qos="none", capacity=cap)
        for p, c in ((1, 1), (2, 2)):
            yield dict(mode="throughput", producers=p, consumers=c, oversubscribe=1,
                       qos="none")
        for q in ("all-int", "all-bg"):
            yield dict(mode="throughput", producers=4, consumers=4, oversubscribe=1, qos=q)
        for f in (1, 4):
            yield dict(mode="latency", producers=4, consumers=4, oversubscribe=f,
                       qos="none", rate=1_000_000)
        return
    ratios = [(1, 1), (2, 2), (4, 4), (1, 7), (7, 1), (2, 6), (6, 2)]
    if smoke:
        ratios = [(1, 1), (4, 4)]
    # -- throughput: ratio sweep (dedicated cores, default QoS)
    for p, c in ratios:
        yield dict(mode="throughput", producers=p, consumers=c, oversubscribe=1, qos="none")
    # -- throughput: oversubscription at the balanced 4x4 shape (H1 axis)
    for f in ([2, 4] if not smoke else [4]):
        yield dict(mode="throughput", producers=4, consumers=4, oversubscribe=f, qos="none")
    # -- throughput: QoS placement at 4x4 (H2 axis; 'none' covered above)
    for q in (["all-int", "all-bg", "prod-bg", "cons-bg"] if not smoke else ["all-bg"]):
        yield dict(mode="throughput", producers=4, consumers=4, oversubscribe=1, qos=q)
    # -- throughput: capacity sensitivity at the H1 shape (bounds the pipelining
    #    explanation of FAA's oversubscription gain; 1024 covered above)
    if not smoke:
        for cap in [64, 8192]:
            yield dict(mode="throughput", producers=4, consumers=4, oversubscribe=4,
                       qos="none", capacity=cap)
    # -- latency: paced 1 M msg/s total; dedicated vs oversubscribed (H1 tails)
    lat = [(1, 1, 1), (4, 4, 1), (4, 4, 4)] if not smoke else [(4, 4, 4)]
    for p, c, f in lat:
        yield dict(mode="latency", producers=p, consumers=c, oversubscribe=f, qos="none",
                   rate=1_000_000)


def queues_for(shape, queues):
    qs = list(queues)
    if shape["producers"] == 1 and shape["consumers"] == 1 and shape["oversubscribe"] == 1:
        qs.append("spsc")  # the 1:1 baseline row
    return qs


def run_one(bench, shape, queue, trial, seconds, out, timeout):
    cmd = [str(bench), "--queue", queue,
           "--producers", str(shape["producers"]),
           "--consumers", str(shape["consumers"]),
           "--oversubscribe", str(shape["oversubscribe"]),
           "--qos", shape["qos"],
           "--mode", shape["mode"],
           "--seconds", str(seconds),
           "--capacity", str(shape.get("capacity", 1024)),
           "--trial", str(trial),
           "--csv", str(out)]
    if "rate" in shape:
        cmd += ["--rate", str(shape["rate"])]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
        print(f"    {line}", flush=True)
        if r.returncode != 0:
            print(f"    !! exit {r.returncode}: {r.stderr.strip()}", file=sys.stderr, flush=True)
        return r.returncode == 0
    except subprocess.TimeoutExpired:
        print(f"    !! TIMEOUT {queue} {shape}", file=sys.stderr, flush=True)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default="build/benchmarks/bench_mpmc")
    ap.add_argument("--out", default="paper/data/matrix_v1.csv")
    ap.add_argument("--trials", type=int, default=5,
                    help="trials per config INCLUDING the warmup trial 0")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--queues", default=",".join(QUEUES))
    ap.add_argument("--smoke", action="store_true", help="tiny matrix for pipeline testing")
    ap.add_argument("--focus", default="", choices=["", "h1", "h3", "load"],
                    help="named shape subset (h1/h3/load)")
    args = ap.parse_args()

    bench = Path(args.bench)
    if not bench.exists():
        sys.exit(f"benchmark binary not found: {bench} (build with -DSPSC_BENCH_THIRDPARTY=ON)")
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    queues = [q for q in args.queues.split(",") if q]
    # Drop the moody arm automatically when the binary was built without it.
    probe = subprocess.run([str(bench), "--queue", "moody", "--producers", "1",
                            "--consumers", "1", "--seconds", "0.05"],
                           capture_output=True, text=True)
    if probe.returncode != 0 and "moody" in queues:
        print("note: binary lacks moodycamel support; dropping 'moody' arm", flush=True)
        queues.remove("moody")

    shape_list = list(shapes(args.smoke, args.focus))
    total = sum(len(queues_for(s, queues)) for s in shape_list) * args.trials
    print(f"matrix: {len(shape_list)} shapes x queues x {args.trials} trials = {total} runs",
          flush=True)

    t0 = time.time()
    done = 0
    fails = 0
    for trial in range(args.trials):
        print(f"== trial round {trial} ==", flush=True)
        for shape in shape_list:
            for queue in queues_for(shape, queues):
                ok = run_one(bench, shape, queue, trial, args.seconds, out, args.timeout)
                fails += 0 if ok else 1
                done += 1
                time.sleep(0.3)  # cooldown between runs
        time.sleep(2.0)  # longer cooldown between rounds
        print(f"   round {trial} done ({done}/{total}, {fails} failures, "
              f"{time.time()-t0:.0f}s elapsed)", flush=True)
    print(f"matrix complete: {done} runs, {fails} failures, {time.time()-t0:.0f}s -> {out}",
          flush=True)


if __name__ == "__main__":
    main()
