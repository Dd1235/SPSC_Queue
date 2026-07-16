#!/usr/bin/env python3
"""Aggregate the matrix CSV and render the paper's figures.

Statistics: trial 0 of every configuration is warmup and dropped; bars/points
show the MEDIAN across remaining trials with IQR error bars (Hoefler & Belli).
Also emits paper/data/summary_<tag>.md with the aggregated tables.

Design rules applied (dataviz method): one color per queue, fixed across every
figure (color follows the entity); one axis per chart (side-by-side panels, not
dual axes); thin marks with surface gaps; recessive grid; log scales stated in
the axis label.

Usage: python3 scripts/make_plots.py --csv paper/data/matrix_v1.csv --tag v1
"""

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

from dataset_utils import CONFIG_COLUMNS, DatasetError, DatasetSelection, load_dataset

# Fixed entity colors (validated 6-slot categorical palette; CVD-safe order).
# Control variants wear their parent's hue restyled (hatch/dash), never a new
# hue: color follows the entity family (dataviz rule -- no 7th/8th hues).
COLOR = {
    "ms": "#2a78d6",         # blue
    "vyukov": "#1baf7a",     # aqua
    "vyukov-b": "#1baf7a",   # aqua (variant of vyukov)
    "faa": "#eda100",        # yellow
    "casticket": "#eda100",  # yellow (variant of faa)
    "spsc": "#008300",       # green
    "moody": "#4a3aa7",      # violet
    "mutex": "#e34948",      # red
    "rigtorp": "#eda100",    # yellow (industrial member of the ticket family)
    "xenium": "#2a78d6",     # blue (industrial MS variant: hazard pointers)
}
LABEL = {
    "ms": "Michael–Scott (EBR)",
    "vyukov": "Vyukov",
    "vyukov-b": "Vyukov+backoff",
    "faa": "FAA/ticket",
    "casticket": "CAS/ticket",
    "mutex": "mutex+deque",
    "moody": "moodycamel CQ",
    "spsc": "SPSC (1:1 only)",
    "rigtorp": "rigtorp MPMC",
    "xenium": "xenium MS+HP",
}
HATCH = {"vyukov-b": "//", "casticket": "//", "rigtorp": "..", "xenium": ".."}
DASH = {"vyukov-b": (4, 2), "casticket": (4, 2), "rigtorp": (1, 1), "xenium": (1, 1)}
ORDER = ["ms", "xenium", "vyukov", "vyukov-b", "faa", "casticket", "rigtorp",
         "moody", "mutex", "spsc"]

plt.rcParams.update({
    "font.size": 8.5,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.25,
    "grid.linewidth": 0.5,
    "axes.axisbelow": True,
    "figure.dpi": 200,
    # IEEE-style publication pipelines reject Matplotlib's default Type 3 PDF
    # glyphs.  Embed TrueType outlines instead.
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})


def agg(df, value):
    """median + IQR over trials > 0, grouped by config columns + queue."""
    d = df[df.trial > 0]
    # Keep every experimental axis in the grouping key.  In particular,
    # capacity, duration, and offered rate must never be pooled merely because
    # a caller forgot that a CSV contains a control sweep.
    g = d.groupby(CONFIG_COLUMNS, observed=True)[value]
    out = g.agg(median="median",
                q1=lambda s: s.quantile(0.25),
                q3=lambda s: s.quantile(0.75),
                cov=lambda s: s.std(ddof=0) / s.mean() if s.mean() else 0.0).reset_index()
    return out


def queues_present(df):
    return [q for q in ORDER if q in set(df.queue)]


