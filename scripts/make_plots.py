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
}
HATCH = {"vyukov-b": "//", "casticket": "//"}   # bar variant marker
DASH = {"vyukov-b": (4, 2), "casticket": (4, 2)}  # line variant marker
ORDER = ["ms", "vyukov", "vyukov-b", "faa", "casticket", "moody", "mutex", "spsc"]

plt.rcParams.update({
    "font.size": 8.5,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.25,
    "grid.linewidth": 0.5,
    "axes.axisbelow": True,
    "figure.dpi": 200,
})


def agg(df, value):
    """median + IQR over trials > 0, grouped by config columns + queue."""
    d = df[df.trial > 0]
    g = d.groupby(["queue", "mode", "producers", "consumers", "oversubscribe", "qos"])[value]
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
                 (df.qos == "none") & (df.queue != "spsc")], "throughput_mops")
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
                 (df.qos == "none")], "throughput_mops")
    fig, ax = plt.subplots(figsize=(3.4, 2.6))
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
    ax.set_xlabel("oversubscription (4P:4C base)")
    ax.legend(fontsize=6.5, frameon=False)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_oversubscription_{tag}.{ext}")
    plt.close(fig)


def fig_latency_tail(df, outdir, tag):
    base = df[(df["mode"] == "latency") & (df.producers == 4) & (df.consumers == 4)]
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
            ax.bar(xs, ys, width * 0.92, color=COLOR[q], label=LABEL[q] if f == 1 else None,
                   edgecolor="white", linewidth=0.6)
        ax.set_xticks(range(len(cats)))
        ax.set_xticklabels(cats)
        ax.set_yscale("log")
        ax.set_title(f"oversubscription x{f}", fontsize=8.5)
    axes[0].set_ylabel("tick-to-pop latency (ns, log)")
    axes[0].legend(fontsize=6.5, frameon=False)
    fig.suptitle("paced 1 M msg/s, 4P:4C — coordinated-omission-aware", fontsize=8.5, y=1.02)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"fig_latency_tail_{tag}.{ext}", bbox_inches="tight")
    plt.close(fig)


def fig_qos(df, outdir, tag):
    sub = agg(df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
                 (df.oversubscribe == 1)], "throughput_mops")
    cats = [q for q in ["none", "all-int", "all-bg", "prod-bg", "cons-bg"] if q in set(sub.qos)]
    fig, ax = plt.subplots(figsize=(6.8, 2.6))
    grouped_bars(ax, sub, cats, "qos", ylabel="throughput (Mops/s)")
    ax.set_xlabel("QoS policy (P-core bias = interactive, E-core bias = background)")
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
    """Producer & consumer fairness (CoV of per-thread op counts), 4P:4C."""
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
    fig, axes = plt.subplots(1, 2, figsize=(6.8, 2.6))
    for q in queues_present(base):
        rows = agg(base[base.queue == q], "retries_per_op").sort_values("oversubscribe")
        if rows["median"].max() > 0:
            axes[0].plot(rows.oversubscribe, rows["median"].clip(lower=1e-3),
                         color=COLOR[q], label=LABEL[q], marker="o", markersize=3,
                         linestyle="--" if q in DASH else "-", linewidth=1.5)
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
                     color=COLOR[q], marker="o", markersize=3,
                     linestyle="--" if q in DASH else "-", linewidth=1.5)
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


def md_table(piv):
    """Markdown-render a pivot table without the optional tabulate dependency."""
    cols = list(piv.columns)
    lines = ["| " + " | ".join([str(piv.index.name or "")] + [str(c) for c in cols]) + " |",
             "|" + "---|" * (len(cols) + 1)]
    for idx, row in piv.iterrows():
        cells = ["" if pd.isna(row[c]) else f"{row[c]:g}" for c in cols]
        lines.append("| " + " | ".join([str(idx)] + cells) + " |")
    return "\n".join(lines)


def write_summary(df, outdir, tag):
    lines = [f"# Matrix summary ({tag})", ""]
    lines.append(f"rows: {len(df)}  configs: "
                 f"{len(df.groupby(['queue','mode','producers','consumers','oversubscribe','qos']))}"
                 f"  calib drift: {df.calib_ns.min()/1e6:.1f}–{df.calib_ns.max()/1e6:.1f} ms")
    lines.append("")
    lines.append("## Throughput medians (Mops/s), dedicated cores, qos=none")
    t = agg(df[(df["mode"] == "throughput") & (df.oversubscribe == 1) & (df.qos == "none")],
            "throughput_mops")
    t["ratio"] = t.producers.astype(str) + ":" + t.consumers.astype(str)
    piv = t.pivot_table(index="ratio", columns="queue", values="median")
    lines.append(md_table(piv.round(2)))
    lines.append("")
    lines.append("## Oversubscription (4P:4C), throughput medians")
    o = agg(df[(df["mode"] == "throughput") & (df.producers == 4) & (df.consumers == 4) &
               (df.qos == "none")], "throughput_mops")
    lines.append(md_table(o.pivot_table(index="oversubscribe", columns="queue",
                                       values="median").round(2)))
    lines.append("")
    lines.append("## Latency p99.9 (us), paced 1M/s 4P:4C")
    la = agg(df[df["mode"] == "latency"], "p999_ns")
    la = la[(la.producers == 4) & (la.consumers == 4)]
    lines.append(md_table((la.pivot_table(index="oversubscribe", columns="queue",
                                        values="median") / 1000).round(1)))
    (outdir / f"summary_{tag}.md").write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="paper/data/matrix_v1.csv")
    ap.add_argument("--tag", default="v1")
    ap.add_argument("--assets", default="paper/assets")
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    assets = Path(args.assets)
    assets.mkdir(parents=True, exist_ok=True)
    data_dir = Path(args.csv).parent

    fig_throughput_ratio(df, assets, args.tag)
    fig_oversubscription(df, assets, args.tag)
    fig_latency_tail(df, assets, args.tag)
    fig_qos(df, assets, args.tag)
    fig_calib(df, assets, args.tag)
    if "cons_cov" in df.columns:
        fig_fairness(df, assets, args.tag)
    if "retries_per_op" in df.columns and df.retries_per_op.max() > 0:
        fig_mechanism(df, assets, args.tag)
    write_summary(df, data_dir, args.tag)
    print(f"figures -> {assets}/fig_*_{args.tag}.(pdf|png); summary -> "
          f"{data_dir}/summary_{args.tag}.md")


if __name__ == "__main__":
    main()
