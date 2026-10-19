# Recorded benchmark experiment — 2026-10-19

Warm-cache Release measurements on an AMD Ryzen 5 7640HS (6 cores/12 threads),
15,530,904 KiB RAM, Linux/ext4. GCC 13.3, C++20, `-O3 -DNDEBUG`; sanitizers off.

## Workload

400 shuffled rows, 200 lookup/range requests, 30 update transactions per client,
one or four clients, seed 20261019. Each cache configuration has three independent
process runs. Recovery uses one row with growing committed history and nine opens
per log size. No cache drop, affinity or CPU-frequency control.

| Workload | Unit | Four frames | Sixteen frames |
| --- | --- | ---: | ---: |
| Raw heap + index insert including final sync | rows/s | 976.9 | 1275.5 |
| Raw indexed lookup + heap fetch | lookups/s | 8159.2 | 11165.1 |
| Raw heap.scan + key filtering | lookups/s | 1238.4 | 2449.0 |
| Raw indexed range + up to 20 heap fetches | requests/s | 927.4 | 1544.0 |
| Durable bulk insert, one transaction for 400 rows | rows/s | 477.2 | 475.2 |
| Durable update, one client | transactions/s | 635.5 | 649.4 |
| Durable update, four clients, same hot key | transactions/s, aggregate | 562.5 | 562.1 |

## Recovery

Full-open latency includes parsing, reconstruction, sync and validation.

| Committed updates after initial insert | WAL bytes | Redo page images | Four-frame median (min–max), ms | Sixteen-frame median (min–max), ms |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 58208 | 8 | 4.353 (4.041–5.028) | 4.558 (4.019–5.042) |
| 200 | 1744608 | 208 | 37.329 (35.827–38.010) | 37.131 (36.013–40.198) |
| 400 | 3431008 | 408 | 70.038 (68.453–72.583) | 70.316 (68.372–72.063) |

## Reproduce and inspect

```bash
scripts/benchmark.sh /tmp/fresh-results --rows 400 --lookups 200 \
  --transactions 30 --recovery-transactions 200 --clients 4 --frames 4 --seed 20261019
```

Repeat with `--frames 16` and a new destination. Raw calls are not transactional;
these small-workload results are not production-capacity estimates. The global
writer gate prevents parallel-write scaling. Full ranges and original metadata:

- Four frames: [summary](frames-4/summary.md), [environment](frames-4/environment.txt),
  [run 1](frames-4/run-1.csv), [run 2](frames-4/run-2.csv), [run 3](frames-4/run-3.csv).
- Sixteen frames: [summary](frames-16/summary.md), [environment](frames-16/environment.txt),
  [run 1](frames-16/run-1.csv), [run 2](frames-16/run-2.csv), [run 3](frames-16/run-3.csv).

See the [main benchmark suite](../../benchmark-validation.md) for current results.