def grouped_bars(ax, sub, xcats, xkey, ykey="median", ylabel="", logy=False):
    qs = queues_present(sub)
    n = len(qs)
    width = min(0.8 / max(n, 1), 0.16)
    for i, q in enumerate(qs):
        rows = sub[sub.queue == q].set_index(xkey)
        xs, ys, lo, hi = [], [], [], []
        for j, cat in enumerate(xcats):
            if cat in rows.index:
                r = rows.loc[cat]
                xs.append(j + (i - (n - 1) / 2) * width)
                ys.append(r["median"])
                lo.append(r["median"] - r["q1"])
                hi.append(r["q3"] - r["median"])
        ax.bar(xs, ys, width * 0.92, color=COLOR[q], label=LABEL[q],
               edgecolor="white", linewidth=0.6, hatch=HATCH.get(q, None))
        ax.errorbar(xs, ys, yerr=[lo, hi], fmt="none", ecolor="#444", elinewidth=0.7,
                    capsize=1.5)
    ax.set_xticks(range(len(xcats)))
    ax.set_xticklabels([str(c) for c in xcats])
    ax.set_ylabel(ylabel)
    if logy:
        ax.set_yscale("log")


def fig_throughput_ratio(df, outdir, tag):
    sub = agg(df[(df["mode"] == "throughput") & (df.oversubscribe == 1) &
                 (df.qos == "none") & (df.capacity == 1024) &
                 (df.queue != "spsc")], "throughput_mops")
    sub["ratio"] = sub.producers.astype(str) + ":" + sub.consumers.astype(str)
    ratios = ["1:1", "2:2", "4:4", "1:7", "2:6", "6:2", "7:1"]
    ratios = [r for r in ratios if r in set(sub.ratio)]
    fig, ax = plt.subplots(figsize=(6.8, 2.6))
    grouped_bars(ax, sub, ratios, "ratio", ylabel="throughput (Mops/s)")
    ax.set_xlabel("producers : consumers (dedicated cores)")
    ax.legend(ncol=3, fontsize=7, frameon=False)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_throughput_ratio_{tag}.{ext}")
    plt.close(fig)


def fig_oversubscription(df, outdir, tag):
    sub = agg(df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
                 (df.qos == "none") & (df.capacity == 1024)], "throughput_mops")
    fig, ax = plt.subplots(figsize=(3.4, 3.15))
    for q in queues_present(sub):
        rows = sub[sub.queue == q].sort_values("oversubscribe")
        ax.errorbar(rows.oversubscribe, rows["median"],
                    yerr=[rows["median"] - rows.q1, rows.q3 - rows["median"]],
                    color=COLOR[q], label=LABEL[q], marker="o", markersize=3.5,
                    linewidth=1.6, capsize=1.5,
                    dashes=DASH.get(q, (None, None)) if q in DASH else (None, None),
                    linestyle="--" if q in DASH else "-")
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4])
    ax.set_xticklabels(["x1\n(8 thr)", "x2\n(16 thr)", "x4\n(32 thr)"])
    ax.set_yscale("log")
    ax.set_ylabel("throughput (Mops/s, log)")
    ax.set_xlabel("oversubscription (4P:4C base, capacity 1024)")
    ax.legend(fontsize=5.8, frameon=False, ncol=2, loc="lower center",
              bbox_to_anchor=(0.5, 1.01), columnspacing=0.8, handlelength=1.8)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_oversubscription_{tag}.{ext}", bbox_inches="tight")
    plt.close(fig)


