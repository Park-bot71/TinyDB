# Verification

## Comprehensive benchmark validation — 2026-10-19 (current)

The follow-up review passes **126/126 Release CTest entries**: the 125 GoogleTests
and the Python driver suite, now integrated into CTest. All eight Python tests
pass, including saved CSV/log/sample consistency, rejection before replacing an
existing report, evidence checks under Python `-O`, and crash-worker exit-status
consistency. The report revalidates retained evidence when regenerated. Review
JUnit/logs and source hashes are retained under the original evidence directory's
`review` subdirectory, and a `ctest_review` CSV row records the actual new result.
Performance observations below are retained from the original measured run;
the review does not substitute freshly invented or extrapolated metrics.

The recorded Release run passes **125/125 GoogleTests**, including two new
integration tests for the real benchmark worker. The worker tests exercise
indexed/sequential lookup sample keys and counts, successful multi-client
transactions, actual buffer eviction counters, refusal to overwrite datasets,
and all 16 separate-process crash points with two verified restarts each.
Those two new tests also pass in Debug with ASan/UBSan (1.81 seconds;
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`). This does not claim a
new full-suite sanitizer run or a leak/race-detector result.

The original four Python driver tests passed. They use genuine C++ samples in temporary files
and prove the verifier rejects changed keys, missing samples and incorrect
median/throughput summaries. A real sleeping child is killed by its deadline,
which must return timeout rather than success. Test fixtures do not populate the
published performance CSV.

The original comprehensive run saved 41 workload/scenario rows (plus the later
correctness-review row) in
[benchmarks/results.csv](../benchmarks/results.csv) and a generated
[report](../benchmarks/report.md), with raw latency samples, JUnit, compiler flags,
hardware, source/binary hashes, exact commands and worker logs. It completed
10,000 indexed plus 10,000 sequential requests on each of 1K and 10K records.
Both 100K and 1M setup processes were actually attempted and hit their explicit
90-second deadlines; dependent lookup rows are `not_run`, with no invented
performance. These are incomplete scale measurements, not unsupported record
counts or correctness failures. The suite exits 2 to expose that incompleteness.

All eight worker counts (1, 2, 4, 8, 16, 32, 64, 100) completed: **22,700
acknowledged two-record transfers, zero failed attempts**, exact balances and
structural/lock invariants checked before and after reopening each database.
The separate 16-scenario crash matrix also passes every case. Buffer capacities
1, 16, 64 and 128 each complete 10,000 verified real page fetches. The report
derives its five resume suggestions only from accepted measured results.

See [the suite documentation](benchmark-validation.md) for reproduction,
deadline controls, timing/cache boundaries and the distinction between raw index
performance and durable transactions. The database implementation was unchanged.
Earlier sections record verification at each historical phase.

## Phase 6 — 2026-10-19

**123/123 GoogleTests pass** in Debug and ASan/UBSan builds. Full runs took
32.77 seconds and 44.39 seconds respectively. After the final recovery-benchmark
and shutdown refinements, all 10 CLI/benchmark integration tests were rebuilt and
rerun in both configurations (0.50/0.79 seconds), passing. The earlier 111 tests,
including 18 real subprocess crash/recovery tests, remain in the full run.
Toolchain: GCC 13.3.0, CMake 3.28.3, GoogleTest 1.14.0, C++20 and Linux.
LeakSanitizer remains unavailable under the tracer; runs use
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`. No leak-free,
race-detector or physical power-loss result is claimed.

The ten CLI tests exercise complete account-transfer/rollback/reopen sessions,
autocommit, integer boundaries, quoted/escaped and empty names, comments,
case-insensitive commands, strict complete-line parsing, range/limit behavior,
constraint-error transaction aborts, invalid lifecycle/diagnostic calls, EOF/QUIT
rollback, WAL statistics and flush/validation. Separate exec'd shell processes
verify batch exit status/persistence and SIGINT/SIGTERM while waiting for input.
The signal test waits for the pending insert's acknowledgment, leaves a partial
SELECT line without a newline, sends each signal, requires exit 130/143 and
reopens to confirm no unfinished insert survived. A nonblocking wakeup pipe
closes the check-flag/begin-read race; no stream/storage work runs in the handler.

A benchmark subprocess smoke test runs every real workload with a one-frame
cache, verifies its CSV contains lookup/concurrency/recovery results, then proves
a second run refuses to reuse the existing directory. Workloads themselves check
records, index counts, exact concurrent increments and recovered values.

Two additional stress tests cover:

- Six writers and two scanning readers, each doing 30 transactions, at both one
  and eight frames per file. Each configuration completes 144 committed and 36
  aborted two-record transfers plus 60 scan transactions. Every scan sees total
  balance 20000; exact final balances match committed deltas, locks are cleaned
  up, and recovery preserves the results. Records fill individual heap pages.
- Four clients concurrently insert, abort, delete and reinsert disjoint key sets
  with one-frame caches and B+ tree capacity three. Across 160 transactions,
  structural splits/merges, free-page reuse and undo leave exactly 64 expected
  keys/names/balances. Reopen reconstructs and verifies every key plus all tree
  invariants. Thread failures are propagated to the test thread.

All seven test executables run through `scripts/verify.sh`. Real Release
benchmark results, three process repetitions per buffer configuration and raw
CSV/hardware/build metadata are retained in
[the measured report](benchmark-results/2026-10-19/report.md). Correctness test
times above are not performance measurements. Read [cli.md](cli.md) for shell
semantics and [benchmarks.md](benchmarks.md) for workload/timing boundaries.
Dynamic tables/catalogs, SQL, checkpointing/log recycling, parallel writers and
production-scale performance validation remain deferred.

## Phase 5 — 2026-10-19

**111/111 GoogleTests pass** in Debug and AddressSanitizer +
UndefinedBehaviorSanitizer builds: the earlier 93 tests plus 18 recovery tests.
The final full Debug run took 25.75 seconds; the full sanitizer run took 36.90
seconds. After the final error-message and crash-assertion edits, all 44
lock/transaction/recovery tests were rebuilt and rerun under sanitizers, passing
in 7.66 seconds. GCC 13.3.0, CMake 3.28.3, GoogleTest 1.14.0, Linux and C++20
remain unchanged. LeakSanitizer is unavailable under this environment's tracer;
commands use `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.
No leak-free, race-detector, device-power-loss or performance result is claimed.

The recovery executable is a separate exec'd process using real database/WAL
files. Named fault hooks call `_exit(86)` without cleanup. Tests cover:

- A loser interrupted between account updates or immediately before commit,
  including dirty-page eviction with one frame per pool.
- Durable COMMIT before all data flushes: an offline raw heap read proves one
  balance is still old, then recovery yields the entire committed pair. Another
  process exits after acknowledged commit without close/destructor cleanup.
- Half-written heap/index pages, half-written BEGIN/UPDATE/COMMIT log frames,
  durable ABORT before live undo and a crash during physical undo.
- Repeated crashes after baseline staging, during redo, halfway through page
  publication and between heap/index publication. Repeated successful opens
  yield identical results and valid heap/index/allocator structures.
- Structural loser rollback and committed inserts with B+ tree splits, page
  allocation/reuse, preserved RIDs and generation high water.
- Torn superblocks, truncated indexes and one/both missing materialized files.
  Recovery runs before ordinary file-header validation.
- Complete corrupt final/interior WAL frames, damaged payloads, missing WAL or
  manifest, and corrupt immutable baselines fail closed without publishing data.
- Independent coordinator/raw-file ownership, adoption of a preexisting nonempty
  unlogged heap/index pair, and refusal to adopt WAL-stamped data without its
  required recovery inputs.

Separate exception-injection tests verify failed commit force never reports
success, and an error after durable COMMIT cannot undo that decision. These
outcomes are indeterminate to the caller until recovery. Existing physical-undo
tests explicitly sync before comparing data-file bytes because commit now forces
WAL rather than data. Existing file-size-limit tests allow WAL failure before
the intended data write and require either completed abort or a failed handle
with the database unavailable.

The transaction demo was rerun in two separate invocations against one directory.
`diff` found identical output: both account updates commit, temporary updates
roll back, and final balances are Alice 800/Bob 700. All five test executables
are included by the verification commands below. See [recovery.md](recovery.md)
for the full-history reconstruction protocol, retained-WAL/baseline requirements
and filesystem assumptions. Incremental checkpointing, CLRs, log truncation,
the CLI and measured benchmarks remain deferred.

Earlier sections record evidence and limitations at each historical phase.

## Phase 4 — 2026-10-19

**93/93 GoogleTests pass** in the normal Debug build and under
AddressSanitizer + UndefinedBehaviorSanitizer: all 67 earlier tests plus 26 lock,
undo and transaction tests. Toolchain remains GCC 13.3.0, CMake 3.28.3 and
GoogleTest 1.14.0 on Linux, C++20. LeakSanitizer is still unavailable in this
process-traced environment, so sanitizer verification uses
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`. No leak-free or
race-detector result is claimed. The commands below now run four test executables.

The new tests verify the full five-mode compatibility matrix, shared grants,
upgrades to X/SIX, retained grants after timeout, FIFO writer admission, canceled
waiter registration tokens and lock-table cleanup. Transaction tests cover:

- BEGIN/COMMIT/ROLLBACK states and IDs, clean restart persistence after commit
  without database close, invalid terminal/moved handles and active-close refusal.
- Read-your-writes, multiple account updates, insert/delete undo, destructor undo,
  move-assignment cleanup, and handles that outlive the database wrapper.
- Automatic full rollback after duplicate/missing keys, invalid records/key
  changes/ranges, and a failed growing update after earlier successful updates.
- Physical restoration after deleting 50 full-page records, collapsing the
  index, inserting 80 replacement records and splitting/reusing/extending pages
  with one frame per pool. Baseline heap data pages, exact index file bytes
  (including RIDs/root/free-list state), file lengths and records are restored.
  Record-generation high water remains above the baseline to prevent reuse.
- Dirty-read blocking with an unrelated-key reader still proceeding, repeatable
  reads, missing-key locks blocking insert, empty-range phantom protection, and
  a scan waiting for the whole two-account commit.
- A scan-to-write SIX upgrade, timeout undo of prior updates, and a real lock
  cycle between the writer gate and an already-held read lock. Timeout victims
  undo before releasing locks; surviving operations see consistent values.
- Four concurrent clients doing 40 committed read/modify/write increments with
  `get_for_update`, without a lost increment.

Independent disk/cache tests verify metadata/type/free-chain restoration,
extension truncation, stale cached-image discard, pin rejection and generation
high water. Child processes use real `RLIMIT_FSIZE` failures: index allocation
fails after a heap insert, the heap is undone, and the database refuses new
clients; another test makes commit write a partial page and also prevents undo.
It requires FAILED state, no successful commit and an unavailable database.
These are real I/O-error tests, not claims of process-crash recovery.

The transaction demo was run twice against the same directory in separate
invocations. `diff` verified identical output: both account updates commit, both
temporary updates roll back, and final balances are Alice 800/Bob 700. This is
clean-restart persistence and live atomicity/isolation evidence; the RAM-only
undo journal does not provide WAL durability, torn-page repair or crash atomicity.
See transactions.md for the scope of serializable isolation and the writer gate.

## Phase 3 — 2026-10-19

**67/67 GoogleTests pass** in both the normal Debug build and the
AddressSanitizer + UndefinedBehaviorSanitizer build. This includes all 46 prior
storage/buffer regressions and 21 index/page/integration tests. Toolchain remains
GCC 13.3.0, CMake 3.28.3 and GoogleTest 1.14.0 on Linux, C++20. As before,
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` was used because this
environment cannot run LeakSanitizer under its process tracer. No leak-free or
race-detector result is claimed.

The index tests cover:

- Full 184-key leaf and internal codec round trips, little-endian fields, signed
  key extremes, malformed headers/counts/order/references and reserved fields.
- Typed page allocation across reopen, immutable allocated page types, reuse
  with a different type, and WAL force ordering for a nonzero index-page LSN.
- Duplicate rejection preserving the original value, absent keys, invalid RIDs,
  empty trees, root creation and reuse after the final deletion.
- 140 keys inserted in ascending, descending, and random order (seed 12345), with
  a complete invariant check and comparison to `std::map` after every insertion.
  These use one frame and capacity 3 to exercise tall trees and internal splits.
- Directed leaf borrowing from both sides and leaf merges from either side,
  verifying root collapse and structural event counters.
- 240 randomized insertions and full randomized deletion (seed `0xBEEF`), checking
  every invariant and all entries after every mutation. Internal merges,
  internal redistribution, left/right borrowing and multiple root collapses are
  required by assertions, not merely assumed to occur.
- 400 random CRUD operations for each capacity 3, 4, 5 and 8, seeds
  `20261019 + capacity`, validating after every operation and reopening every
  50 operations with a one-frame pool.
- The default 184-key capacity with 600 shuffled insertions and complete deletion
  (seed 12), checking invariants/model after every mutation and splitting full
  physical leaf pages. Tall-tree internal structural coverage uses the smaller
  capacities; no larger default-capacity stress result is implied.
- Half-open ranges crossing leaves, limits, missing bounds, signed key extremes,
  invalid ranges, and persistent free-page reuse without file growth.
- Correct rejection of capacity mismatches, closed handles, wrong file kinds,
  and checksummed structural corruption: wrong counts/separators/parents,
  duplicate child references, broken leaf links, underfull leaves and orphan pages.
- Real heap records inserted in reverse key order, synchronized and reopened,
  then found via persisted RIDs and scanned in key order.
- Four concurrent clients with disjoint keys inserting, looking up, deleting and
  scanning through one index; final contents and structure are verified.

A child process builds/splits an index, deletes records, synchronizes, and calls
`_exit` without destructors. The parent reopens and verifies the retained keys
and structure. A separate child uses a real file-size limit to make node
allocation fail: insertion raises `Io`, subsequent index use is rejected, and
the parent verifies the untouched empty index. These are persistence/error tests,
not a crash-recovery claim.

`index_demo` was run twice in separate invocations against the same new heap/index
pair. `diff` verified identical output in ascending key order (Alice, Bob, Carol).
The two files are not transactionally coordinated. No throughput, indexed-vs-scan
performance improvement, or ACID result is inferred from these correctness tests.
The reproduction commands below now run all three test executables.

## Phase 2 — 2026-10-19

Verified with GCC 13.3.0, CMake 3.28.3 and GoogleTest 1.14.0 on Linux, C++20,
Debug configuration. **46/46 tests pass** in the normal build and under
AddressSanitizer + UndefinedBehaviorSanitizer. This includes all 25 Phase 1
regressions and 21 new buffer/integration tests in `tests/buffer_tests.cpp`.
Leak detection remains disabled because of the process-traced sandbox:
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.

New coverage includes fixed capacity, cache hits, CLOCK second chances, pinned
victims, pool exhaustion before allocation, RAII and guard moves, exception
unpinning, dirty/clean eviction, explicit flush and close, pinned-flush rejection,
invalid requests, page deletion/reuse, LSN regression and invalid image rejection,
and guard lifetime beyond pool destruction. No test requires a mock disk manager.

WAL hook tests inspect the real data file inside the force callback, proving that
the old page is still on disk before force returns. Tests also verify that a
nonzero LSN with no provider cannot be written, a failed force retains the dirty
resident page, and retry can later complete eviction. This verifies the buffer
ordering contract, not an implemented WAL or log durability.

Concurrency tests hold two shared readers while an exclusive writer is pinned
and waiting, perform 400 increments by four writers on one page with no lost
updates, and perform another 400 increments across six pages through two frames
while a separate thread flushes. The last workload retries `Busy` without keeping
other guards. A 500-operation randomized cache workload (seed `20261019`) checks
contents through eviction and final reopen. A six-page heap test uses a single
frame, updates and reuses a freed page, synchronizes, and reopens with valid IDs.

A forked child uses `RLIMIT_FSIZE` to force a real dirty-page write failure. It
verifies that the pool retains dirty state and rejects subsequent cache hits
with `Io`. The parent verifies the unchanged on-disk contents. Existing subprocess
sync/exit and partial-page-write tests also pass through the updated code.
The storage demo was also run in two separate invocations against one new file;
`diff` confirmed identical persisted records and IDs after the cache integration.

The reproduction commands below build and run all current test executables.
See [buffer-pool.md](buffer-pool.md) for lifecycle and synchronization limits.
No transaction, B+ tree, WAL/recovery, throughput or speedup claim follows from
these correctness tests.

## Phase 1 — 2026-10-18

Verified on 2026-10-18 with GCC 13.3.0, CMake 3.28.3, GoogleTest 1.14.0,
C++20, Linux, Debug configuration. CMake/GoogleTest packages were extracted to
`/tmp/custom-db-tools/root`, with no system package changes. This is correctness
verification; elapsed test times are not engine benchmark results.

## Results

- Normal build: **25/25 GoogleTests pass**.
- AddressSanitizer + UndefinedBehaviorSanitizer build: **25/25 pass**,
  `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.
- Leak detection was unavailable: the initial sanitizer discovery process failed
  because LeakSanitizer does not support the environment's process tracer.
  Disabling leak detection allowed address/UB verification; no leak-free claim
  is made.
- `storage_demo` was run twice against the same new file in separate processes;
  `diff` confirmed identical output, including record IDs and balances.

## Coverage and limits

The tests exercise page checksums (single-bit mutation at every byte position),
exact record capacity, empty/oversized records, signed record serialization,
embedded NUL names, malformed lengths, compaction, stale IDs, failed growth,
page allocation/free-chain reuse across reopen, invalid IDs, exclusive ownership,
explicit close, file truncation, checksum damage, free-list cycles/orphans,
invalid metadata/version, and generation exhaustion.

A 1500-step page test checks invariants after every operation against an in-memory
model (seed `0x1234`). A 500-step heap test compares CRUD/scan results with a model
and closes/reopens every 25 steps (seed `20261018`). Four threads perform 200
inserts with reads, updates and deletes through one heap handle, leaving 100
expected records. These test call-level synchronization, not transaction isolation.

A forked child inserts and synchronizes a record, then uses `_exit` without
destructors. The parent successfully reopens and reads it. Another child sets
`RLIMIT_FSIZE` to 4196 bytes and ignores SIGXFSZ, causing a real partial page write
followed by EFBIG. The engine raises an I/O error and rejects subsequent use of
the failed handle; the parent verifies the partial length and corruption on
reopen. This verifies failure reporting and detection, not recovery.

There are no transaction, index, WAL, recovery or performance results yet.
Those subsystems and their milestone gates are described in architecture.md.

## Reproduce with the locally extracted tools

```bash
export PATH="/tmp/custom-db-tools/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/custom-db-tools/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
scripts/verify.sh -DTINYDB_GTEST_SOURCE_DIR=/tmp/custom-db-tools/root/usr/src/googletest
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  TINYDB_BUILD_DIR="$PWD/build-sanitize" scripts/verify.sh \
  -DTINYDB_SANITIZERS=ON \
  -DTINYDB_GTEST_SOURCE_DIR=/tmp/custom-db-tools/root/usr/src/googletest
```

On a machine with installed CMake and GoogleTest, omit the two exports and
`TINYDB_GTEST_SOURCE_DIR`. Omit `detect_leaks=0` when the execution environment
supports leak detection.
