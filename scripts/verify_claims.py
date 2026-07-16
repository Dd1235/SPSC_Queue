#!/usr/bin/env python3
"""Recompute the registered quantitative claims from the raw datasets.

Run after any data regeneration; CI-friendly (exit 1 on any FAIL). Committed
data use a 2% display-rounding-aware transcription check. ``--replication``
selects separately declared wider bands for machine-to-machine variability.
"""
import argparse
import sys
from decimal import Decimal
from pathlib import Path

import pandas as pd


from dataset_utils import DatasetError, load_dataset


DATA_DIR = Path("paper/data")
datasets = {}
selections = {}


def dataset(name):
    if name not in datasets:
        # matrix_v3s contains an abandoned partial 2.00 s run followed by the
        # complete 1.50 s mechanism run.  Select the latter explicitly; pooling
        # durations would give some arms more weight than others.
        seconds = 1.5 if name == "matrix_v3s" else None
        datasets[name], selections[name] = load_dataset(
            DATA_DIR / f"{name}.csv", seconds=seconds
        )
    return datasets[name]


def med(csv, value='throughput_mops', maxtrial=None, **kw):
    df = dataset(csv)
    d = df[df.trial > 0]
    if maxtrial is not None:
        d = d[d.trial <= maxtrial]
    for k, v in kw.items():
        if k not in d.columns:
            raise DatasetError(f"{csv}: missing filter column {k!r}")
        d = d[d[k] == v]
    if d.empty:
        raise DatasetError(f"{csv}: no rows for {kw}")
    return d.groupby('queue')[value].median()

checks = []
REPLICATION_MODE = False


def ck(name, claimed, actual, committed_tol=0.02, replication_tol=0.10,
       display_places=None):
    """Check committed-data transcription tightly or a new replication broadly."""
    if claimed == 0:
        limit = 0.0
    elif REPLICATION_MODE:
        limit = replication_tol * abs(claimed)
    else:
        # A displayed 1.7 represents values in [1.65, 1.75), while 1.70 is
        # more precise.  Preserve that declared precision in addition to the
        # tight relative transcription band.
        places = (display_places if display_places is not None
                  else max(0, -Decimal(str(claimed)).as_tuple().exponent))
        rounding_half_unit = 0.5 * (10 ** -places)
        limit = max(committed_tol * abs(claimed), rounding_half_unit)
    ok = abs(actual - claimed) <= limit
    checks.append((name, claimed, round(float(actual), 6), 'PASS' if ok else 'FAIL'))


