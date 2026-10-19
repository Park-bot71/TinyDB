# Benchmarks

Run the main correctness and performance suite:

```bash
python3 scripts/validate_benchmarks.py
```

It measures lookups, 1–100 transaction workers, buffer usage and real crash
recovery. See the [results](../benchmarks/report.md),
[CSV](../benchmarks/results.csv) and [run options](benchmark-validation.md).

A separate, smaller experiment measures throughput and recovery as WAL grows:

```bash
scripts/benchmark.sh /tmp/new-results
```

It builds Release and saves three fresh-process runs, hardware details and a
summary. The output directory must be new. See the
[recorded experiment](benchmark-results/2026-10-19/report.md).

Both tools use warm OS caches. Raw storage and durable transaction results have
different guarantees. Writers are serialized; results do not show parallel-write
scaling or cold-device performance.
