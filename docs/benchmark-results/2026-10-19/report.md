# Phase 6 measured results — 2026-10-19

These are actual Release measurements from the development machine, with raw
CSV retained below. The local date was October 19 (runs started October 18 at
20:10 UTC). These small, warm-cache workloads establish reproducible observations,
not production capacity or a comparison with another database engine.

## Configuration and reproduction

- AMD Ryzen 5 7640HS, six cores/twelve logical CPUs; reported memory 15,530,904 KiB.
- Linux 6.17.0-35-generic, x86-64; `/tmp` on ext4, `/dev/nvme0n1p5`.
- GCC 13.3.0, CMake 3.28.3, C++20, Release `-O3 -DNDEBUG`; sanitizers off.
- 400 shuffled raw/transaction rows, 200 seeded lookup/range requests, 30 update
  transactions per client, one and four clients, seed 20261019.
- Four or sixteen CLOCK frames **per file**; default B+ tree capacity 184.
- Separate one-row recovery database; stages add 0, 200 and 400 committed updates.
- Three independent executable runs per configuration, fresh files each time.
  Three recovery samples per size per run (nine observations per recovery size).
- Verification/build jobs were stopped before retained measurements. No affinity,
  cache drop or CPU frequency control; files/OS caches warm between operations.

```bash
scripts/benchmark.sh /tmp/fresh-results-f4 \
  --rows 400 --lookups 200 --transactions 30 --recovery-transactions 200 \
  --clients 4 --frames 4 --seed 20261019
scripts/benchmark.sh /tmp/fresh-results-f16 \
  --rows 400 --lookups 200 --transactions 30 --recovery-transactions 200 \
  --clients 4 --frames 16 --seed 20261019
```

Destinations must not exist. See [methodology](../../benchmarks.md) for precise
timing boundaries and guarantees, including the difference between raw operations
and durable transactions. Source files for the measured executable are identified
by [SHA-256 checksums](source-sha256.txt); the
[Release executable digest](binary-sha256.txt) identifies the measured local build.
From the repository root, use
`sha256sum -c docs/benchmark-results/2026-10-19/source-sha256.txt`.

## Throughput and cache observations

Median throughput across the three processes:

| Workload | Unit | Four frames | Sixteen frames |
| --- | --- | ---: | ---: |
| Raw heap + index insert including final sync | rows/s | 976.9 | 1275.5 |
| Raw indexed lookup + heap fetch | lookups/s | 8159.2 | 11165.1 |
| Raw heap.scan + key filtering | lookups/s | 1238.4 | 2449.0 |
| Raw indexed range + up to 20 heap fetches | requests/s | 927.4 | 1544.0 |
| Durable bulk insert, one transaction for 400 rows | rows/s | 477.2 | 475.2 |
| Durable update, one client | transactions/s | 635.5 | 649.4 |
| Durable update, four clients, same hot key | transactions/s, aggregate | 562.5 | 562.1 |

Indexed lookup heap/index hit rates were 26.500%/83.750% with four frames and
100%/100% with sixteen frames. Heap-scan lookup hit rates were 0.038% and 100%.
These are the engine's CLOCK-cache counters, excluding OS caching and allocator
I/O. Dataset/order/capacity matter: the sixteen-frame cache fits this raw heap.
The scan API copies the entire heap for every request, then the caller filters;
these numbers do not compare against a streaming early-exit scan.

Four clients did not improve aggregate update throughput in this hot-key workload.
The global writer gate still serializes writes; thread contention adds overhead.
No parallel write scaling is claimed. Raw records have 96-byte names while
transaction records use `account`, and raw calls lack transaction atomicity:
do not interpret raw versus durable rates as a controlled overhead ratio.

## Recovery versus retained history

Median full-open latency, including log checksums/parse/force, reconstruction,
file sync and normal validation. The logical database remains one row.

| Committed updates after initial insert | WAL bytes | Redo page images | Four-frame median (min–max), ms | Sixteen-frame median (min–max), ms |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 58208 | 8 | 4.353 (4.041–5.028) | 4.558 (4.019–5.042) |
| 200 | 1744608 | 208 | 37.329 (35.827–38.010) | 37.131 (36.013–40.198) |
| 400 | 3431008 | 408 | 70.038 (68.453–72.583) | 70.316 (68.372–72.063) |

This observed increase reflects full-history replay of repeated overwrites, even
though the materialized table stays small. It is not a forecast for larger logs
or cold storage. There is no checkpoint/log truncation, and these measurements
do not hide that startup and memory grow with retained history. Correctness under
actual process crashes is established separately by subprocess tests, not by
these clean-open timings.

## Retained evidence

| Frames per file | Raw process CSV | Summaries and environment |
| ---: | --- | --- |
| 4 | [run 1](frames-4/run-1.csv), [run 2](frames-4/run-2.csv), [run 3](frames-4/run-3.csv) | [all medians/min–max](frames-4/summary.md), [hardware/flags/command](frames-4/environment.txt), [build log](frames-4/build.log) |
| 16 | [run 1](frames-16/run-1.csv), [run 2](frames-16/run-2.csv), [run 3](frames-16/run-3.csv) | [all medians/min–max](frames-16/summary.md), [hardware/flags/command](frames-16/environment.txt), [build log](frames-16/build.log) |

The sixteen-frame raw insert run ranged from 311.168 to 415.076 ms; medians alone
would conceal that spread. Full per-workload ranges are retained. No confidence
interval, tail-latency, cold-device, physical-power-loss, race-detector or
large-database claim follows from this report.