def fig_latency_tail(df, outdir, tag):
    base = df[(df["mode"] == "latency") & (df.producers == 4) & (df.consumers == 4) &
              (df.capacity == 1024) & (df.qos == "none") & (df.rate == 1_000_000)]
    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.6), sharey=True)
    for ax, f in zip(axes, [1, 4]):
        subdf = base[base.oversubscribe == f]
        rows = []
        for stat, col in [("p50", "p50_ns"), ("p99", "p99_ns"), ("p99.9", "p999_ns")]:
            a = agg(subdf, col)
            a["stat"] = stat
            rows.append(a)
        sub = pd.concat(rows)
        qs = queues_present(sub)
        n = len(qs)
        width = min(0.8 / max(n, 1), 0.16)
        cats = ["p50", "p99", "p99.9"]
        for i, q in enumerate(qs):
            r = sub[sub.queue == q].set_index("stat")
            xs = [j + (i - (n - 1) / 2) * width for j in range(len(cats))]
            ys = [r.loc[c, "median"] if c in r.index else float("nan") for c in cats]
            lo = [r.loc[c, "median"] - r.loc[c, "q1"] if c in r.index else float("nan")
                  for c in cats]
            hi = [r.loc[c, "q3"] - r.loc[c, "median"] if c in r.index else float("nan")
                  for c in cats]
            ax.bar(xs, ys, width * 0.92, color=COLOR[q], label=LABEL[q] if f == 1 else None,
                   edgecolor="white", linewidth=0.6)
            ax.errorbar(xs, ys, yerr=[lo, hi], fmt="none", ecolor="#444",
                        elinewidth=0.7, capsize=1.5)
        ax.set_xticks(range(len(cats)))
        ax.set_xticklabels(cats)
        ax.set_yscale("log")
        ax.set_title(f"oversubscription x{f}", fontsize=8.5)
        sample_n = subdf[subdf.trial > 0].groupby("queue", observed=True).size()
        if not sample_n.empty:
            n_text = (str(int(sample_n.min())) if sample_n.min() == sample_n.max()
                      else f"{int(sample_n.min())}–{int(sample_n.max())}")
            ax.set_xlabel(f"retained n={n_text} per queue", fontsize=6.5,
                          color="#555", labelpad=1)
    axes[0].set_ylabel("tick-to-pop latency (ns, log)")
    axes[0].legend(fontsize=6.5, frameon=False)
    fig.suptitle("paced 1 M msg/s, 4P:4C — coordinated-omission-aware", fontsize=8.5, y=1.02)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_latency_tail_{tag}.{ext}", bbox_inches="tight")
    plt.close(fig)


def fig_qos(df, outdir, tag):
    sub = agg(df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
                 (df.oversubscribe == 1) & (df.capacity == 1024)], "throughput_mops")
    cats = [q for q in ["none", "all-int", "all-bg", "prod-bg", "cons-bg"] if q in set(sub.qos)]
    fig, ax = plt.subplots(figsize=(6.8, 2.6))
    grouped_bars(ax, sub, cats, "qos", ylabel="throughput (Mops/s)")
    ax.set_xlabel("requested thread QoS policy (interactive / background; best-effort)")
    ax.legend(ncol=3, fontsize=7, frameon=False)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_qos_{tag}.{ext}")
    plt.close(fig)


def fig_calib(df, outdir, tag):
    fig, ax = plt.subplots(figsize=(6.8, 1.8))
    d = df.reset_index(drop=True)
    ax.plot(d.index, d.calib_ns / 1e6, color="#2a78d6", linewidth=1.0)
    ax.set_xlabel("run index (chronological)")
    ax.set_ylabel("calibration (ms)")
    ax.set_title("spin-calibration drift across the matrix (thermal screen)", fontsize=8.5)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_calib_{tag}.{ext}")
    plt.close(fig)


def fig_fairness(df, outdir, tag):
    """Finite-window producer/consumer work balance, 4P:4C (legacy file stem)."""
    base = df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
              (df.qos == "none") & (df.capacity == 1024)]
    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.6), sharey=True)
    for ax, col, title in zip(axes, ["fair_cov", "cons_cov"],
                              ["producer-side", "consumer-side"]):
        sub = agg(base, col)
        cats = [1, 4]
        sub = sub[sub.oversubscribe.isin(cats)]
        grouped_bars(ax, sub, cats, "oversubscribe",
                     ylabel="CoV of per-thread ops" if col == "fair_cov" else "")
        ax.set_xticklabels(["x1", "x4"])
        ax.set_title(title, fontsize=8.5)
        ax.set_xlabel("oversubscription")
    axes[0].legend(ncol=2, fontsize=6, frameon=False)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_fairness_{tag}.{ext}")
    plt.close(fig)


