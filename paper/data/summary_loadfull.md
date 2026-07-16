# Matrix summary (loadfull)

selected rows: 362; calib drift: 94.9–203.8 ms
- `paper/data/matrix_load.csv`: seconds=2; 30 configurations; retained measured trials 1,2,3,4,5
- `paper/data/matrix_load2.csv`: seconds=2; 26 configurations; retained measured trials 1,2,3,4,5,6; excluded 22 documented unsupported rows
Per-configuration retained sample n: 5–6.

## Latency p99.9 (us), paced 1M/s 4P:4C
| oversubscribe | casticket | faa | moody | ms | mutex | vyukov |
|---|---|---|---|---|---|---|
| 1 | 913.8 | 418.5 | 554.6 | 492.7 | 344.2 | 7738.8 |
