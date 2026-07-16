# Matrix summary (ind)

selected rows: 567; calib drift: 96.7–429.3 ms
- `paper/data/matrix_ind.csv`: seconds=2; 63 configurations; retained measured trials 1,2,3,4,5,6,7,8
Per-configuration retained sample n: 8–8.

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | faa | moody | ms | mutex | rigtorp | spsc | vyukov | xenium |
|---|---|---|---|---|---|---|---|---|
| 1:1 | 125.57 | 26.28 | 11.1 | 34.17 | 120.32 | 359.92 | 115.7 | 11.34 |
| 1:7 | 20.05 | 7.06 | 4.75 | 11.15 | 6.65 |  | 5.55 | 2.6 |
| 2:6 | 13.4 | 8.57 | 5.85 | 15.77 | 7.51 |  | 5.97 | 3.32 |
| 4:4 | 16.38 | 7.64 | 5.84 | 19.03 | 9.6 |  | 7.67 | 4.24 |

## Oversubscription (4P:4C, capacity 1024), throughput medians
| oversubscribe | faa | moody | ms | mutex | rigtorp | vyukov | xenium |
|---|---|---|---|---|---|---|---|
| 1 | 16.38 | 7.64 | 5.84 | 19.03 | 9.6 | 7.67 | 4.24 |
| 2 | 19.18 | 7.82 | 5.36 | 14.96 | 0.12 | 2.1 | 3.53 |
| 4 | 29.62 | 7.68 | 5.22 | 14.54 | 0.04 | 1.66 | 3.65 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | faa | moody | ms | mutex | rigtorp | vyukov | xenium |
|---|---|---|---|---|---|---|---|
| 1 | 4479.5 | 435 | 83.8 | 168.1 | 20894.2 | 317.6 | 461.8 |
| 4 | 13938.8 | 125262 | 41214.1 | 104434 | 5.00903e+07 | 78127.4 | 54382.5 |
