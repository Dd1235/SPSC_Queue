# Matrix summary (v3s)

selected rows: 227 of 412; configs: 77; measured trials: 1,2; calib drift: 93.8–134.6 ms
Non-exact latency samples excluded (sample buffer count differed from the exact scheduled count): moody=1, ms=2, vyukov=1.
Per-configuration retained sample n: 1–2.

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | casticket | faa | moody | ms | mutex | spsc | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|---|
| 1:1 | 78.43 | 129.36 | 25.7 | 11.78 | 33.87 | 363.92 | 116.78 | 112.91 |
| 2:2 | 26.59 | 25.36 | 13.45 | 16.32 | 22.98 |  | 23.51 | 23.07 |
| 4:4 | 15.93 | 16.48 | 7.94 | 5.78 | 19.05 |  | 8.05 | 8.4 |

## Oversubscription (4P:4C, capacity 1024), throughput medians
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|
| 1 | 15.93 | 16.48 | 7.94 | 5.78 | 19.05 | 8.05 | 8.4 |
| 2 | 12.52 | 24.74 | 8.01 | 5.45 | 14.35 | 1.64 | 1.79 |
| 4 | 18.2 | 30.17 | 7.73 | 5.42 | 14.73 | 1.72 | 1.66 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|
| 1 | 6948.5 | 951 | 234.6 | 2373 | 468.8 | 534.8 | 520.8 |
| 4 | 13410.4 | 12521.6 | 96572.1 | 39578.1 | 77145.5 | 99586 | 74460.2 |
