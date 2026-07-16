#!/usr/bin/env python3
"""Render the sustained-soak thermal series (fig_thermal_<tag>).

matrix_thermal.csv is a TIME SERIES: `trial` is the soak index within one
design's back-to-back run, and soak 0 (the cold point) is essential data.
The kept-rounds machinery in dataset_utils would drop it as warmup and treat
soaks as repeated measures, so this figure reads the raw CSV directly --
the one deliberate exception to the shared selection pipeline, documented
here and in learn/36.

Left panel: throughput per soak, normalized to each design's first three
soaks (cold baseline). Right panel: the spin-calibration probe (ms) per soak
-- the thermometer. If decay tracks calib, the chip slowed; if a design falls
faster than calib, it has a thermal amplifier of its own.
"""

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

COLOR = {"ms": "#2a78d6", "vyukov": "#1baf7a", "faa": "#eda100", "mutex": "#e34948"}
LABEL = {"ms": "Michael–Scott (EBR)", "vyukov": "Vyukov", "faa": "FAA/ticket",
         "mutex": "mutex+deque"}

plt.rcParams.update({
    "font.size": 8.5, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.alpha": 0.25, "grid.linewidth": 0.5,
    "axes.axisbelow": True, "figure.dpi": 200,
    "pdf.fonttype": 42, "ps.fonttype": 42,
})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="paper/data/matrix_thermal.csv")
    ap.add_argument("--tag", default="thermal")
    ap.add_argument("--assets", default="paper/assets")
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    outdir = Path(args.assets)
    outdir.mkdir(parents=True, exist_ok=True)

    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.6))
    for q in [q for q in COLOR if q in set(df.queue)]:
        d = df[df.queue == q].sort_values("trial")
        cold = d[d.trial < 3].throughput_mops.median()
        axes[0].plot(d.trial, d.throughput_mops / cold, color=COLOR[q],
                     label=LABEL[q], linewidth=1.4, marker="o", markersize=2.2)
        axes[1].plot(d.trial, d.calib_ns / 1e6, color=COLOR[q],
                     linewidth=1.4, marker="o", markersize=2.2)
    axes[0].axhline(1.0, color="#888", linewidth=0.7, linestyle=":")
    axes[0].set_ylabel("throughput / cold baseline")
    axes[0].set_xlabel("soak index (10 s back-to-back trials)")
    axes[0].legend(fontsize=6, frameon=False)
    axes[1].set_ylabel("calibration probe (ms)")
    axes[1].set_xlabel("soak index")
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_thermal_{args.tag}.{ext}")
    print(f"figure -> {outdir}/fig_thermal_{args.tag}.(pdf|png)")

    print("\ndecay summary (median of last 5 soaks vs cold baseline):")
    for q in [q for q in COLOR if q in set(df.queue)]:
        d = df[df.queue == q].sort_values("trial")
        cold = d[d.trial < 3].throughput_mops.median()
        hot = d[d.trial >= d.trial.max() - 4].throughput_mops.median()
        calib_cold = d[d.trial < 3].calib_ns.median() / 1e6
        calib_hot = d[d.trial >= d.trial.max() - 4].calib_ns.median() / 1e6
        print(f"  {q:8s} throughput {hot/cold:6.1%} of cold"
              f"  (calib {calib_cold:.1f} -> {calib_hot:.1f} ms)")


if __name__ == "__main__":
    main()
