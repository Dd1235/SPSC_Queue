#!/usr/bin/env python3
"""Recompute every quantitative claim in paper/main.tex from the raw datasets.

Run after any data regeneration; CI-friendly (exit 1 on any FAIL). Each check
names the claim as it appears in the paper. Tolerance defaults to 8% (medians
jitter run-to-run); tighter where the claim is exact.
"""
import sys
import pandas as pd

def med(csv, value='throughput_mops', maxtrial=None, **kw):
    df = pd.read_csv(f'paper/data/{csv}.csv')
    d = df[df.trial > 0]
    if maxtrial: d = d[d.trial <= maxtrial]
    for k, v in kw.items(): d = d[d[k] == v]
    return d.groupby('queue')[value].median()

checks = []
def ck(name, claimed, actual, tol=0.08):
    ok = abs(actual - claimed) <= tol * max(abs(claimed), 1e-9) or (claimed == 0 and actual == 0)
    checks.append((name, claimed, round(float(actual), 2), 'PASS' if ok else 'FAIL'))

t1 = med('matrix_v3', mode='throughput', producers=4, consumers=4, oversubscribe=1, qos='none', capacity=1024)
t4 = med('matrix_v3', mode='throughput', producers=4, consumers=4, oversubscribe=4, qos='none', capacity=1024)
for q, a, b in [('vyukov',7.9,1.7),('vyukov-b',8.0,1.7),('casticket',7.1,19.3),('faa',16.5,29.7)]:
    ck(f'controls {q} x1={a}', a, t1[q]); ck(f'controls {q} x4={b}', b, t4[q])

l4 = med('matrix_v3', 'p999_ns', mode='latency', producers=4, oversubscribe=4) / 1e6
for q, v in [('vyukov',68.7),('vyukov-b',67.5),('casticket',14.1),('faa',14.2)]:
    ck(f'p99.9@x4 {q}={v}ms', v, l4[q])

for cap, vals in [(64,dict(faa=2.6,casticket=2.6,vyukov=0.9,mutex=4.5)),
                  (1024,dict(faa=29.7,casticket=19.3,vyukov=1.7,mutex=14.7)),
                  (8192,dict(faa=19.2,casticket=8.4,vyukov=2.4,mutex=15.9))]:
    c = med('matrix_v3', mode='throughput', producers=4, consumers=4, oversubscribe=4, qos='none', capacity=cap)
    for q, v in vals.items(): ck(f'cap{cap} {q}={v}', v, c[q])

r11 = med('matrix_v2', maxtrial=7, mode='throughput', producers=1, consumers=1, oversubscribe=1, qos='none', capacity=1024)
r22 = med('matrix_v2', maxtrial=7, mode='throughput', producers=2, consumers=2, oversubscribe=1, qos='none', capacity=1024)
r44 = med('matrix_v2', maxtrial=7, mode='throughput', producers=4, consumers=4, oversubscribe=1, qos='none', capacity=1024)
for q, v in [('faa',127.7),('vyukov',118.4),('mutex',34.7),('spsc',373.6)]: ck(f'1:1 {q}={v}', v, r11[q])
for q, v in [('faa',26.3),('vyukov',25.2),('mutex',23.3)]: ck(f'2:2 {q}={v}', v, r22[q])
for q, v in [('mutex',19.0),('faa',16.5),('ms',6.2)]: ck(f'4:4 {q}={v}', v, r44[q])

q1 = pd.read_csv('paper/data/matrix_v1.csv'); q1 = q1[q1.trial > 0]
qq = q1[(q1['mode']=='throughput')&(q1.producers==4)&(q1.consumers==4)&(q1.oversubscribe==1)]
piv = qq.groupby(['qos','queue']).throughput_mops.median().unstack()
ck('E-core retention faa=96%', 96, 100*piv.loc['all-bg','faa']/piv.loc['all-int','faa'], tol=0.05)
ck('E-core retention mutex=13%', 13, 100*piv.loc['all-bg','mutex']/piv.loc['all-int','mutex'], tol=0.15)
ck('E-core retention vyukov=6%', 6, 100*piv.loc['all-bg','vyukov']/piv.loc['all-int','vyukov'], tol=0.15)

rss = med('matrix_v2', 'peak_rss_mb', maxtrial=7, mode='throughput', oversubscribe=1, qos='none',
          capacity=1024, producers=1, consumers=7)
ck('RSS 1:7 ms=742MB', 742, rss['ms'], tol=0.10)

m = med('matrix_v3s', 'retries_per_op', mode='throughput', producers=4, consumers=4, oversubscribe=4, qos='none', capacity=1024)
s2 = med('matrix_v3s', 'secondary_per_op', mode='throughput', producers=4, consumers=4, oversubscribe=4, qos='none', capacity=1024)
ck('mech vyukov 0.7 fails/op', 0.7, m['vyukov'], tol=0.20)
ck('mech vyukov 1.8 rereads/op', 1.8, s2['vyukov'], tol=0.20)
ck('mech faa 0 fails/op (exact)', 0.0, m['faa'], tol=0)

p50 = med('matrix_v3', 'p50_ns', mode='latency', producers=4, oversubscribe=1)
ck('p50 faa~333ns', 333, p50['faa'], tol=0.15)
ck('p50 mutex~1.75us', 1750, p50['mutex'], tol=0.15)

fails = [c for c in checks if c[3] == 'FAIL']
for c in checks: print(f"{c[3]}  {c[0]:40s} claimed={c[1]:<9} actual={c[2]}")
print(f"\n{len(checks)-len(fails)}/{len(checks)} PASS")
sys.exit(1 if fails else 0)
