# TinyDB benchmark and correctness report

Run started: 2026-10-19T01:53:53.561386+05:30 (Asia/Kolkata).

[Full environment](evidence/20261019T015353+0530/environment.txt) · [Commands](evidence/20261019T015353+0530/commands.txt) · [Raw results](results.csv)

## Setup

| Item | Recorded value |
| --- | --- |
| CPU | AMD Ryzen 5 7640HS w/ Radeon 760M Graphics; 12 logical CPUs |
| Memory | 15903645696 bytes |
| System | Linux-6.17.0-35-generic-x86_64-with-glibc2.39 |
| Filesystem | /dev/nvme0n1p5 (ext4) |
| Compiler | c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Release flags | -O3 -DNDEBUG |

C++20 Release; sanitizers off; seed 20261019; 64 frames/file. Requested records: [1000, 10000, 100000, 1000000]; 10000 lookups per method.

Both methods validate the same files and warm the same first 1000 probes. OS caches stay warm. Sequential lookup copies the full heap each request. Per-request latency includes decoding/checks; throughput includes loop bookkeeping. Setup, warmup and final validation are outside timing. One measured pass per method; raw lookup is not a transaction. [Full methodology](../docs/benchmark-validation.md).

## Correctness validation

| Check | Executed | Passed | Failed | Status | Evidence |
| --- | ---: | ---: | ---: | --- | --- |
| ctest | 125 | 125 | 0 | pass | [log/results](evidence/20261019T015353+0530/correctness.xml) |
| ctest_review | 126 | 126 | 0 | pass | [log/results](evidence/20261019T015353+0530/review/correctness.xml) |

Counts are CTest entries, including a Python test suite. Review rows retain the original performance samples.

## B+ tree: measured comparisons

| Records | Method | Operations | Median latency, µs | Throughput, ops/s | Median speedup | Throughput speedup | Status |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1000 | indexed | 10000 | 89.790 | 11061.613 | 4.227 | 4.262 | pass |
| 1000 | sequential | 10000 | 379.568 | 2595.288 | — | — | pass |
| 10000 | indexed | 10000 | 94.564 | 9289.788 | 71.155 | 62.868 | pass |
| 10000 | sequential | 10000 | 6728.708 | 147.766 | — | — | pass |
| 100000 | indexed | — | — | — | — | — | not_run |
| 100000 | sequential | — | — | — | — | — | not_run |
| 1000000 | indexed | — | — | — | — | — | not_run |
| 1000000 | sequential | — | — | — | — | — | not_run |

## Transaction concurrency

| Workers | Attempted | Successful | Failed | Timeouts | Successful tx/s | Correctness | Status |
| ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 1 | 100 | 100 | 0 | 0 | 521.24 | pass | pass |
| 2 | 200 | 200 | 0 | 0 | 418.30 | pass | pass |
| 4 | 400 | 400 | 0 | 0 | 431.99 | pass | pass |
| 8 | 800 | 800 | 0 | 0 | 356.11 | pass | pass |
| 16 | 1600 | 1600 | 0 | 0 | 436.43 | pass | pass |
| 32 | 3200 | 3200 | 0 | 0 | 437.51 | pass | pass |
| 64 | 6400 | 6400 | 0 | 0 | 411.28 | pass | pass |
| 100 | 10000 | 10000 | 0 | 0 | 195.65 | pass | pass |

Each worker attempts 100 two-record transfers, including durable COMMIT, with no retries. Timing runs from thread release to join. Exact balances, tree invariants and lock cleanup are checked, then verified after reopening. Writers remain serialized; lock timeout is 30 seconds.

## Buffer pool

| Workload | Records/pages | Frames/file | Heap hits | Heap misses | Heap hit rate | Heap evictions | Index hit rate | Index evictions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| indexed | 1000 | 64 | 10000 | 0 | 100.00% | 0 | 100.00% | 0 |
| sequential | 1000 | 64 | 110000 | 0 | 100.00% | 0 | — | 0 |
| indexed | 10000 | 64 | 6348 | 3652 | 63.48% | 3652 | 93.41% | 1319 |
| sequential | 10000 | 64 | 33 | 1019967 | 0.00% | 1019967 | — | 0 |
| hot16_scan128 | 128 | 1 | 0 | 10000 | 0.00% | 9999 | — |  |
| hot16_scan128 | 128 | 16 | 4992 | 5008 | 49.92% | 4992 | — |  |
| hot16_scan128 | 128 | 64 | 4992 | 5008 | 49.92% | 4944 | — |  |
| hot16_scan128 | 128 | 128 | 9872 | 128 | 98.72% | 0 | — |  |

Lookup counters exclude warmup. The buffer test mixes 16 hot pages with scans of 128 pages; every fetch is validated. OS cache/allocator I/O is excluded; no fetches means N/A.

## Real process crashes

| Distinct scenario | Status | Correctness |
| --- | --- | --- |
| before_commit | pass | pass |
| between_updates | pass | pass |
| commit.after_force | pass | pass |
| acknowledged_no_close | pass | pass |
| data.heap.after_half_write | pass | pass |
| torn_after_commit | pass | pass |
| abort.after_force | pass | pass |
| torn_during_abort | pass | pass |
| wal.begin.after_half_frame | pass | pass |
| wal.update.after_half_frame | pass | pass |
| wal.commit.after_half_frame | pass | pass |
| recovery.after_baseline | pass | pass |
| recovery.redo_page | pass | pass |
| recovery.after_redo | pass | pass |
| recovery.after_half_page | pass | pass |
| recovery.after_publish_heap | pass | pass |

**16/16 distinct crash scenarios passed.** Each uses a fresh database and an exec'd child exiting without cleanup. Exact state and invariants are checked after two restarts. Cases include stale data after forced COMMIT and interrupted recovery. These are process/partial-write tests, not device power cuts.

## Failed, incomplete and unsupported work

- setup/prepare (rows=100000, workers=1): **timed_out**, process deadline 90s exceeded
- lookup/indexed (rows=100000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported
- lookup/sequential (rows=100000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported
- setup/prepare (rows=1000000, workers=1): **timed_out**, process deadline 90s exceeded
- lookup/indexed (rows=1000000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported
- lookup/sequential (rows=1000000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported

Process limits: setup 90s, lookup 240s, other phases 180s. Timeouts are incomplete work, not unsupported record counts; no performance is estimated. Use `--setup-timeout 0 --lookup-timeout 0` to remove dataset limits. Parallel writers, SQL, checkpointing, cold-device latency and transactional cache counters are outside this benchmark.

## Strongest verified resume metrics

- **126 Release CTest entries passed** (includes the Python driver suite).
- **71.16× median lookup speedup** at **10000 records**: 94.56 µs indexed, 10000 identical-key requests per method, warm OS cache, 64 frames/file.
- **100 workers committed 10000 transfers with zero failures**, at 195.7 tx/s; recovered balances verified, writers serialized.
- **16 distinct process-crash scenarios passed**, with exact state verified after two restarts each.
- Measured CLOCK hit rates of **0.0% at 1 frames** and **98.7% at 128 frames**, with **10000 verified page fetches** per configuration; workload-specific, not a general speedup.

Keep workload qualifiers in resume claims. These results do not establish million-record performance, parallel write scaling or production ACID certification.