def run_checks():
    t1 = med('matrix_v3', mode='throughput', producers=4, consumers=4,
             oversubscribe=1, qos='none', capacity=1024)
    t4 = med('matrix_v3', mode='throughput', producers=4, consumers=4,
             oversubscribe=4, qos='none', capacity=1024)
    for q, a, b in [('vyukov', 7.9, 1.7), ('vyukov-b', 8.0, 1.7),
                    ('casticket', 7.1, 19.3), ('faa', 16.5, 29.7)]:
        ck(f'controls {q} x1={a}', a, t1[q])
        ck(f'controls {q} x4={b}', b, t4[q])
    for q, a, b in [('mutex', 19.2, 14.7), ('ms', 6.1, 5.2)]:
        ck(f'oversub {q} x1={a}', a, t1[q])
        ck(f'oversub {q} x4={b}', b, t4[q])
    ck('oversub mutex decrease=24%', 24, 100 * (1 - t4['mutex'] / t1['mutex']))
    ck('oversub ms decrease=15%', 15, 100 * (1 - t4['ms'] / t1['ms']))
    ck('oversub vyukov decrease~80%', 80, 100 * (1 - t4['vyukov'] / t1['vyukov']))
    ck('FAA/CAS-ticket speedup x1=2.3x', 2.3, t1['faa'] / t1['casticket'])
    ck('FAA/CAS-ticket speedup x4=1.5x', 1.5, t4['faa'] / t4['casticket'])

    for csv, claimed in [('matrix_v1', 94), ('matrix_v2', 77), ('matrix_v3', 80)]:
        lo = med(csv, mode='throughput', producers=4, consumers=4,
                 oversubscribe=1, qos='none', capacity=1024)['faa']
        hi = med(csv, mode='throughput', producers=4, consumers=4,
                 oversubscribe=4, qos='none', capacity=1024)['faa']
        ck(f'{csv} FAA x1->x4 increase={claimed}%', claimed, 100 * (hi / lo - 1),
           replication_tol=0.20)

    l4 = med('matrix_v3', 'p999_ns', mode='latency', producers=4, consumers=4,
             oversubscribe=4, qos='none', capacity=1024, rate=1_000_000) / 1e6
    for q, value in [('ms', 73.3), ('vyukov', 70.6), ('vyukov-b', 75.0),
                     ('casticket', 14.1), ('faa', 14.2), ('mutex', 69.5),
                     ('moody', 107.5)]:
        ck(f'p99.9@x4 {q}={value}ms', value, l4[q])
    ck('ticket tail advantage~5x', 5, l4['mutex'] / l4['faa'],
       replication_tol=0.25)

    for cap, values in [
        (64, dict(faa=2.6, casticket=2.55, vyukov=0.9, ms=5.3, mutex=4.5)),
        (1024, dict(faa=29.7, casticket=19.3, vyukov=1.7, ms=5.2, mutex=14.7)),
        (8192, dict(faa=19.2, casticket=8.4, vyukov=2.4, ms=5.2, mutex=15.9)),
    ]:
        cap_data = med('matrix_v3', mode='throughput', producers=4, consumers=4,
                       oversubscribe=4, qos='none', capacity=cap)
        for q, value in values.items():
            ck(f'cap{cap} {q}={value}', value, cap_data[q])

    r11 = med('matrix_v2', mode='throughput', producers=1, consumers=1,
              oversubscribe=1, qos='none', capacity=1024)
    r22 = med('matrix_v2', mode='throughput', producers=2, consumers=2,
              oversubscribe=1, qos='none', capacity=1024)
    r44 = med('matrix_v2', mode='throughput', producers=4, consumers=4,
              oversubscribe=1, qos='none', capacity=1024)
    r26 = med('matrix_v2', mode='throughput', producers=2, consumers=6,
              oversubscribe=1, qos='none', capacity=1024)
    for q, value in [('faa', 127.7), ('vyukov', 118.4),
                     ('mutex', 34.7), ('spsc', 373.6)]:
        ck(f'1:1 {q}={value}', value, r11[q])
    for q, value in [('faa', 26.3), ('vyukov', 25.2), ('mutex', 23.3)]:
        ck(f'2:2 {q}={value}', value, r22[q])
    for q, value in [('mutex', 19.0), ('faa', 16.5), ('ms', 6.2)]:
        ck(f'4:4 {q}={value}', value, r44[q])
    for q, value in [('mutex', 15.9), ('faa', 14.6)]:
        ck(f'2:6 {q}={value}', value, r26[q])
    ck('1:1->2:2 FAA loss~79%', 79, 100 * (1 - r22['faa'] / r11['faa']))
    ck('1:1->2:2 vyukov loss~79%', 79, 100 * (1 - r22['vyukov'] / r11['vyukov']))
    ck('1:1->2:2 mutex loss~33%', 33, 100 * (1 - r22['mutex'] / r11['mutex']))
    ck('SPSC/FAA specialization gap=2.93x', 2.93, r11['spsc'] / r11['faa'])

    qos_data = dataset('matrix_v2')
    qos_data = qos_data[qos_data.trial > 0]
    qq = qos_data[(qos_data['mode'] == 'throughput') & (qos_data.producers == 4) &
                  (qos_data.consumers == 4) & (qos_data.oversubscribe == 1) &
                  (qos_data.capacity == 1024)]
    piv = qq.groupby(['qos', 'queue']).throughput_mops.median().unstack()
    for qos, queue, value in [
        ('all-int', 'faa', 16.7), ('all-bg', 'faa', 14.7),
        ('all-int', 'mutex', 19.1), ('all-bg', 'mutex', 3.6),
        ('all-int', 'vyukov', 8.1), ('all-bg', 'vyukov', 0.46),
    ]:
        ck(f'{qos} {queue}={value}', value, piv.loc[qos, queue],
           replication_tol=0.20)
    ck('background-QoS retention faa=88%', 88,
       100 * piv.loc['all-bg', 'faa'] / piv.loc['all-int', 'faa'],
       replication_tol=0.06)
    ck('background-QoS retention mutex=19%', 19,
       100 * piv.loc['all-bg', 'mutex'] / piv.loc['all-int', 'mutex'],
       replication_tol=0.20)
    ck('background-QoS retention vyukov=6%', 6,
       100 * piv.loc['all-bg', 'vyukov'] / piv.loc['all-int', 'vyukov'],
       replication_tol=0.20)

    for qos, queue, value in [
        ('prod-bg', 'ms', 2.1), ('cons-bg', 'ms', 4.3),
        ('prod-bg', 'moody', 4.5), ('cons-bg', 'moody', 2.1),
    ]:
        ck(f'role QoS {qos} {queue}={value}', value, piv.loc[qos, queue],
           replication_tol=0.25)

    # These checks transcribe a specific historical scheduler event.  They are
    # important evidence for the paper's variability disclosure, but neither
    # its trial number nor its exact winner count is a replication criterion.
    if not REPLICATION_MODE:
        all_bg = qq[qq.qos == 'all-bg'].pivot(index='trial', columns='queue',
                                              values='throughput_mops')
        for queue, value in [('faa', 0.0), ('vyukov', 0.092), ('mutex', 0.174),
                             ('moody', 0.253), ('ms', 2.65)]:
            ck(f'all-bg outlier trial4 {queue}={value}', value, all_bg.loc[4, queue],
               committed_tol=0.02)
        ck('all-bg paired rounds led by FAA=6', 6,
           int((all_bg.idxmax(axis=1) == 'faa').sum()), committed_tol=0)

    rss = med('matrix_v2', 'peak_rss_mb', mode='throughput', oversubscribe=1,
              qos='none', capacity=1024, producers=1, consumers=7)
    ck('RSS 1:7 ms=742MB', 742, rss['ms'], replication_tol=0.12)
    for producers, consumers, value in [(1, 1, 376), (2, 6, 680), (7, 1, 7)]:
        shape_rss = med('matrix_v2', 'peak_rss_mb', mode='throughput',
                        oversubscribe=1, qos='none', capacity=1024,
                        producers=producers, consumers=consumers)
        ck(f'RSS {producers}:{consumers} ms={value}MB', value, shape_rss['ms'],
           replication_tol=0.15)

    mechanism = med('matrix_v3s', 'retries_per_op', mode='throughput',
                    producers=4, consumers=4, oversubscribe=4, qos='none', capacity=1024)
    secondary = med('matrix_v3s', 'secondary_per_op', mode='throughput',
                    producers=4, consumers=4, oversubscribe=4, qos='none', capacity=1024)
    ck('mech vyukov 0.75 fails/op', 0.75, mechanism['vyukov'],
       replication_tol=0.20)
    ck('mech vyukov 1.69 rereads/op', 1.69, secondary['vyukov'],
       replication_tol=0.20)
    ck('mech faa 0 fails/op (exact)', 0.0, mechanism['faa'],
       committed_tol=0, replication_tol=0)
    ck('mech faa 0.057 slot yields/op', 0.057, secondary['faa'],
       replication_tol=0.25)
    switches = med('matrix_v3s', 'ivcsw', mode='throughput', producers=4,
                   consumers=4, oversubscribe=4, qos='none', capacity=1024) / 1000
    for queue, value in [('vyukov', 84.9), ('vyukov-b', 57.0), ('faa', 919.9)]:
        ck(f'mech ivcsw {queue}={value}k', value, switches[queue],
           replication_tol=0.30)

    p50 = med('matrix_v3', 'p50_ns', mode='latency', producers=4, consumers=4,
              oversubscribe=1, qos='none', capacity=1024, rate=1_000_000)
    ck('p50 faa~375ns', 375, p50['faa'], replication_tol=0.15)
    ck('p50 mutex~1.79us', 1790, p50['mutex'], replication_tol=0.15)
    p99 = med('matrix_v3', 'p99_ns', mode='latency', producers=4, consumers=4,
              oversubscribe=1, qos='none', capacity=1024, rate=1_000_000) / 1000
    for queue, value in [('mutex', 262), ('ms', 21.2), ('vyukov', 1.8),
                         ('moody', 12.5)]:
        ck(f'p99 x1 {queue}={value}us', value, p99[queue], replication_tol=0.25)
    p999 = med('matrix_v3', 'p999_ns', mode='latency', producers=4, consumers=4,
               oversubscribe=1, qos='none', capacity=1024, rate=1_000_000) / 1e6
    for queue, value in [('vyukov', 0.024), ('vyukov-b', 0.045),
                         ('casticket', 0.206), ('faa', 0.583), ('moody', 0.737),
                         ('ms', 8.20), ('mutex', 6.10)]:
        ck(f'p99.9 x1 {queue}={value}ms', value, p999[queue],
           replication_tol=0.25)

    for over, queue, value in [(1, 'faa', 0.0017), (4, 'faa', 0.0076),
                               (1, 'casticket', 0.014), (4, 'casticket', 0.010),
                               (4, 'mutex', 0.073), (4, 'ms', 0.150)]:
        balance = med('matrix_v3', 'fair_cov', mode='throughput', producers=4,
                      consumers=4, oversubscribe=over, qos='none', capacity=1024)
        places = 3 if value in (0.010, 0.150) else None
        ck(f'producer balance x{over} {queue}={value}', value, balance[queue],
           replication_tol=0.30, display_places=places)
    for producers, consumers, value in [(6, 2, 0.18), (7, 1, 0.19)]:
        balance = med('matrix_v2', 'fair_cov', mode='throughput',
                      producers=producers, consumers=consumers, oversubscribe=1,
                      qos='none', capacity=1024)
        ck(f'moody producer-heavy {producers}:{consumers} CoV={value}', value,
           balance['moody'], replication_tol=0.30)

    h3_rss_17 = med('matrix_h3', 'peak_rss_mb', mode='throughput', producers=1,
                    consumers=7, oversubscribe=1, qos='none', capacity=1024)
    h3_tp_17 = med('matrix_h3', mode='throughput', producers=1, consumers=7,
                   oversubscribe=1, qos='none', capacity=1024)
    h3_rss_26 = med('matrix_h3', 'peak_rss_mb', mode='throughput', producers=2,
                    consumers=6, oversubscribe=1, qos='none', capacity=1024)
    h3_tp_26 = med('matrix_h3', mode='throughput', producers=2, consumers=6,
                   oversubscribe=1, qos='none', capacity=1024)
    for name, claimed, actual in [
        ('F8 1:7 legacy RSS=626MB', 626, h3_rss_17['ms']),
        ('F8 1:7 prefix RSS=153MB', 153, h3_rss_17['ms-fix']),
        ('F8 1:7 legacy throughput=5.4', 5.4, h3_tp_17['ms']),
        ('F8 1:7 prefix throughput=9.4', 9.4, h3_tp_17['ms-fix']),
        ('F8 2:6 legacy RSS=536MB', 536, h3_rss_26['ms']),
        ('F8 2:6 prefix RSS=97MB', 97, h3_rss_26['ms-fix']),
        ('F8 2:6 legacy throughput=7.0', 7.0, h3_tp_26['ms']),
        ('F8 2:6 prefix throughput=8.9', 8.9, h3_tp_26['ms-fix']),
    ]:
        ck(name, claimed, actual, replication_tol=0.15)
    ck('F8 1:7 RSS reduction=76%', 76,
       100 * (1 - h3_rss_17['ms-fix'] / h3_rss_17['ms']), replication_tol=0.15)
    ck('F8 1:7 throughput increase=76%', 76,
       100 * (h3_tp_17['ms-fix'] / h3_tp_17['ms'] - 1), replication_tol=0.15)
    ck('F8 2:6 RSS reduction=82%', 82,
       100 * (1 - h3_rss_26['ms-fix'] / h3_rss_26['ms']), replication_tol=0.15)
    ck('F8 2:6 throughput increase=28%', 28,
       100 * (h3_tp_26['ms-fix'] / h3_tp_26['ms'] - 1), replication_tol=0.15)

    h3_rss_44 = med('matrix_h3', 'peak_rss_mb', mode='throughput', producers=4,
                    consumers=4, oversubscribe=1, qos='none', capacity=1024)
    h3_tp_44 = med('matrix_h3', mode='throughput', producers=4, consumers=4,
                   oversubscribe=1, qos='none', capacity=1024)
    ck('F8 4:4 RSS reduction=52%', 52,
       100 * (1 - h3_rss_44['ms-fix'] / h3_rss_44['ms']), replication_tol=0.20)
    ck('F8 4:4 throughput decrease=13%', 13,
       100 * (1 - h3_tp_44['ms-fix'] / h3_tp_44['ms']), replication_tol=0.20)
    h3_rss_11 = med('matrix_h3', 'peak_rss_mb', mode='throughput', producers=1,
                    consumers=1, oversubscribe=1, qos='none', capacity=1024)
    ck('F8 1:1 legacy RSS~530MB', 541, h3_rss_11['ms'], replication_tol=0.15)
    ck('F8 1:1 prefix RSS~530MB', 526, h3_rss_11['ms-fix'], replication_tol=0.15)
    ck('F8 retry 1:7 RSS=717MB', 717, h3_rss_17['ms-retry'], replication_tol=0.15)
    ck('F8 retry 1:7 throughput=2.6', 2.6, h3_tp_17['ms-retry'],
       replication_tol=0.20)
    ck('F8 retry 1:7 throughput decrease=51%', 51,
       100 * (1 - h3_tp_17['ms-retry'] / h3_tp_17['ms']), replication_tol=0.20)

    # -- F8-M: instrumented reclamation mechanism (matrix_h3s, stats build) --
    def h3s_med(value, producers, consumers):
        return med('matrix_h3s', value=value, mode='throughput',
                   producers=producers, consumers=consumers,
                   oversubscribe=1, qos='none', capacity=1024)

    maint_17 = h3s_med('ebr_maint_ns_per_pass', 1, 7)
    adv_17 = h3s_med('ebr_adv_success_rate', 1, 7)
    limbo_17 = h3s_med('ebr_limbo_peak', 1, 7)
    rss_17s = h3s_med('peak_rss_mb', 1, 7)
    tp_17s = h3s_med('throughput_mops', 1, 7)
    ck('F8-M 1:7 legacy maint=79.6us/pass', 79.6, maint_17['ms'] / 1000)
    ck('F8-M 1:7 prefix maint=21.3us/pass', 21.3, maint_17['ms-fix'] / 1000)
    ck('F8-M 1:7 retry maint=168us/pass', 168, maint_17['ms-retry'] / 1000)
    ck('F8-M 1:7 adv success <0.3% in both non-retry modes', 1,
       float(adv_17['ms'] < 0.003 and adv_17['ms-fix'] < 0.003),
       committed_tol=0, replication_tol=0)
    ck('F8-M 1:7 retry adv success=68.6%', 68.6, 100 * adv_17['ms-retry'],
       replication_tol=0.30)
    ck('F8-M 1:7 prefix peak limbo LARGER than legacy', 1,
       float(limbo_17['ms-fix'] > limbo_17['ms']),
       committed_tol=0, replication_tol=0)
    ck('F8-M 1:7 replicated RSS 578->227MB', 578 / 227,
       rss_17s['ms'] / rss_17s['ms-fix'], replication_tol=0.30)
    ck('F8-M 1:7 replicated throughput 5.4->10.4', 10.4, tp_17s['ms-fix'],
       replication_tol=0.20)
    maint_11 = h3s_med('ebr_maint_ns_per_pass', 1, 1)
    adv_11 = h3s_med('ebr_adv_success_rate', 1, 1)
    limbo_11 = h3s_med('ebr_limbo_peak', 1, 1)
    rss_11s = h3s_med('peak_rss_mb', 1, 1)
    ck('F8-M 1:1 reclamation healthy in both modes '
       '(adv>=99.8%, maint<=5us, limbo<=3.5k)', 1,
       float(all(adv_11[q] >= 0.998 and maint_11[q] <= 5000 and
                 limbo_11[q] <= 3500 for q in ('ms', 'ms-fix'))),
       committed_tol=0, replication_tol=0)
    ck('F8-M 1:1 RSS ~550-600MB despite healthy reclamation', 1,
       float(all(500 <= rss_11s[q] <= 660 for q in ('ms', 'ms-fix'))),
       committed_tol=0, replication_tol=0)

    # -- F9: saturation knees (matrix_load + matrix_load2 offered-rate sweep) --
    def p50_us(csv, rate):
        return med(csv, value='p50_ns', mode='latency', producers=4,
                   consumers=4, oversubscribe=1, qos='none', capacity=1024,
                   rate=rate) / 1000

    ck('F9 faa p50 at 8M offered=0.4us', 0.4, p50_us('matrix_load2', 8e6)['faa'])
    ck('F9 casticket p50 at 8M offered=24.7us', 24.7,
       p50_us('matrix_load2', 8e6)['casticket'], replication_tol=0.50)
    ck('F9 mutex pre-knee gradient 0.8->52.5us (0.25M->4M)', 52.5 / 0.83,
       p50_us('matrix_load', 4e6)['mutex'] / p50_us('matrix_load', 250e3)['mutex'],
       committed_tol=0.05, replication_tol=0.50)
    ck('F9 knees ordered: ms+mutex<=6M, vyukov<=8M, faa>8M', 1,
       float(p50_us('matrix_load', 4e6)['ms'] < 1
             and p50_us('matrix_load2', 6e6)['ms'] > 10_000
             and p50_us('matrix_load2', 6e6)['mutex'] > 10_000
             and p50_us('matrix_load2', 6e6)['vyukov'] < 1_000
             and p50_us('matrix_load2', 8e6)['vyukov'] > 10_000
             and p50_us('matrix_load2', 8e6)['faa'] < 1
             and p50_us('matrix_load2', 12e6)['faa'] > 10_000),
       committed_tol=0, replication_tol=0)
    ck('F9 moody strand rows excluded=22', 22,
       selections['matrix_load2'].excluded_known_rows,
       committed_tol=0, replication_tol=0)

    # -- F10/F11: industrial cross-checks (matrix_ind) --
    def ind(value='throughput_mops', **kw):
        return med('matrix_ind', value=value, qos='none', **kw)

    t11 = ind(mode='throughput', producers=1, consumers=1, oversubscribe=1,
              capacity=1024)
    t44 = {o: ind(mode='throughput', producers=4, consumers=4, oversubscribe=o,
                  capacity=1024) for o in (1, 2, 4)}
    ck('F10 rigtorp matches faa at 1:1 (within 10%)', 1,
       float(abs(t11['rigtorp'] / t11['faa'] - 1) < 0.10),
       committed_tol=0, replication_tol=0)
    ck('F10 rigtorp 1:1=120.3', 120.3, t11['rigtorp'], replication_tol=0.15)
    ck('F10 rigtorp x4 collapse: faa/rigtorp ratio >= 700x', 1,
       float(t44[4]['faa'] / t44[4]['rigtorp'] >= 700),
       committed_tol=0, replication_tol=0)
    ck('F10 rigtorp below vyukov at x4', 1,
       float(t44[4]['rigtorp'] < t44[4]['vyukov']),
       committed_tol=0, replication_tol=0)
    lat1 = ind(value='p50_ns', mode='latency', producers=4, consumers=4,
               oversubscribe=1, capacity=1024, rate=1_000_000)
    lat4 = ind(value='p50_ns', mode='latency', producers=4, consumers=4,
               oversubscribe=4, capacity=1024, rate=1_000_000)
    ck('F10 rigtorp paced x1 p50 sub-us (matches faa)', 1,
       float(lat1['rigtorp'] < 1000 and lat1['faa'] < 1000),
       committed_tol=0, replication_tol=0)
    ck('F10 rigtorp paced x4 p50=25.7s backlog', 25.7,
       lat4['rigtorp'] / 1e9, replication_tol=0.50)

    rss = {pc: ind(value='peak_rss_mb', mode='throughput', producers=pc[0],
                   consumers=pc[1], oversubscribe=1, capacity=1024)
           for pc in ((1, 1), (2, 6), (1, 7))}
    ck('F11 xenium HP RSS at 1:7=760MB', 760, rss[(1, 7)]['xenium'],
       replication_tol=0.40)
    ck('F11 memory failure survives scheme swap (HP >= 100x mutex, all shapes)',
       1, float(all(rss[pc]['xenium'] >= 100 * rss[pc]['mutex']
                    for pc in rss)), committed_tol=0, replication_tol=0)
    tp17 = ind(mode='throughput', producers=1, consumers=7, oversubscribe=1,
               capacity=1024)
    ck('F11 HP throughput parity at 1:1 (within 10%)', 1,
       float(abs(t11['xenium'] / t11['ms'] - 1) < 0.10),
       committed_tol=0, replication_tol=0)
    ck('F11 HP cost at 1:7 = -45% vs EBR', 45.3,
       100 * (1 - tp17['xenium'] / tp17['ms']), replication_tol=0.40)
    t44c64 = ind(mode='throughput', producers=4, consumers=4, oversubscribe=4,
                 capacity=64)
    ck('F11 unbounded arms lead the cap64 x4 corner', 1,
       float(t44c64['ms'] > t44c64['faa'] and
             t44c64['xenium'] > t44c64['faa']),
       committed_tol=0, replication_tol=0)

    # -- T: sustained-soak thermal series. Deliberately raw: `trial` is a soak
    # index and soak 0 (cold) is data, so kept-rounds selection must not apply.
    thermal = pd.read_csv(DATA_DIR / 'matrix_thermal.csv')

    def soak_ratio(queue, column='throughput_mops'):
        d = thermal[thermal.queue == queue].sort_values('trial')
        cold = d[d.trial < 3][column].median()
        hot = d[d.trial >= d.trial.max() - 4][column].median()
        return hot / cold

    ck('T faa thermally flat (hot/cold=101.9%)', 101.9,
       100 * soak_ratio('faa'), replication_tol=0.10)
    ck('T ms sheds 19% under soak (hot/cold=81.3%)', 81.3,
       100 * soak_ratio('ms'), replication_tol=0.15)
    ck('T decay ordering ms > vyukov/mutex > faa', 1,
       float(soak_ratio('ms') < soak_ratio('vyukov') < soak_ratio('faa') and
             soak_ratio('ms') < soak_ratio('mutex') < soak_ratio('faa')),
       committed_tol=0, replication_tol=0)
    ck('T faa probe stays cold while vyukov probe heats', 1,
       float(soak_ratio('faa', 'calib_ns') < 1.05 and
             soak_ratio('vyukov', 'calib_ns') > 1.15),
       committed_tol=0, replication_tol=0)
    ck('T ranking stable under soak (no 4:4 crossings, last-5 medians)', 1,
       float((lambda m: m['mutex'] > m['faa'] > m['vyukov'] > m['ms'])(
           {q: thermal[(thermal.queue == q) &
                       (thermal.trial >= thermal.trial.max() - 4)]
            .throughput_mops.median() for q in ('faa', 'vyukov', 'ms', 'mutex')})),
       committed_tol=0, replication_tol=0)

    calib_rows = dataset('matrix_v2')
    calib = calib_rows[calib_rows.trial > 0].groupby('queue').calib_ns.median()
    ck('v2 queue-wise calibration spread=1.6%', 1.6,
       100 * (calib.max() / calib.min() - 1), replication_tol=0.50)
    ck('v2 analyzed configuration count=91', 91,
       selections['matrix_v2'].configurations, committed_tol=0, replication_tol=0)

def main():
    global DATA_DIR, REPLICATION_MODE
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", type=Path,
                    default=Path(__file__).resolve().parents[1] / "paper/data")
    ap.add_argument(
        "--replication",
        action="store_true",
        help="use wider predeclared bands for an independent machine/run",
    )
    args = ap.parse_args()
    DATA_DIR = args.data_dir
    REPLICATION_MODE = args.replication

    try:
        run_checks()
        for name in ("matrix_v1", "matrix_v2", "matrix_v3", "matrix_v3s", "matrix_h3"):
            dataset(name)
    except (DatasetError, KeyError) as exc:
        ap.error(str(exc))
    print("verification mode: " + ("independent replication bands" if REPLICATION_MODE
                                   else "committed-data transcription (2% default)"))
    for name in sorted(selections):
        print(f"{name}: {selections[name].describe()}")

    fails = [c for c in checks if c[3] == 'FAIL']
    for c in checks:
        print(f"{c[3]}  {c[0]:40s} claimed={c[1]:<9} actual={c[2]}")
    print(f"\n{len(checks)-len(fails)}/{len(checks)} PASS")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
