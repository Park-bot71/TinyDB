# Comprehensive benchmark and correctness validation

Run `python3 scripts/validate_benchmarks.py` from a working checkout with CMake,
a C++20 compiler, GoogleTest sources/package, Python 3.9+ with timezone data and
Linux utilities (`lscpu`, `free`, `df`). No network download is enabled. As in
README, set PATH/LD_LIBRARY_PATH to the locally extracted tools if needed and use
`--gtest-source /tmp/custom-db-tools/root/usr/src/googletest`.

The suite builds `tinydb_validation` and the full test suite in `build-validation`
as **Release, `-O3 -DNDEBUG`, sanitizers off**. Existing storage, buffer, B+ tree,
transaction and WAL implementations are used without changing their behavior.
The previous `tinydb_bench`/`scripts/benchmark.sh` harness remains independently
available. The comprehensive suite is the source of the requested current files:

- `benchmarks/results.csv`: one typed workload/scenario result per row; missing
  measurements are empty, never zero-filled estimates.
- `benchmarks/report.md`: hardware, flags, methodology, measured tables, every
  failed/incomplete/unsupported case, and 3–5 resume metrics selected only from
  passing, verified results.
- `benchmarks/evidence/TIMESTAMP/`: commands, build logs, CTest JUnit and logs,
  source/binary hashes, worker progress, identical lookup keys and individual
  latency samples. Timestamps use Asia/Kolkata.

Database scratch files live in a newly created `/tmp/tinydb-validation-*`
directory, whose path is recorded in environment metadata. No existing database
is reused, removed or overwritten. `--work-parent` selects another existing
parent; timings describe that filesystem. Results/evidence are persisted after
each completed process. Each rerun creates a new evidence directory and replaces
only the two current CSV/report files. Old evidence is retained. A killed or
interrupted large setup cannot be reused as a valid dataset.

Markdown can be regenerated from retained CSV and matching evidence without
running measurements again:

```bash
python3 scripts/validate_benchmarks.py --render-evidence benchmarks/evidence/TIMESTAMP
```

The renderer checks the CSV against its original worker logs and JUnit, retained
lookup samples, probe hashes, cache arithmetic, transaction counts and paired
speedup calculations before replacing the report. It rejects a CSV belonging to
another evidence run. Measurement checks remain active with Python `-O`.
To check the
driver's rejection of altered probe keys, missing samples and incorrect reported
medians/throughput, plus real subprocess timeout handling, run:

```bash
python3 tests/benchmark_driver_tests.py build-validation/tinydb_validation
```

Those tests generate actual small temporary C++ workloads; their deliberate
evidence corruption is never included in benchmark results.
When CMake finds Python 3, these tests also run automatically as the
`BenchmarkDriverTests` CTest entry. Invalid crash-worker exit status or incomplete
scenario results fail validation before any crash pass is accepted.

## Workloads and timing

**Index comparison:** default sizes are 1000, 10000, 100000 and 1000000. A seeded
shuffle determines insertion order; `HeapFile.insert` and `BPlusTree.insert`
populate real pages, then sync and check every row/RID/index binding and tree
invariant. Names are `benchmark`, balances equal keys, primary keys are unique.
The engine's current heap insertion scans existing pages to locate free space;
setup can become expensive. There is no bulk-load shortcut or fabricated data file.

The seeded probe file is written once per dataset. Independently opened indexed
and sequential processes validate the same database and warm the same first
1000 indexed probes before measuring **10,000 requests per method** by default.
The orchestrator refuses fewer than 10,000 for a recorded benchmark. There is
one measured pass per method, indexed then sequential. This is explicitly warm
OS cache; the bounded CLOCK state follows real accesses and is not reset to
pretend that disks are cold. No OS caches are dropped or CPU frequency controlled.
The sequential method calls the existing full-heap-copy `HeapFile.scan` each
time and filters/decodes until a match. It never searches a preloaded `std::vector`
instead of the database. Indexed lookup includes index descent plus heap RID fetch.
Both verify the exact returned record inside the timed request.

Latency is an actual per-request `steady_clock` interval in microseconds. The
reported median is computed from all requests, not total elapsed/count. Throughput
uses total measured loop wall time, including timer/sample bookkeeping and ten
progress-log writes. Setup, startup validation, warmup, writing samples and final
invariant checks are excluded. Samples are retained; Python recomputes their
median and count and checks their keys before accepting results. The report
derives both median-latency and throughput speedup only when **both** complete
methods passed. Raw index/heap operations are not transactions.

