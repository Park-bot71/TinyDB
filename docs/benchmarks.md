# Reproducible benchmarks

For the comprehensive 1K–1M lookup, 1–100 worker and real-crash validation matrix,
use [the validation-suite driver](benchmark-validation.md). Its current outputs
are [benchmarks/results.csv](../benchmarks/results.csv) and
[benchmarks/report.md](../benchmarks/report.md). This document describes the
original smaller throughput and retained-log-size harness, which remains usable.

`tinydb_bench` is a C++20 executable using real files, the actual heap/index and
the transactional coordinator. Every workload checks resulting records/counts;
an incorrect result fails the run. It never deletes/reuses an existing database
directory. Results describe this implementation and workload, not an external
database comparison or a general performance guarantee.

## Run

```bash
scripts/benchmark.sh /tmp/tinydb-results \
  --rows 400 --lookups 200 --transactions 30 --recovery-transactions 200 \
  --clients 4 --frames 4 --seed 20261019
```

The destination must not exist and its parent must exist. The script builds
Release with tests and sanitizers disabled, records the actual compiler/build
flags, CPU, memory, kernel, filesystem and command, and runs three fresh-process
repetitions. It writes `run-1.csv` through `run-3.csv`, `environment.txt`,
`build.log`, `summary.md` and three database directories. Python 3's standard
library summarizes medians and min–max ranges. Required utilities are CMake,
a C++20 compiler, POSIX/Linux tools (`lscpu`, `df`) and Python 3.

Set `TINYDB_BENCH_BUILD_DIR` to choose a separate Release build directory. The
locally extracted CMake can be selected using the PATH/LD_LIBRARY_PATH exports
in README. Direct single-run usage:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --parallel 2
./build-release/tinydb_bench --dir /tmp/fresh-run --rows 400 --frames 4 > results.csv
python3 scripts/summarize_benchmarks.py /tmp/tinydb-results
```

Defaults: rows=200, lookups=100, transactions=20 per client,
recovery-transactions=200 per added stage, clients=4, frames=16 per file,
seed=20261019. The executable requires `--dir`, accepts only option/value pairs,
validates numeric ranges and fails if the destination already exists. The
script accepts the same workload options; it supplies each run's directory.

## What each number measures

| CSV workload | Timed work and operation unit |
| --- | --- |
| `raw_heap_index_insert_sync` | Seeded shuffled insertion of all rows into an independent heap/index, including their final sync; operations are rows |
| `raw_indexed_lookup` | Same seeded key stream, index find plus heap RID fetch and record validation; operations are lookups |
| `raw_heap_scan_lookup` | Same keys, public heap.scan copying the entire heap followed by caller filtering/decoding until a match; operations are lookups |
| `raw_index_range_max20` | Index range plus heap fetch/validation of up to 20 rows from each probe key; operations are range requests |
| `durable_bulk_insert_one_tx` | All rows inserted into one transaction, including forced COMMIT; operations are rows, not independently committed transactions |
| `durable_update_1_client` | Read-for-update/increment/update/forced commit, one transaction per operation |
| `durable_update_N_clients` | N threads updating the same hot key, each doing transactions operations; timed interval releases already-created waiting threads through join; operations are completed transactions |
| `recovery_stageS_sampleR` | Full database constructor: WAL parse/force, reconstruction/fsync and normal startup validation; operations=1 reopen |

Raw records have 96-byte names and balance=key. Transactional insertion uses
`account` names, and all updates increment key zero. Both serial and concurrent
transaction workloads use the same prepopulated database; writers remain
serialized by the engine's writer gate. Multi-client numbers include contention
and synchronization, not parallel write scaling. A 30-second lock timeout applies;
timeouts/errors fail the benchmark rather than being silently retried.

Recovery uses a separate **one-row database** at every stage. Stage 0 contains its
initial committed insert; stages 1 and 2 each add recovery-transactions committed
updates of that same row. Each size is reopened three times per process without
appending more log between samples. Read-only raw inspection verifies the
recovered balance outside the timed interval. This isolates retained-history
growth from row-count growth, and includes startup validation/fsync costs rather
than pretending to measure redo CPU alone. These are normal opens of durable
files; fault-injected correctness tests separately establish crash behavior.

CSV includes cumulative WAL bytes at workload completion and recovered redo-page
count. Heap/index hit/miss counters are deltas over raw lookup/range workloads;
raw insertion includes initial startup activity. A zero rate with zero fetches
means not measured, including every transaction/recovery row. Allocator I/O and
OS page-cache hits are outside these cache counters. Startup validation and one
indexed probe warmup occur before timed lookup comparison. Timed comparison order
is indexed, heap scan, then range. All use the same heap and key stream; scan
measures the current materializing scan API rather than an optimized streaming
scan that stops disk reads after a match.

## Interpretation and limits

Each process starts with fresh database files. No OS cache is dropped between
workloads or repetitions; recovery rereads warm files. There is no CPU affinity,
frequency control, isolated device or simultaneous-run guarantee enforced by the
script. Run without other builds/tests for recorded results. Three process runs
provide three observations per non-recovery row; recovery has nine observations
per size. Min–max is observed spread, not a confidence interval. Very short
workloads, host scheduling and fsync/storage cache behavior can dominate results.

Compare buffer configurations with separate destinations and identical options.
Durable transaction results include WAL force, next-writer baseline flush and
locking costs. Raw operations have no transaction/crash-atomicity guarantee and
must not be presented as durable transaction throughput. Full-page logging and
unbounded retained history impose substantial overhead. The workload does not
measure cold-cache device latency, mixed production traffic, percentiles, scale
to large databases or any SQLite/PostgreSQL comparison. Actual retained results
and their configurations are linked from README; no improvement is inferred
from a correctness test's elapsed time.
