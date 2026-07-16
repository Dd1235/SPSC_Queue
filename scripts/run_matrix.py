#!/usr/bin/env python3
"""Run the MPMC study's experiment matrix.

Drives benchmarks/bench_mpmc (one process per trial -> one CSV row) with the
thermal discipline the fanless M2 requires:

  * WITHIN-SHAPE interleaving: each trial round iterates the shapes and runs
    every queue back-to-back inside a shape.  The committed paper protocol uses
    a fixed queue order; --rotate-queues enables deterministic rotation for new
    studies.  Cross-queue observations within a shape are close in time.
  * cooldown gaps between trials and rounds;
  * per-trial spin-calibration is recorded by the binary itself (calib_ns
    column) so plots can screen for thermal drift after the fact;
  * each trial runs under a hard timeout; any failed arm makes the command fail.

Trial 0 of every config is warmup by convention and dropped by make_plots.py.

Usage:
  python3 scripts/run_matrix.py --out paper/data/matrix_v1.csv \
      [--bench build/benchmarks/bench_mpmc] [--trials 5] [--seconds 2] [--smoke]
"""

import argparse
import csv
import math
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_QUEUES = ["ms", "vyukov", "vyukov-b", "faa", "casticket", "mutex", "moody"]
FOCUS_QUEUES = {
    "h3": ["ms", "ms-fix", "ms-retry", "mutex"],
    "h3s": ["ms", "ms-fix", "ms-retry"],
    "load": ["ms", "vyukov", "faa", "casticket", "mutex", "moody"],
    "load2": ["ms", "vyukov", "faa", "casticket", "mutex", "moody"],
    "ind": ["rigtorp", "xenium", "faa", "ms", "vyukov", "mutex", "moody"],
}
KNOWN_QUEUES = set(DEFAULT_QUEUES) | {"ms-fix", "ms-retry", "spsc", "rigtorp", "xenium"}
CSV_KEY_COLUMNS = [
    "queue", "mode", "producers", "consumers", "oversubscribe", "capacity",
    "qos", "seconds", "rate", "trial",
]


def shapes(smoke: bool, focus: str = ""):
    """Yield dicts of shape parameters (everything except queue + trial)."""
    if focus == "h3":
        # F8 reclamation A/B/C: the RSS-pathology ratios, dedicated cores.
        for pc in ((1, 1), (4, 4), (1, 7), (2, 6)):
            yield dict(mode="throughput", producers=pc[0], consumers=pc[1],
                       oversubscribe=1, qos="none")
        return
    if focus == "h3s":
        # F8 mechanism instrumentation: same shapes as h3, run against the
        # STATS twin binary (pass --bench .../bench_mpmc_stats) so the ebr_*
        # columns are populated. Mutex omitted: it has no EBR domain.
        for pc in ((1, 1), (4, 4), (1, 7), (2, 6)):
            yield dict(mode="throughput", producers=pc[0], consumers=pc[1],
                       oversubscribe=1, qos="none")
        return
    if focus == "load":
        # Optional offered-rate extension: schedule-to-dequeue latency, 4P:4C x1.
        for rate in (250_000, 500_000, 1_000_000, 2_000_000, 4_000_000):
            yield dict(mode="latency", producers=4, consumers=4, oversubscribe=1,
                       qos="none", rate=rate)
        return
    if focus == "load2":
        # Saturation extension: the 0.25-4M sweep never saturated any arm
        # (delivered fraction 1.0 everywhere), so push the offered rate toward
        # each design's measured 4:4 throughput to expose the knees.
        for rate in (6_000_000, 8_000_000, 12_000_000, 16_000_000, 24_000_000):
            yield dict(mode="latency", producers=4, consumers=4, oversubscribe=1,
                       qos="none", rate=rate)
        return
    if focus == "ind":
        # Industrial cross-check: rigtorp's ticket/turn MPMCQueue (tests the
        # slot-discipline claim with independent engineering) and xenium's
        # Michael-Scott + hazard pointers (tests whether the F8 memory
        # pathology is EBR-specific). Shapes reuse the study's key axes.
        for pc in ((1, 1), (4, 4), (2, 6), (1, 7)):
            yield dict(mode="throughput", producers=pc[0], consumers=pc[1],
                       oversubscribe=1, qos="none")
        for over in (2, 4):
            yield dict(mode="throughput", producers=4, consumers=4,
                       oversubscribe=over, qos="none")
        yield dict(mode="throughput", producers=4, consumers=4,
                   oversubscribe=4, qos="none", capacity=64)
        for over in (1, 4):
            yield dict(mode="latency", producers=4, consumers=4,
                       oversubscribe=over, qos="none", rate=1_000_000)
        return
    if focus == "h1":
        # The F1-attribution subset: oversubscription sweep + capacity control +
        # the 1:1/2:2 cliff + QoS-policy extremes + paced tails. ~8 arms x 12 shapes.
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