**Concurrency:** 1, 2, 4, 8, 16, 32, 64 and 100 workers each attempt 100 two-account
transactions. Each does BEGIN, read-for-update on both keys, decrement Alice,
increment Bob, two updates and forced COMMIT. A start latch excludes thread
creation from timing; elapsed time ends after join. No retries or fake parallel
execution. Failed attempts and LockTimeout errors are recorded separately with
messages; successes/seconds uses acknowledged successful transactions only.
The 30-second lock timeout is unchanged across worker counts. A thread-resource
failure marks that requested count unsupported on this host rather than claiming
it ran. The engine still has one writer gate; these measure contention and
correctness, not parallel-write speedup.

Every completed concurrency count verifies exact balances against successful
commits, total balance conservation, tree invariants, heap/index bindings and no
active grants/waiters/transactions. It closes/reopens, runs recovery and verifies
again. Checks occur outside timed throughput. An indeterminate commit error is
not silently counted as a successful acknowledged operation; a recovered-state
disagreement fails correctness.

**Buffer pool:** reports per-file capacity plus actual hits, misses, hit rate and
evictions. Lookup measurements use counter deltas excluding startup/warmup.
The dedicated test uses 128 real pages and 10,000 guarded page fetches, alternating
16 hot pages repeated eight times and a full 128-page traversal. Capacities are
1, 16, 64 and 128 frames; every fetched page is validated. A column with no fetches
is N/A, even when the stored numeric hit rate is zero. Allocator I/O and OS caching
are outside engine cache counters. TransactionalDatabase does not expose buffer
statistics; the suite does not modify its API or invent transactional hit rates.

**Crashes:** 16 distinct named crash points cover before commit, between record
updates, forced COMMIT with a stale data page, acknowledged commit without close,
torn loser/winner pages, forced abort/interrupted undo, incomplete log frames,
and five reconstruction/publication points. Every case gets a fresh seeded
database. A separate exec'd process must hit the specified point and `_exit(86)`
without cleanup. Recovery cases first crash a committed writer, then a recovering
process. Exact records and structural invariants are checked on two normal
restarts. An offline read after forced COMMIT verifies one data page is actually
stale. Scenario count comes from distinct passing scenario names, not exception
injections, repeated restarts or GoogleTest test count. No device power-cut or
arbitrary WAL/baseline media-damage claim applies.

## Limits and statuses

```bash
# Remove per-dataset time limits to attempt the entire scale without a deadline:
python3 scripts/validate_benchmarks.py --setup-timeout 0 --lookup-timeout 0
# Select workload sizes or another output/build location:
python3 scripts/validate_benchmarks.py --datasets 1000 10000 --workers 1 8 100 \
  --output /tmp/my-benchmark-output --build-dir /tmp/my-benchmark-build
```

Default process deadlines are setup=90 seconds, lookup=240 seconds and other
phases=180 seconds. Each child has a separate process group; timeout kills the
whole group, including recovery grandchildren. A large setup or scan exceeding
its budget is `timed_out`, not evidence that the engine cannot store that many
records. Dependent lookups are `not_run` when their setup is incomplete. No median,
throughput or speedup is assigned to unfinished work. Fully completed smaller
datasets remain usable evidence; no million-record claim follows from them.

`pass` requires actual results and validation. `completed_with_failures` counts
real unsuccessful transactions while still recording verified outcomes.
`fail` denotes a test, invariant, process or evidence mismatch. `unsupported`
requires an observed missing capability/resource. `not_run` and `timed_out` are
incomplete and appear explicitly in the report. CSV numeric fields are empty when
not measured. Exit status 0 means all requested work passed; 1 means an observed
failure; 2 means incomplete/unsupported work without an observed correctness
failure. Full correctness failures stop dependent performance measurements.

Repetitions, cache state, record size, scan API, local fsync behavior and the
single-writer gate constrain interpretation. Reported results are not latency
percentiles, confidence intervals, cold storage, large-scale extrapolations or
comparisons against another DBMS. Resume suggestions preserve these qualifiers.
