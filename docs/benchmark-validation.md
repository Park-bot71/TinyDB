# Benchmark suite

```bash
python3 scripts/validate_benchmarks.py
```

Requires Linux, CMake, a C++20 compiler, GoogleTest and Python 3.9+.
Use `--gtest-source /path/to/googletest` for a local GoogleTest checkout.
The driver builds Release, runs tests and saves [CSV](../benchmarks/results.csv),
[a report](../benchmarks/report.md) and raw evidence using fresh databases.

| Workload | Default |
| --- | --- |
| Lookup | 1K, 10K, 100K, 1M rows; 10,000 requests per method |
| Transactions | 1, 2, 4, 8, 16, 32, 64, 100 workers; 100 transfers each |
| Buffer | 1, 16, 64, 128 frames; 10,000 fetches each |
| Recovery | 16 process-crash scenarios; two verified restarts each |

Lookup methods use identical keys and 1000 warmup probes; OS caches stay warm.
Indexed reads fetch the heap record; sequential reads copy and filter the full
heap. Timing includes decoding and result checks, excluding setup and warmup.
Median latency uses individual samples; throughput uses total loop time.

Transfers include two updates and durable commit. Balances, locks and tree
invariants are checked before and after reopening. Writers remain serialized.
Buffer counters exclude allocator I/O and OS caches.

Setup, lookup and other phases default to 90, 240 and 180 seconds respectively.
Timeouts are marked incomplete, with no estimated performance. Exit codes are
0 for passing, 1 for failure and 2 for incomplete/unsupported work.

```bash
# Disable dataset time limits
python3 scripts/validate_benchmarks.py --setup-timeout 0 --lookup-timeout 0
# Regenerate a report from checked evidence
python3 scripts/validate_benchmarks.py --render-evidence benchmarks/evidence/TIMESTAMP
```

Reports are checked against logs, samples, key hashes and JUnit, including under
Python `-O`. Use `--help` for all options.