def fig_mechanism(df, outdir, tag):
    """Stats-build dataset: retries/op (log) and involuntary csw vs oversub."""
    base = df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
              (df.qos == "none") & (df.capacity == 1024)]
    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.9))
    zero_arms = []
    for q in queues_present(base):
        rows = agg(base[base.queue == q], "retries_per_op").sort_values("oversubscribe")
        if rows["median"].max() > 0:
            axes[0].plot(rows.oversubscribe, rows["median"].clip(lower=1e-3),
                         color=COLOR[q], label=LABEL[q], marker="o", markersize=3,
                         linestyle="--" if q in DASH else "-", linewidth=1.5)
        elif q in ("faa",):  # zero retries IS the finding -- annotate, don't hide
            zero_arms.append(q)
    for q in zero_arms:
        axes[0].annotate(f"{LABEL[q]}: 0 retries/op (exact)", xy=(0.03, 0.04),
                         xycoords="axes fraction", fontsize=6.5, color=COLOR[q],
                         fontweight="bold")
    axes[0].set_yscale("log")
    axes[0].set_xscale("log", base=2)
    axes[0].set_xticks([1, 2, 4]); axes[0].set_xticklabels(["x1", "x2", "x4"])
    axes[0].set_ylabel("retries per op (log)")
    axes[0].set_xlabel("oversubscription")
    axes[0].set_title("claim/CAS retries", fontsize=8.5)
    axes[0].legend(fontsize=5.5, frameon=False)
    for q in queues_present(base):
        rows = agg(base[base.queue == q], "ivcsw").sort_values("oversubscribe")
        axes[1].plot(rows.oversubscribe, rows["median"].clip(lower=1),
                     color=COLOR[q], label=LABEL[q], marker="o", markersize=3,
                     linestyle="--" if q in DASH else "-", linewidth=1.5)
    axes[1].legend(fontsize=5, frameon=False, ncol=2)
    axes[1].set_yscale("log")
    axes[1].set_xscale("log", base=2)
    axes[1].set_xticks([1, 2, 4]); axes[1].set_xticklabels(["x1", "x2", "x4"])
    axes[1].set_ylabel("involuntary ctx switches (log)")
    axes[1].set_xlabel("oversubscription")
    axes[1].set_title("preemptions per trial", fontsize=8.5)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_mechanism_{tag}.{ext}")
    plt.close(fig)


def fig_ebrfix(df, outdir, tag):
    """F8 A/B/C: peak RSS (log) and throughput across ratios, ms modes+mutex."""
    base = df[(df["mode"] == "throughput") & (df.oversubscribe == 1) &
              (df.qos == "none") & (df.capacity == 1024)]
    base = base.copy()
    base["ratio"] = base.producers.astype(str) + ":" + base.consumers.astype(str)
    ratios = [r for r in ["1:1", "4:4", "2:6", "1:7"] if r in set(base.ratio)]
    labels = {"ms": "legacy", "ms-fix": "prefix fix", "ms-retry": "retry variant",
              "mutex": "mutex (bounded ref)"}
    colors = {"ms": "#2a78d6", "ms-fix": "#008300", "ms-retry": "#e34948",
              "mutex": "#9aa3b2"}
    hatches = {"ms-fix": None, "ms-retry": "//", "ms": None, "mutex": None}
    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.6))
    for ax, col, ylabel, logy in ((axes[0], "peak_rss_mb", "peak RSS (MB, log)", True),
                                  (axes[1], "throughput_mops", "throughput (Mops/s)", False)):
        d = base[base.ratio.isin(ratios) & (base.trial > 0)]
        stats = d.groupby(["ratio", "queue"], observed=True)[col].agg(
            median="median",
            q1=lambda values: values.quantile(0.25),
            q3=lambda values: values.quantile(0.75),
        ).reset_index()
        qs = [q for q in ["ms", "ms-fix", "ms-retry", "mutex"] if q in set(stats.queue)]
        width = min(0.8 / max(len(qs), 1), 0.2)
        for i, q in enumerate(qs):
            rows = stats[stats.queue == q].set_index("ratio")
            xs, ys, lo, hi = [], [], [], []
            for j, rt in enumerate(ratios):
                if rt in rows.index:
                    row = rows.loc[rt]
                    xs.append(j + (i - (len(qs) - 1) / 2) * width)
                    value = float(row["median"])
                    ys.append(max(value, 0.5) if logy else value)
                    lo.append(value - float(row["q1"]))
                    hi.append(float(row["q3"]) - value)
            ax.bar(xs, ys, width * 0.9, color=colors[q], label=labels[q],
                   edgecolor="white", linewidth=0.6, hatch=hatches.get(q))
            ax.errorbar(xs, ys, yerr=[lo, hi], fmt="none", ecolor="#444",
                        elinewidth=0.7, capsize=1.5)
        ax.set_xticks(range(len(ratios)))
        ax.set_xticklabels(ratios)
        ax.set_ylabel(ylabel)
        ax.set_xlabel("producers : consumers")
        if logy:
            ax.set_yscale("log")
    handles, legend_labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, legend_labels, fontsize=6, frameon=False, ncol=4,
               loc="upper center", bbox_to_anchor=(0.5, 1.0), columnspacing=1.0)
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_ebrfix_{tag}.{ext}")
    plt.close(fig)


