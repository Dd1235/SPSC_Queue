#!/usr/bin/env python3
"""95% bootstrap percentile intervals for the paper's headline medians.

With 7 kept rounds per configuration, parametric intervals would be theater;
the bootstrap percentile interval at n=7 is coarse but honest -- it reports
the resampling spread of the median actually observed. The paper quotes these
next to headline claims; a wide interval is information, not a defect.

Deterministic (fixed seed) so the quoted numbers are reproducible.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dataset_utils import load_dataset  # noqa: E402

DATA = Path(__file__).resolve().parents[1] / "paper/data"
RNG = np.random.default_rng(20260716)
B = 20_000


def rows(csv, **filt):
    df, _ = load_dataset(DATA / f"{csv}.csv")
    d = df[df.trial > 0]
    for k, v in filt.items():
        d = d[d[k] == v]
    if d.empty:
        raise SystemExit(f"no rows: {csv} {filt}")
    return d


def vals(csv, queue, value="throughput_mops", **filt):
    return rows(csv, queue=queue, **filt)[value].to_numpy()


def boot_median(x):
    idx = RNG.integers(0, len(x), size=(B, len(x)))
    return np.percentile(np.median(x[idx], axis=1), [2.5, 97.5])


def boot_ratio(num, den):
    """CI for median(num)/median(den) with independent resampling."""
    ni = RNG.integers(0, len(num), size=(B, len(num)))
    di = RNG.integers(0, len(den), size=(B, len(den)))
    r = np.median(num[ni], axis=1) / np.median(den[di], axis=1)
    return np.percentile(r, [2.5, 97.5])


def report(name, point, ci, unit=""):
    print(f"{name:58s} {point:8.2f}{unit}  CI95 [{ci[0]:.2f}, {ci[1]:.2f}]")


def main():
    shape = dict(mode="throughput", producers=4, consumers=4, qos="none",
                 capacity=1024)

    # F1: FAA improves under x4 oversubscription while Vyukov collapses (v3).
    faa1 = vals("matrix_v3", "faa", oversubscribe=1, **shape)
    faa4 = vals("matrix_v3", "faa", oversubscribe=4, **shape)
    vyu1 = vals("matrix_v3", "vyukov", oversubscribe=1, **shape)
    vyu4 = vals("matrix_v3", "vyukov", oversubscribe=4, **shape)
    report("F1 faa x4/x1 throughput ratio", np.median(faa4) / np.median(faa1),
           boot_ratio(faa4, faa1), "x")
    report("F1 vyukov x4/x1 throughput ratio", np.median(vyu4) / np.median(vyu1),
           boot_ratio(vyu4, vyu1), "x")

    # F2: QoS-policy retention (v2, all-bg / all-int).
    for q in ("faa", "mutex", "vyukov"):
        hi = vals("matrix_v2", q, oversubscribe=1, mode="throughput",
                  producers=4, consumers=4, qos="all-int", capacity=1024)
        lo = vals("matrix_v2", q, oversubscribe=1, mode="throughput",
                  producers=4, consumers=4, qos="all-bg", capacity=1024)
        report(f"F2 {q} all-bg/all-int retention",
               np.median(lo) / np.median(hi), boot_ratio(lo, hi), "x")

    # F8: prefix-fix RSS and throughput effects at 1:7 (h3).
    mem = dict(mode="throughput", producers=1, consumers=7, oversubscribe=1,
               qos="none", capacity=1024)
    leg = vals("matrix_h3", "ms", value="peak_rss_mb", **mem)
    fix = vals("matrix_h3", "ms-fix", value="peak_rss_mb", **mem)
    report("F8 1:7 RSS legacy/fix ratio", np.median(leg) / np.median(fix),
           boot_ratio(leg, fix), "x")
    legt = vals("matrix_h3", "ms", **mem)
    fixt = vals("matrix_h3", "ms-fix", **mem)
    report("F8 1:7 throughput fix/legacy ratio", np.median(fixt) / np.median(legt),
           boot_ratio(fixt, legt), "x")


if __name__ == "__main__":
    main()