def is_documented_skip(shape, queue):
    """Moodycamel arms documented as incompatible with this harness.

    cap64/x4 throughput wedges during drain; latency at saturated offered
    loads (>= 8M msg/s) probabilistically strands messages because pills can
    overtake older data across sub-queues, tripping the accounting invariant.
    See dataset_utils._known_unsupported for the analysis-side twin.
    """
    if queue != "moody" or shape["qos"] != "none":
        return False
    if (shape["mode"] == "throughput" and shape["producers"] == 4
            and shape["consumers"] == 4 and shape["oversubscribe"] == 4
            and shape.get("capacity", 1024) == 64):
        return True
    return (
        shape["mode"] == "latency"
        and shape["producers"] == 4
        and shape["consumers"] == 4
        and shape["oversubscribe"] == 1
        and shape.get("capacity", 1024) == 1024
        and shape.get("rate", 0) >= 8_000_000
    )


def queues_for(shape, queues):
    qs = list(queues)
    if shape["producers"] == 1 and shape["consumers"] == 1 and shape["oversubscribe"] == 1:
        qs.append("spsc")  # the 1:1 baseline row
    return [queue for queue in qs if not is_documented_skip(shape, queue)]


def float_key(value):
    """Canonical, exact key for a CSV double (works with old decimal rows too)."""
    return float(value).hex()


def run_key(shape, queue, trial, seconds):
    return (
        queue,
        shape["mode"],
        int(shape["producers"]),
        int(shape["consumers"]),
        int(shape["oversubscribe"]),
        int(shape.get("capacity", 1024)),
        shape["qos"],
        float_key(seconds),
        float_key(shape.get("rate", 1_000_000)),
        int(trial),
    )


def row_key(row):
    try:
        return (
            row["queue"],
            row["mode"],
            int(row["producers"]),
            int(row["consumers"]),
            int(row["oversubscribe"]),
            int(row["capacity"]),
            row["qos"],
            float_key(row["seconds"]),
            float_key(row["rate"]),
            int(row["trial"]),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"malformed existing CSV row: {exc}") from exc


def read_existing(out):
    with out.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"existing output is empty or lacks a CSV header: {out}")
        missing = [column for column in CSV_KEY_COLUMNS if column not in reader.fieldnames]
        if missing:
            raise ValueError(f"existing output lacks columns {missing}: {out}")
        rows = list(reader)
    keys = [row_key(row) for row in rows]
    if len(keys) != len(set(keys)):
        raise ValueError(f"existing output contains duplicate configuration/trial rows: {out}")
    return reader.fieldnames, rows, set(keys)


def rewrite_complete_prefix(out, fieldnames, rows, keep_trials):
    temp = out.with_name(out.name + ".resume-tmp")
    with temp.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(row for row in rows if int(row["trial"]) in keep_trials)
    temp.replace(out)