def fig_ebrmech(df, outdir, tag):
    """F8 mechanism (stats twin): maintenance cost, advance success, limbo peak.

    One panel per counter, ms modes across ratios. Together the panels observe
    each link of the hypothesized cost feedback loop separately, so competing
    failure theories make different predictions (see paper Sec. F8).
    """
    base = df[(df["mode"] == "throughput") & (df.oversubscribe == 1) &
              (df.qos == "none") & (df.capacity == 1024)].copy()
    base["ratio"] = base.producers.astype(str) + ":" + base.consumers.astype(str)
    ratios = [r for r in ["1:1", "4:4", "2:6", "1:7"] if r in set(base.ratio)]
    labels = {"ms": "legacy", "ms-fix": "prefix fix", "ms-retry": "retry variant"}
    colors = {"ms": "#2a78d6", "ms-fix": "#008300", "ms-retry": "#e34948"}
    hatches = {"ms": None, "ms-fix": None, "ms-retry": "//"}
    panels = (
        ("ebr_maint_ns_per_pass", "maintenance (µs/pass, log)", True, 1e-3),
        ("ebr_adv_success_rate", "advance success (%)", False, 100.0),
        ("ebr_limbo_peak", "peak limbo (entries, log)", True, 1.0),
    )
    fig, axes = plt.subplots(1, 3, figsize=(6.8, 2.4))
    for ax, (col, ylabel, logy, scale) in zip(axes, panels):
        d = base[base.ratio.isin(ratios) & (base.trial > 0)]
        stats = d.groupby(["ratio", "queue"], observed=True)[col].agg(
            median="median",
            q1=lambda values: values.quantile(0.25),
            q3=lambda values: values.quantile(0.75),
        ).reset_index()
        qs = [q for q in ["ms", "ms-fix", "ms-retry"] if q in set(stats.queue)]
        width = min(0.8 / max(len(qs), 1), 0.25)
        for i, q in enumerate(qs):
            rows = stats[stats.queue == q].set_index("ratio")
            xs, ys, lo, hi = [], [], [], []
            for j, rt in enumerate(ratios):
                if rt in rows.index:
                    row = rows.loc[rt]
                    xs.append(j + (i - (len(qs) - 1) / 2) * width)
                    value = float(row["median"]) * scale
                    floor = 0.5 if col == "ebr_limbo_peak" else 1e-3
                    ys.append(max(value, floor) if logy else value)
                    lo.append(value - float(row["q1"]) * scale)
                    hi.append(float(row["q3"]) * scale - value)
            ax.bar(xs, ys, width * 0.9, color=colors[q], label=labels[q],
                   edgecolor="white", linewidth=0.6, hatch=hatches.get(q))
            ax.errorbar(xs, ys, yerr=[lo, hi], fmt="none", ecolor="#444",
                        elinewidth=0.7, capsize=1.5)
        ax.set_xticks(range(len(ratios)))
        ax.set_xticklabels(ratios, fontsize=6)
        ax.set_ylabel(ylabel, fontsize=7)
        ax.set_xlabel("producers : consumers", fontsize=7)
        ax.tick_params(labelsize=6)
        if logy:
            ax.set_yscale("log")
    handles, legend_labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, legend_labels, fontsize=6, frameon=False, ncol=3,
               loc="upper center", bbox_to_anchor=(0.5, 1.0), columnspacing=1.0)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_ebrmech_{tag}.{ext}")
    plt.close(fig)


