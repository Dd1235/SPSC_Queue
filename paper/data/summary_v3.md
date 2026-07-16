# Matrix summary (v3)

selected rows: 611 of 620; configs: 77; measured trials: 1,2,3,4,5,6,7; calib drift: 100.0–141.7 ms
Documented unsupported moody/cap64/x4 rows excluded: 4.
Non-exact latency samples excluded (sample buffer count differed from the exact scheduled count): moody=2, ms=1, vyukov=1, vyukov-b=1.
Per-configuration retained sample n: 5–7.

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | casticket | faa | moody | ms | mutex | spsc | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|---|
| 1:1 | 79.4 | 126.12 | 26.09 | 11 | 34.58 | 373.95 | 117.32 | 120.77 |
| 2:2 | 19.76 | 25.79 | 12.74 | 16.47 | 22.85 |  | 25.18 | 24.57 |
| 4:4 | 7.06 | 16.46 | 7.83 | 6.11 | 19.15 |  | 7.88 | 8 |

## Oversubscription (4P:4C, capacity 1024), throughput medians
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|
| 1 | 7.06 | 16.46 | 7.83 | 6.11 | 19.15 | 7.88 | 8 |
| 2 | 9.2 | 17.86 | 7.86 | 5.36 | 15.01 | 2.11 | 2.11 |
| 4 | 19.33 | 29.68 | 7.61 | 5.17 | 14.65 | 1.66 | 1.68 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|
| 1 | 206.1 | 582.7 | 736.8 | 8203.3 | 6101.8 | 24 | 45.3 |
| 4 | 14086.2 | 14195.5 | 107521 | 73332.8 | 69456.4 | 70571 | 74949.5 |
