# Matrix summary (h3)

rows: 136  configs: 17  calib drift: 92.3–140.3 ms

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | ms | ms-fix | ms-retry | mutex | spsc |
|---|---|---|---|---|---|
| 1:1 | 11.82 | 11.31 | 11.89 | 33.7 | 344.35 |
| 1:7 | 5.36 | 9.41 | 2.6 | 10.93 |  |
| 2:6 | 6.99 | 8.92 | 3.48 | 15.6 |  |
| 4:4 | 6.33 | 5.51 | 5.49 | 19.03 |  |

## Oversubscription (4P:4C), throughput medians
| oversubscribe | ms | ms-fix | ms-retry | mutex |
|---|---|---|---|---|
| 1 | 6.33 | 5.51 | 5.49 | 19.03 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe |
|---|
