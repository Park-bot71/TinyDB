# Benchmark suite

```bash
python3 scripts/validate_benchmarks.py
```

Requires CMake, a C++20 compiler, GoogleTest, Python 3.9+ and Linux tools
(`lscpu`, `free`, `df`). Use `--gtest-source /path/to/googletest` if needed.
The driver builds Release, runs correctness tests and saves
[results.csv](../benchmarks/results.csv), [report.md](../benchmarks/report.md)
and raw evidence. Each run uses a fresh temporary database directory.

## Default workloads

| Workload | Configuration |
| --- | --- |
| Lookup | 1K, 10K, 100K, 1M records; 10,000 identical-key requests per method |
| Transactions | 1, 2, 4, 8, 16, 32, 64, 100 workers; 100 transfers each |
| Buffer pool | 1, 16, 64, 128 frames; 10,000 real page fetches each |
| Recovery | 16 separate-process crash points; two verified restarts per case |

Lookup compares index-plus-heap reads against the existing full-heap-copy scan.
Both validate the dataset and warm the same first 1000 probes; OS caches are not
dropped. Per-request timings include decoding and correctness checks. Throughput
uses total loop time; setup, warmup and final validation are excluded. Medians and
speedups are checked against retained samples and matching keys.

Transaction timing includes BEGIN, two updates and durable COMMIT. Exact balances,
locks and tree invariants are checked before and after reopening. Writers remain
serialized; this measures contention, not parallel-write scaling. Cache counters
exclude allocator I/O and OS caches.

## Limits and evidence

Setup defaults to 90 seconds per dataset; lookup processes to 240 seconds; other
phases to 180 seconds. Timeouts are incomplete work, not unsupported capacities.
No performance is reported for unfinished runs. Exit codes are 0 for all passing,
1 for failures, and 2 for incomplete/unsupported work.

```bash
# Remove dataset time limits:
python3 scripts/validate_benchmarks.py --setup-timeout 0 --lookup-timeout 0
# Rebuild a report from existing evidence, without rerunning benchmarks:
python3 scripts/validate_benchmarks.py --render-evidence benchmarks/evidence/TIMESTAMP
```

Report regeneration checks CSV values against logs, samples, hashes and JUnit;
these checks remain active under Python `-O`. Python driver tests run in CTest
when Python is available, or directly with
`python3 tests/benchmark_driver_tests.py build-validation/tinydb_validation`.
