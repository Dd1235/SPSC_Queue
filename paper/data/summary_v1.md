# Matrix summary (v1)

selected rows: 408 of 410; configs: 82; measured trials: 1,2,3,4; calib drift: 96.9–171.5 ms
Non-exact latency samples excluded (sample buffer count differed from the exact scheduled count): moody=2.
Per-configuration retained sample n: 2–4.

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | faa | moody | ms | mutex | spsc | vyukov |
|---|---|---|---|---|---|---|
| 1:1 | 123.14 | 25.62 | 11.31 | 32.24 | 361.25 | 117.24 |
| 1:7 | 19.88 | 7.55 | 5.7 | 12.3 |  | 6.01 |
| 2:2 | 26.56 | 12.77 | 16.76 | 23.43 |  | 25.29 |
| 2:6 | 15.6 | 8.68 | 6.31 | 16.68 |  | 6.74 |
| 4:4 | 16.04 | 7.87 | 7.31 | 19.65 |  | 8.59 |
| 6:2 | 16 | 7.5 | 6.08 | 13.23 |  | 5.2 |
| 7:1 | 18.86 | 7.9 | 5.68 | 7.13 |  | 3.72 |

## Oversubscription (4P:4C, capacity 1024), throughput medians
| oversubscribe | faa | moody | ms | mutex | vyukov |
|---|---|---|---|---|---|
| 1 | 16.04 | 7.87 | 7.31 | 19.65 | 8.59 |
| 2 | 21.82 | 7.95 | 5.72 | 14.91 | 1.78 |
| 4 | 31.15 | 7.85 | 5.43 | 14.65 | 1.48 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | faa | moody | ms | mutex | vyukov |
|---|---|---|---|---|---|
| 1 | 8421.6 | 8879.4 | 9230.2 | 6980.9 | 9840.2 |
| 4 | 17954.1 | 125641 | 81158.7 | 71631.7 | 58715.1 |
