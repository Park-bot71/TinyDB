# Benchmarks

## Main suite

```bash
python3 scripts/validate_benchmarks.py
```

Use this for indexed/sequential lookup, 1–100 transaction workers, buffer-cache
statistics and real crash validation. Start with the
[current report](../benchmarks/report.md), [CSV](../benchmarks/results.csv),
and [run options and methodology](benchmark-validation.md).

The suite records hardware, compiler flags, raw samples and failed/incomplete
work. Measurements use actual database APIs; no metrics are estimated.

## Throughput and WAL-history experiment

```bash
scripts/benchmark.sh /tmp/new-results
```

This smaller harness builds Release and runs three fresh-process repetitions.
The output directory must not exist. It writes raw CSV, environment metadata
and a median/min–max summary. `TINYDB_BENCH_BUILD_DIR` selects the build directory.

It measures raw heap/index insertion with final sync, point and range reads,
durable bulk insert, hot-key update transactions and recovery at three WAL sizes.
Recovery keeps one logical row and adds committed update history; each size is
opened three times per process. Constructor timing includes replay, sync and
startup validation.

Both tools use warm OS caches without cache drops or CPU-frequency controls.
Raw lookup throughput and durable transaction throughput have different
persistence guarantees. There is one writer gate, and results do not establish
parallel write scaling, cold-device performance or production capacity.

See the [retained 400-row experiment](benchmark-results/2026-10-19/report.md).
