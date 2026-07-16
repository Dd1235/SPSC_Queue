#!/usr/bin/env python3
"""Observed cluster residency + energy per operation (macOS, Apple silicon).

Runs the QoS arms of the study while sampling `powermetrics` and writes
paper/data/matrix_power.csv with, per trial:

  - the benchmark's own row fields (queue, qos, throughput, ops, ...)
  - e_resid / p_resid : mean HW-active residency of the E- and P-clusters (%)
  - e_freq / p_freq   : mean HW-active frequency of each cluster (MHz)
  - cpu_mw            : mean CPU package power (mW)
  - nj_per_op         : cpu_mw * elapsed / ops, in nanojoules per completed op

Why: the committed matrices record the REQUESTED QoS policy only. Sampling
cluster residency during the run turns "requesting background QoS reverses
the ranking" into an observation about where the work actually executed (at
cluster granularity -- macOS still does not expose per-thread residency), and
energy per op is the client-silicon currency the paper's motivation appeals
to. See paper Sec. "QoS policy" and learn/34.

powermetrics requires root. Run either:
    sudo python3 scripts/run_power.py
or grant a session first (`sudo -v`) and run normally; the script re-invokes
powermetrics with sudo -n. Nothing else here needs privileges; the benchmark
processes themselves run unprivileged (spawned with the invoking user's
environment either way).
"""

import argparse
import csv
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "build/benchmarks/bench_mpmc"

QUEUES = ["faa", "vyukov", "ms", "mutex", "moody"]
QOS_ARMS = ["none", "all-int", "all-bg"]

# One powermetrics line looks like (M-series, macOS 14/15):
#   E-Cluster HW active residency:  63.50% (600 MHz: ...)
#   P-Cluster HW active frequency: 3204 MHz
#   CPU Power: 3345 mW
RE_RESID = re.compile(r"^([EP])\d?-Cluster HW active residency:\s+([0-9.]+)%")
RE_FREQ = re.compile(r"^([EP])\d?-Cluster HW active frequency:\s+([0-9.]+)\s*MHz")
RE_CPU_MW = re.compile(r"^CPU Power:\s+([0-9.]+)\s*mW")


def powermetrics_cmd(interval_ms):
    argv = ["/usr/bin/powermetrics", "--samplers", "cpu_power",
            "-i", str(interval_ms)]
    if os.geteuid() != 0:
        argv = ["sudo", "-n"] + argv
    return argv


def check_privileges():
    if os.geteuid() == 0:
        return
    probe = subprocess.run(["sudo", "-n", "true"], capture_output=True)
    if probe.returncode != 0:
        sys.exit(
            "powermetrics needs root and no sudo session is active.\n"
            "Run `sudo -v` first (or run this script itself with sudo), "
            "then retry."
        )


def parse_samples(text):
    """Aggregate one powermetrics text stream into means per metric."""
    acc = {"e_resid": [], "p_resid": [], "e_freq": [], "p_freq": [], "cpu_mw": []}
    for line in text.splitlines():
        line = line.strip()
        m = RE_RESID.match(line)
        if m:
            acc["e_resid" if m.group(1) == "E" else "p_resid"].append(float(m.group(2)))
            continue
        m = RE_FREQ.match(line)
        if m:
            acc["e_freq" if m.group(1) == "E" else "p_freq"].append(float(m.group(2)))
            continue
        m = RE_CPU_MW.match(line)
        if m:
            acc["cpu_mw"].append(float(m.group(1)))
    return {key: (sum(vals) / len(vals) if vals else None) for key, vals in acc.items()}


def run_one(queue, qos, trial, seconds, interval_ms, csv_path):
    """One benchmark process bracketed by a powermetrics sampler."""
    with tempfile.NamedTemporaryFile(mode="w+", suffix=".pm.txt") as pm_out:
        sampler = subprocess.Popen(
            powermetrics_cmd(interval_ms), stdout=pm_out, stderr=subprocess.DEVNULL
        )
        time.sleep(0.6)  # let the first (partial) sample land before the run
        bench = subprocess.run(
            [str(BENCH), "--queue", queue, "--producers", "4", "--consumers", "4",
             "--capacity", "1024", "--seconds", str(seconds), "--qos", qos,
             "--trial", str(trial), "--csv", str(csv_path)],
            capture_output=True, text=True, timeout=120,
        )
        time.sleep(0.3)
        sampler.send_signal(signal.SIGINT)  # powermetrics flushes on SIGINT
        try:
            sampler.wait(timeout=10)
        except subprocess.TimeoutExpired:
            sampler.kill()
        pm_out.seek(0)
        metrics = parse_samples(pm_out.read())
    if bench.returncode != 0:
        raise RuntimeError(
            f"bench failed ({queue} qos={qos} trial={trial}): {bench.stderr.strip()}"
        )
    return metrics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=7,
                    help="process runs per configuration; trial 0 is warmup")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--interval-ms", type=int, default=250)
    ap.add_argument("--out", default="paper/data/matrix_power.csv")
    ap.add_argument("--queues", default=",".join(QUEUES))
    args = ap.parse_args()

    if sys.platform != "darwin":
        sys.exit("powermetrics is macOS-only; this experiment targets the M2 study machine")
    if not BENCH.is_file():
        sys.exit(f"benchmark not built: {BENCH}")
    check_privileges()

    queues = [q.strip() for q in args.queues.split(",") if q.strip()]
    out_path = ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # The bench writes its own row to a per-run temp CSV; we join its fields
    # with the sampler means and append one combined row per trial.
    fieldnames = None
    rows_written = 0
    start = time.time()
    for trial in range(args.trials):
        for qos in QOS_ARMS:
            for queue in queues:
                with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as tf:
                    trial_csv = Path(tf.name)
                try:
                    metrics = run_one(queue, qos, trial, args.seconds,
                                      args.interval_ms, trial_csv)
                    with trial_csv.open() as fh:
                        bench_row = list(csv.DictReader(fh))[-1]
                finally:
                    trial_csv.unlink(missing_ok=True)
                elapsed = float(bench_row["elapsed_s"])
                ops = int(bench_row["ops"])
                nj_per_op = None
                if metrics["cpu_mw"] is not None and ops > 0:
                    # mW * s = mJ; mJ / op * 1e6 = nJ / op
                    nj_per_op = metrics["cpu_mw"] * elapsed / ops * 1e6
                row = dict(bench_row)
                row.update({k: ("" if v is None else f"{v:.3f}")
                            for k, v in metrics.items()})
                row["nj_per_op"] = "" if nj_per_op is None else f"{nj_per_op:.2f}"
                if fieldnames is None:
                    fieldnames = list(row.keys())
                    write_header = not out_path.exists()
                    out = out_path.open("a", newline="")
                    writer = csv.DictWriter(out, fieldnames=fieldnames)
                    if write_header:
                        writer.writeheader()
                writer.writerow(row)
                out.flush()
                rows_written += 1
                print(f"  {queue:8s} qos={qos:8s} trial={trial}: "
                      f"{float(bench_row['throughput_mops']):6.2f} Mops/s  "
                      f"E={metrics['e_resid'] or 0:5.1f}% P={metrics['p_resid'] or 0:5.1f}%  "
                      f"{metrics['cpu_mw'] or 0:7.0f} mW  "
                      f"{row['nj_per_op'] or '?':>8s} nJ/op", flush=True)
        print(f"round {trial} done ({rows_written} rows, "
              f"{time.time() - start:.0f}s elapsed)", flush=True)
    out.close()
    print(f"power matrix complete: {rows_written} rows -> {out_path}")


if __name__ == "__main__":
    main()