def fig_industrial(df, outdir, tag):
    """Industrial cross-checks: rigtorp vs faa under oversubscription (left);
    reclamation-scheme memory at consumer-heavy ratios, xenium HP vs our EBR
    (right). Each panel pairs one of our arms with its independent industrial
    counterpart, same rounds, same thermal regime."""
    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.6))

    over = agg(df[(df["mode"] == "throughput") & (df.producers == 4) &
                  (df.consumers == 4) & (df.qos == "none") &
                  (df.capacity == 1024)], "throughput_mops")
    ax = axes[0]
    for q in [q for q in ("faa", "rigtorp", "vyukov", "mutex")
              if q in set(over.queue)]:
        rows = over[over.queue == q].sort_values("oversubscribe")
        ax.errorbar(rows.oversubscribe, rows["median"],
                    yerr=[rows["median"] - rows.q1, rows.q3 - rows["median"]],
                    color=COLOR[q], label=LABEL[q], marker="o", markersize=3.5,
                    linewidth=1.5, capsize=1.5,
                    dashes=DASH.get(q, (None, None)) if q in DASH else (None, None),
                    linestyle="--" if q in DASH else "-")
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4])
    ax.set_xticklabels(["x1", "x2", "x4"])
    ax.set_yscale("log")
    ax.set_ylabel("throughput (Mops/s, log)")
    ax.set_xlabel("oversubscription (4P:4C, cap 1024)")
    ax.legend(fontsize=6, frameon=False)

    mem = df[(df["mode"] == "throughput") & (df.oversubscribe == 1) &
             (df.qos == "none") & (df.capacity == 1024) & (df.trial > 0)].copy()
    mem["ratio"] = mem.producers.astype(str) + ":" + mem.consumers.astype(str)
    ratios = [r for r in ["1:1", "4:4", "2:6", "1:7"] if r in set(mem.ratio)]
    stats = mem.groupby(["ratio", "queue"], observed=True)["peak_rss_mb"].agg(
        median="median",
        q1=lambda values: values.quantile(0.25),
        q3=lambda values: values.quantile(0.75),
    ).reset_index()
    ax = axes[1]
    qs = [q for q in ("ms", "xenium", "mutex") if q in set(stats.queue)]
    width = min(0.8 / max(len(qs), 1), 0.25)
    for i, q in enumerate(qs):
        rows = stats[stats.queue == q].set_index("ratio")
        xs, ys, lo, hi = [], [], [], []
        for j, rt in enumerate(ratios):
            if rt in rows.index:
                row = rows.loc[rt]
                xs.append(j + (i - (len(qs) - 1) / 2) * width)
                value = float(row["median"])
                ys.append(max(value, 0.5))
                lo.append(value - float(row["q1"]))
                hi.append(float(row["q3"]) - value)
        ax.bar(xs, ys, width * 0.9, color=COLOR[q], label=LABEL[q],
               edgecolor="white", linewidth=0.6, hatch=HATCH.get(q))
        ax.errorbar(xs, ys, yerr=[lo, hi], fmt="none", ecolor="#444",
                    elinewidth=0.7, capsize=1.5)
    ax.set_xticks(range(len(ratios)))
    ax.set_xticklabels(ratios)
    ax.set_yscale("log")
    ax.set_ylabel("peak RSS (MB, log)")
    ax.set_xlabel("producers : consumers")
    ax.legend(fontsize=6, frameon=False)

    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_industrial_{tag}.{ext}")
    plt.close(fig)


