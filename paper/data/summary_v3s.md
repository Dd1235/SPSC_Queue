# Matrix summary (v3s)

rows: 412  configs: 64  calib drift: 93.8–134.6 ms

## Throughput medians (Mops/s), dedicated cores, qos=none
| ratio | casticket | faa | moody | ms | mutex | spsc | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|---|
| 1:1 | 78.6 | 128.14 | 25.87 | 11.94 | 34.14 | 365.66 | 117.1 | 115.75 |
| 2:2 | 27.16 | 25.6 | 14.74 | 16.41 | 23.23 |  | 23.27 | 23.52 |
| 4:4 | 19.6 | 16.43 | 7.55 | 5.69 | 19.15 |  | 7.9 | 8.6 |

## Oversubscription (4P:4C), throughput medians
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|
| 1 | 19.6 | 16.43 | 7.55 | 5.69 | 19.15 | 7.9 | 8.6 |
| 2 | 12.52 | 19.6 | 7.88 | 5.47 | 14.76 | 2.03 | 2.6 |
| 4 | 18.34 | 19.98 | 7.89 | 5.36 | 14.54 | 1.58 | 1.43 |

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov | vyukov-b |
|---|---|---|---|---|---|---|---|
| 1 | 287.9 | 835.7 | 348.8 | 426.6 | 482.2 | 674.4 | 918.7 |
| 4 | 12313.5 | 12450.5 | 103123 | 73241.3 | 71514.1 | 95919 | 65130.3 |
