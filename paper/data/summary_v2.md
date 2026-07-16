# Matrix summary (v2)

selected rows: 718 of 780; configs: 91; measured trials: 1,2,3,4,5,6,7; calib drift: 92.8–148.3 ms
Incomplete/trailing trials excluded: 8.
Documented unsupported moody/cap64/x4 rows excluded: 1.
Non-exact latency samples excluded (sample buffer count differed from the exact scheduled count): moody=5, ms=2, vyukov=3.
Per-configuration retained sample n: 3–7.

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | faa | moody | ms | mutex | spsc | vyukov |
|---|---|---|---|---|---|---|
| 1:1 | 127.68 | 26.43 | 10.49 | 34.71 | 373.62 | 118.42 |
| 1:7 | 20.15 | 7.09 | 5.15 | 11.7 |  | 5.34 |
| 2:2 | 26.34 | 12.67 | 16.57 | 23.34 |  | 25.23 |
| 2:6 | 14.61 | 8.51 | 6.13 | 15.88 |  | 6.22 |
| 4:4 | 16.51 | 7.83 | 6.22 | 19 |  | 7.84 |
| 6:2 | 15.97 | 6.6 | 4.93 | 12.95 |  | 5.74 |
| 7:1 | 19.96 | 5.76 | 4.79 | 6.96 |  | 4.47 |

## Oversubscription (4P:4C, capacity 1024), throughput medians
| oversubscribe | faa | moody | ms | mutex | vyukov |
|---|---|---|---|---|---|
| 1 | 16.51 | 7.83 | 6.22 | 19 | 7.84 |
| 2 | 17.8 | 7.84 | 5.44 | 15 | 2.12 |
| 4 | 29.17 | 7.73 | 5.47 | 14.32 | 1.67 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | faa | moody | ms | mutex | vyukov |
|---|---|---|---|---|---|
| 1 | 556.3 | 661.2 | 272.4 | 386.5 | 469 |
| 4 | 15091.6 | 119124 | 98470.6 | 70687.6 | 68634 |