def fig_loadcurve(df, outdir, tag):

    """Optional offered-rate profile: schedule-to-dequeue p99 latency."""
    base = df[(df["mode"] == "latency") & (df.trial > 0) &
              (df.producers == 4) & (df.consumers == 4) &
              (df.oversubscribe == 1) & (df.capacity == 1024) &
              (df.qos == "none")].copy()
    fig, ax = plt.subplots(figsize=(4.2, 2.8))
    for q in queues_present(base):
        d = base[base.queue == q]
        stats = d.groupby("rate", observed=True)["p99_ns"].agg(
            median="median",
            q1=lambda values: values.quantile(0.25),
            q3=lambda values: values.quantile(0.75),
        ).reset_index().sort_values("rate")
        if len(stats):
            ax.errorbar(
                stats.rate / 1e6,
                stats["median"] / 1000,
                yerr=[(stats["median"] - stats.q1) / 1000,
                      (stats.q3 - stats["median"]) / 1000],
                color=COLOR[q], label=LABEL[q], marker="o", markersize=3.5,
                linewidth=1.5, capsize=1.5,
                linestyle="--" if q in DASH else "-",
            )
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("offered load (M msg/s, log)")
    ax.set_ylabel("p99 latency (us, log)")
    ax.set_title("schedule-to-dequeue latency; exact sample-count rows", fontsize=7.5)
    ax.legend(fontsize=6, frameon=False)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_loadcurve_{tag}.{ext}")
    plt.close(fig)


def md_table(piv):
    """Markdown-render a pivot table without the optional tabulate dependency."""
    cols = list(piv.columns)
    lines = ["| " + " | ".join([str(piv.index.name or "")] + [str(c) for c in cols]) + " |",
             "|" + "---|" * (len(cols) + 1)]
    for idx, row in piv.iterrows():
        cells = ["" if pd.isna(row[c]) else f"{row[c]:g}" for c in cols]
        lines.append("| " + " | ".join([str(idx)] + cells) + " |")
    return "\n".join(lines)


