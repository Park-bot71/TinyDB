# Verification

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

The same reproduction commands below build and run both test executables.
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