def prepare_output(out, planned, trials, overwrite, resume):
    """Refuse accidental append; resume only after complete round boundaries."""
    if overwrite and out.exists():
        out.unlink()
    if not out.exists():
        return set()
    if not resume:
        raise ValueError(
            f"output already exists: {out} (choose a new path, --overwrite, or --resume)"
        )

    fieldnames, rows, existing = read_existing(out)
    planned_set = set(planned)
    extras = existing - planned_set
    if extras:
        raise ValueError(
            f"existing output contains {len(extras)} rows outside this matrix; "
            "use a different path or --overwrite"
        )

    keep_trials = set()
    for trial in range(trials):
        expected = {key for key in planned_set if key[-1] == trial}
        actual = {key for key in existing if key[-1] == trial}
        if actual != expected:
            break
        keep_trials.add(trial)
    kept = {key for key in existing if key[-1] in keep_trials}
    if kept != existing:
        removed = len(existing - kept)
        rewrite_complete_prefix(out, fieldnames, rows, keep_trials)
        print(f"resume: discarded {removed} rows from an incomplete trailing round", flush=True)
    if keep_trials:
        print(f"resume: keeping complete rounds 0..{max(keep_trials)}", flush=True)
    return kept


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
    ap.add_argument("--queues",
                    help="comma-separated queue list (focus profiles have tailored defaults)")
    ap.add_argument("--smoke", action="store_true", help="tiny matrix for pipeline testing")
    ap.add_argument("--focus", default="",
                    choices=["", "h1", "h3", "h3s", "load", "load2", "ind"],
                    help="named shape subset (h1/h3/load)")
    output_mode = ap.add_mutually_exclusive_group()
    output_mode.add_argument("--overwrite", action="store_true",
                             help="replace an existing output CSV")
    output_mode.add_argument("--resume", action="store_true",
                             help="resume after the last complete trial round")
    ap.add_argument("--cooldown", type=float, default=0.3,
                    help="seconds between benchmark processes")
    ap.add_argument("--round-cooldown", type=float, default=2.0,
                    help="additional seconds between trial rounds")
    ap.add_argument(
        "--rotate-queues",
        action="store_true",
        help="deterministically rotate queue order by shape/trial (new studies; paper data used fixed order)",
    )
    args = ap.parse_args()

    if args.trials < 2:
        ap.error("--trials must be at least 2 (trial 0 is warm-up)")
    if not math.isfinite(args.seconds) or not math.isfinite(args.timeout) or \
       args.seconds <= 0 or args.timeout <= 0:
        ap.error("--seconds and --timeout must be positive")
    if not math.isfinite(args.cooldown) or not math.isfinite(args.round_cooldown) or \
       args.cooldown < 0 or args.round_cooldown < 0:
        ap.error("cooldowns must be non-negative")
    if args.smoke and args.focus:
        ap.error("--smoke and --focus are separate matrix profiles; choose one")

    bench = Path(args.bench)
    if not bench.exists():
        ap.error(f"benchmark binary not found: {bench} (configure/build benchmarks first)")
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    default_queues = FOCUS_QUEUES.get(args.focus, DEFAULT_QUEUES)
    queues = [q.strip() for q in (args.queues or ",".join(default_queues)).split(",") if q.strip()]
    if not queues:
        ap.error("--queues must name at least one queue")
    if len(queues) != len(set(queues)):
        ap.error("--queues contains duplicates")
    unknown = sorted(set(queues) - KNOWN_QUEUES)
    if unknown:
        ap.error("unknown queue(s): " + ", ".join(unknown))
    if "spsc" in queues:
        ap.error("spsc is added automatically only for the 1:1 baseline")

    # Drop the moody arm automatically when the binary was built without it.
    if "moody" in queues:
        try:
            probe = subprocess.run([str(bench), "--queue", "moody", "--producers", "1",
                                    "--consumers", "1", "--seconds", "0.05"],
                                   capture_output=True, text=True, timeout=min(args.timeout, 10))
        except subprocess.TimeoutExpired:
            ap.error("moodycamel capability probe timed out")
        if probe.returncode != 0:
            print("note: binary lacks moodycamel support; dropping 'moody' arm", flush=True)
            queues.remove("moody")
    if not queues:
        ap.error("no runnable MPMC queues remain after benchmark capability checks")

    shape_list = list(shapes(args.smoke, args.focus))
    skipped = sum(
        1 for shape in shape_list for queue in queues if is_documented_skip(shape, queue)
    ) * args.trials

    planned_runs = []
    for trial in range(args.trials):
        for shape_index, shape in enumerate(shape_list):
            ordered = queues_for(shape, queues)
            if args.rotate_queues and ordered:
                offset = (trial + shape_index) % len(ordered)
                ordered = ordered[offset:] + ordered[:offset]
            for queue in ordered:
                planned_runs.append((trial, shape, queue, run_key(shape, queue, trial, args.seconds)))

    try:
        completed = prepare_output(
            out,
            [item[3] for item in planned_runs],
            args.trials,
            args.overwrite,
            args.resume,
        )
    except ValueError as exc:
        ap.error(str(exc))

    total = len(planned_runs)
    print(f"matrix: {len(shape_list)} shapes, queues={','.join(queues)}, "
          f"{args.trials} trials = {total} runs", flush=True)
    if skipped:
        print(f"documented exclusion: skipped {skipped} moody/cap64/4P:4C/x4 runs",
              flush=True)
    print("queue order: " + ("deterministic rotation" if args.rotate_queues
                             else "fixed (paper-compatible legacy order)"), flush=True)

    t0 = time.time()
    done = len(completed)
    fails = 0
    for trial in range(args.trials):
        trial_runs = [item for item in planned_runs if item[0] == trial and item[3] not in completed]
        if not trial_runs:
            continue
        print(f"== trial round {trial} ==", flush=True)
        for _, shape, queue, _ in trial_runs:
            ok = run_one(bench, shape, queue, trial, args.seconds, out, args.timeout)
            fails += 0 if ok else 1
            done += 1
            time.sleep(args.cooldown)
        time.sleep(args.round_cooldown)
        print(f"   round {trial} done ({done}/{total}, {fails} failures, "
              f"{time.time()-t0:.0f}s elapsed)", flush=True)

    try:
        _, _, written = read_existing(out)
    except ValueError as exc:
        ap.error(str(exc))
    missing = set(item[3] for item in planned_runs) - written
    print(f"matrix complete: {len(written)}/{total} rows, {fails} process failures, "
          f"{time.time()-t0:.0f}s -> {out}", flush=True)
    if missing:
        print(f"ERROR: output is missing {len(missing)} planned rows; resume with --resume",
              file=sys.stderr)
    if fails and not missing:
        print(f"ERROR: {fails} benchmark process(es) exited unsuccessfully",
              file=sys.stderr)
    if missing or fails:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