def write_summary(df, outdir, tag, selections):
    lines = [f"# Matrix summary ({tag})", ""]
    lines.append(
        f"selected rows: {len(df)}; "
        f"calib drift: {df.calib_ns.min()/1e6:.1f}–{df.calib_ns.max()/1e6:.1f} ms"
    )
    for path, selection in selections:
        lines.append(f"- `{path}`: {selection.describe()}")
    measured = df[df.trial > 0]
    sample_n = measured.groupby(CONFIG_COLUMNS, observed=True).size()
    lines.append(
        f"Per-configuration retained sample n: {int(sample_n.min())}–{int(sample_n.max())}."
    )
    lines.append("")
    t = agg(df[(df["mode"] == "throughput") & (df.oversubscribe == 1) &
               (df.qos == "none") & (df.capacity == 1024)], "throughput_mops")
    if not t.empty:
        lines.append("## Throughput medians (Mops/s), dedicated cores, qos=none")
        t["ratio"] = t.producers.astype(str) + ":" + t.consumers.astype(str)
        piv = t.pivot_table(index="ratio", columns="queue", values="median")
        lines.append(md_table(piv.round(2)))
        lines.append("")
    o = agg(df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
               (df.qos == "none") & (df.capacity == 1024)], "throughput_mops")
    if o.oversubscribe.nunique() >= 2:
        lines.append("## Oversubscription (4P:4C, capacity 1024), throughput medians")
        lines.append(md_table(o.pivot_table(index="oversubscribe", columns="queue",
                                           values="median").round(2)))
        lines.append("")
    la = agg(df[(df["mode"] == "latency") & (df.capacity == 1024) &
                (df.qos == "none") & (df.rate == 1_000_000)], "p999_ns")
    la = la[(la.producers == 4) & (la.consumers == 4)]
    if not la.empty:
        lines.append("## Latency p99.9 (us), paced 1M/s 4P:4C")
        lines.append(md_table((la.pivot_table(index="oversubscribe", columns="queue",
                                            values="median") / 1000).round(1)))
    (outdir / f"summary_{tag}.md").write_text("\n".join(lines).rstrip() + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="paper/data/matrix_v1.csv",
                    help="dataset path, or comma-separated paths sharing one protocol "
                         "(each file is validated and round-selected independently, "
                         "then concatenated -- e.g. the load + load2 rate sweeps)")
    ap.add_argument("--tag", default="v1")
    ap.add_argument("--assets", default="paper/assets")
    ap.add_argument("--summary-dir",
                    help="summary output directory (default: directory containing --csv)")
    ap.add_argument("--seconds", type=float,
                    help="select one duration when a CSV contains multiple runs")
    ap.add_argument("--max-trial", type=int,
                    help="cap the complete measured rounds retained (trial 0 is warm-up)")
    args = ap.parse_args()

    paths = [p.strip() for p in args.csv.split(",") if p.strip()]
    frames = []
    selections = []
    try:
        for path in paths:
            frame, sel = load_dataset(path, seconds=args.seconds, max_trial=args.max_trial)
            frames.append(frame)
            selections.append((path, sel))
            print(f"dataset selection [{path}]: {sel.describe()}")
    except DatasetError as exc:
        ap.error(str(exc))
    df = frames[0] if len(frames) == 1 else pd.concat(frames, ignore_index=True)
    assets = Path(args.assets)
    assets.mkdir(parents=True, exist_ok=True)
    data_dir = Path(args.summary_dir) if args.summary_dir else Path(paths[0]).parent
    data_dir.mkdir(parents=True, exist_ok=True)

    generated = []
    skipped = []

    def emit(stem, renderer, enabled):
        if enabled:
            renderer(df, assets, args.tag)
            generated.append(stem)
            return
        skipped.append(stem)

    has_ebr_focus = bool(set(df.queue) & {"ms-fix", "ms-retry"})
    latency = df[(df["mode"] == "latency") & (df.capacity == 1024) &
                 (df.qos == "none")]
    has_load_focus = (
        not (df["mode"] == "throughput").any()
        and latency.rate.nunique() >= 3
    )
    generic = not has_ebr_focus and not has_load_focus

    ratio_rows = df[(df["mode"] == "throughput") & (df.oversubscribe == 1) &
                    (df.qos == "none") & (df.capacity == 1024)]
    ratio_count = len(set(zip(ratio_rows.producers, ratio_rows.consumers)))
    oversub_rows = df[(df["mode"] == "throughput") & (df.producers == 4) &
                      (df.consumers == 4) & (df.qos == "none") &
                      (df.capacity == 1024)]
    qos_rows = df[(df["mode"] == "throughput") & (df.producers == 4) &
                  (df.consumers == 4) & (df.oversubscribe == 1) &
                  (df.capacity == 1024)]
    tail_rows = latency[(latency.producers == 4) & (latency.consumers == 4) &
                        (latency.rate == 1_000_000)]

    emit("fig_throughput_ratio", fig_throughput_ratio, generic and ratio_count >= 2)
    emit("fig_oversubscription", fig_oversubscription,
         generic and oversub_rows.oversubscribe.nunique() >= 2)
    emit("fig_latency_tail", fig_latency_tail,
         generic and {1, 4}.issubset(set(tail_rows.oversubscribe)))
    emit("fig_qos", fig_qos, generic and qos_rows.qos.nunique() >= 2)
    emit("fig_calib", fig_calib, "calib_ns" in df.columns and not df.empty)
    emit("fig_fairness", fig_fairness,
         generic and "cons_cov" in df.columns and
         {1, 4}.issubset(set(oversub_rows.oversubscribe)))
    emit("fig_ebrfix", fig_ebrfix, has_ebr_focus)
    emit("fig_ebrmech", fig_ebrmech,
         "ebr_maint_ns_per_pass" in df.columns and df.ebr_maint_ns_per_pass.max() > 0)
    emit("fig_loadcurve", fig_loadcurve, has_load_focus)
    emit("fig_industrial", fig_industrial,
         bool(set(df.queue) & {"rigtorp", "xenium"}))
    emit("fig_mechanism", fig_mechanism,
         generic and "retries_per_op" in df.columns and df.retries_per_op.max() > 0)
    write_summary(df, data_dir, args.tag, selections)
    print("generated figures: " + (", ".join(generated) if generated else "none"))
    print("skipped figures (required data absent/profile-specific): " + ", ".join(skipped))
    print(f"summary -> {data_dir}/summary_{args.tag}.md")


if __name__ == "__main__":
    main()
