# TinyDB — disk-backed database engine in C++20

An incremental implementation of database internals on Linux. **All six phases
(storage, buffer pool, B+ tree, transactions, WAL/recovery, CLI and evaluation)
are implemented.** The CLI exposes one fixed-schema `accounts` table. Guarantees and filesystem
assumptions are scoped in the recovery guide; no unrestricted ACID certification
or media-failure recovery is claimed.

The engine currently provides:

- Persistent 4096-byte pages with explicit little-endian encoding and CRC-32.
- Page allocation and a free-page chain that survives reopen.
- Slotted heap records with insert, read, variable-length update, delete and scan.
- Stable record IDs with generation checks against stale slot/page reuse.
- Checked POSIX I/O, explicit sync/close, and exclusive file ownership.
- A bounded CLOCK page cache, RAII pin guards, shared/exclusive page latches,
  dirty tracking, cache statistics, and a WAL-before-page-write callback contract.
- A persistent unique integer-key B+ tree with leaf/internal pages, splits,
  deletion with redistribution/merging, root changes, and ordered range scans.
- A single-table transaction API coordinating heap and index, strict two-phase
  key/table locking, multi-operation physical rollback, and lock timeout aborts.
- Checksummed full-page WAL, forced commit, page-LSN flush ordering, and restart-safe
  recovery of committed state after real process crashes and torn data writes.
- A typed integer-key/name/integer-balance codec and a persistent storage demo.
- An interactive/batch command shell with explicit transactions, autocommit,
  quoted names, ordered queries, diagnostics and checked graceful shutdown.
- Reproducible Release benchmarks for raw CRUD/index access, durable transactions,
  concurrent clients, cache hit rates and recovery versus retained log size.
- GoogleTest coverage of boundaries, random CRUD, concurrency, reopen, subprocess
  exit, corruption detection, real write failure, pin exhaustion, eviction and
  WAL force ordering/failure, B+ tree invariants against `std::map`, and separate
  exec'd-process crash/recovery tests.

## Build and test

Dependencies: Linux, GCC 11+ or another C++20 compiler, CMake 3.20+, Make/Ninja,
and GoogleTest. On Debian/Ubuntu these can be installed with:

```bash
sudo apt-get install build-essential cmake libgtest-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

The engine itself depends only on the C++ standard library and Linux/POSIX APIs.
GoogleTest is used only for tests. For an existing GoogleTest source checkout,
pass `-DTINYDB_GTEST_SOURCE_DIR=/absolute/path/to/googletest`. Alternatively,
`-DTINYDB_FETCH_GTEST=ON` explicitly enables fetching a pinned upstream commit.
There are no network downloads during a default configuration. To build the
library and demo without tests, pass `-DBUILD_TESTING=OFF`.

`scripts/verify.sh` runs configure, build and CTest. Extra arguments are passed
to CMake. Sanitizer verification:

```bash
TINYDB_BUILD_DIR="$PWD/build-sanitize" scripts/verify.sh -DTINYDB_SANITIZERS=ON
```

In a process-traced sandbox, LeakSanitizer cannot run. For that environment only,
prefix the verification command with `ASAN_OPTIONS=detect_leaks=0`; address and
undefined-behavior checks still run. This does not verify absence of leaks.

This workspace initially had neither CMake nor GoogleTest installed. For the
local verification they were downloaded as Ubuntu packages and extracted into
`/tmp/custom-db-tools/root` without modifying system packages. To rerun using
those tools while they remain available:

```bash
export PATH="/tmp/custom-db-tools/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/custom-db-tools/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
scripts/verify.sh -DTINYDB_GTEST_SOURCE_DIR=/tmp/custom-db-tools/root/usr/src/googletest
```

## Use the CLI

```bash
./build/tinydb /tmp/tinydb-accounts
# Or use a fresh directory for the scripted transfer/rollback demonstration:
./build/tinydb /tmp/tinydb-example --batch < examples/accounts.tdb
```

```text
CREATE TABLE accounts
BEGIN
INSERT accounts 1 "Alice" 1000
INSERT accounts 2 "Bob" 500
COMMIT
SELECT accounts *
EXIT
```

Commands outside BEGIN autocommit. EOF/EXIT/SIGINT/SIGTERM roll back unfinished
transactions and close; storage failures stop the session for recovery. CREATE
acknowledges the fixed table; dynamic schemas and multiple tables remain deferred.
See the [CLI grammar and error semantics](docs/cli.md).

## Try persistence through C++

For the coordinated transaction API, run:

```bash
./build/transaction_demo /tmp/tinydb-accounts
./build/transaction_demo /tmp/tinydb-accounts
```

The demo commits both account updates, rolls back two temporary updates, then
prints Alice with 800 and Bob with 700. Both runs preserve those balances. The
API is in [transaction/database.h](include/transaction/database.h); the full
[transaction guide](docs/transactions.md) explains isolation and failure behavior.
Commit forces the WAL before success; dirty data pages may still be in memory.
On reopen, recovery reconstructs committed state before normal storage validation.
It keeps an immutable baseline and the entire WAL, so startup cost and log size
grow with history. Read the [recovery protocol and limits](docs/recovery.md).
Keep the whole database directory: `database.wal`, `recovery.meta`, both baseline
files and the materialized data files. Do not remove the log to reclaim space.

Raw storage examples remain available:

```bash
./build/storage_demo /tmp/tinydb-demo.db
./build/storage_demo /tmp/tinydb-demo.db
```

The first invocation seeds an empty heap, and both print the same records and
record IDs. The demo does not add another copy when reopened:

```text
1 | Alice | 1000 [RID 1:0:1]
2 | Bob | 500 [RID 1:1:2]
```

The index demo connects a separate persisted index to actual heap record IDs:

```bash
./build/index_demo /tmp/accounts.db /tmp/accounts.idx
./build/index_demo /tmp/accounts.db /tmp/accounts.idx
```

Both runs print Alice, Bob and Carol in key order. The demo uses two independent
files; their updates are not yet transactional. See the [B+ tree guide](docs/b-plus-tree.md)
for index insertion, lookup, deletion, range scans, format and limitations.

A minimal API example:

```cpp
#include "storage/heap_file.h"
#include "storage/record.h"

tinydb::HeapFile heap("accounts.db", 64); // Number of buffer frames (default 64).
auto rid = heap.insert(tinydb::serialize({1, "Alice", 1000}));
heap.update(rid, tinydb::serialize({1, "Alice", 800}));
auto row = tinydb::deserialize(heap.get(rid));
heap.erase(rid);
heap.close(); // Flushes and reports I/O errors.
```

Records must contain 1–4052 bytes. An update that cannot fit on its current page
returns `ErrorCode::PageFull` and preserves the old value. Reads/deletes/updates
of stale IDs return `NotFound`. Duplicate typed keys are currently permitted.
`scan()` copies the entire heap into memory. Individual heap calls serialize
under a mutex; a sequence of calls is not a transaction. Heap page reads/writes
now use the bounded cache. `buffer_stats()` exposes its hit/miss, eviction and
dirty-write counts. Modified pages can remain only in memory until eviction or
explicit flush. Handle disk write/sync errors by stopping use of that handle.
`sync()`/`close()` flush dirty frames and synchronize the file; they are the checked
durability boundary. Destructors discard unflushed cache contents, so successful
CRUD calls alone do not promise persistence. These raw APIs remain outside WAL
and can leave unusable files after interrupted writes; use `TransactionalDatabase`
for coordinated logged transactions and recovery.

## Run benchmarks

The comprehensive benchmark/correctness suite writes **`benchmarks/results.csv`**
and **[`benchmarks/report.md`](benchmarks/report.md)**:

```bash
python3 scripts/validate_benchmarks.py
# Optional local GoogleTest sources:
python3 scripts/validate_benchmarks.py \
  --gtest-source /tmp/custom-db-tools/root/usr/src/googletest
```

It builds Release, runs the complete correctness suite, then attempts 1K/10K/100K/1M
record datasets with at least 10,000 identical-key lookups per method, workers
1/2/4/8/16/32/64/100, a buffer-capacity sweep, and 16 distinct exec'd-process crash
points. It retains individual lookup latencies, commands, JUnit, compiler/hardware
metadata and worker logs under `benchmarks/evidence`. Completed comparisons report
measured medians, throughput and speedup; timed-out setup/scan runs are explicitly
incomplete and receive no projected performance values. Defaults limit setup to
90 seconds per dataset and lookup processes to 240 seconds; pass
`--setup-timeout 0 --lookup-timeout 0` to disable those limits. Exit status is 0
for a complete pass, 1 for failed validation/transactions, or 2 for incomplete or
unsupported work. See [the suite methodology](docs/benchmark-validation.md).

The original small throughput/recovery-history harness remains available:

```bash
scripts/benchmark.sh /tmp/tinydb-results \
  --rows 400 --lookups 200 --transactions 30 --recovery-transactions 200 \
  --clients 4 --frames 4 --seed 20261019
```

The script builds Release, runs three fresh-process repetitions, records hardware
and configuration, and writes CSV plus median/range summaries. It requires a new
output directory and Python 3; it never removes existing data. Read the
[benchmark methodology](docs/benchmarks.md) and
[measured results with raw CSV](docs/benchmark-results/2026-10-19/report.md).
Raw lookup throughput and durable transaction throughput have different guarantees.

## Design and remaining limits

Read [architecture and milestone gates](docs/architecture.md), the
[exact storage format](docs/storage-format.md), the [buffer pool contract](docs/buffer-pool.md),
the [persistent B+ tree](docs/b-plus-tree.md), the
[transaction protocol](docs/transactions.md), and [WAL/recovery](docs/recovery.md).
The initial write-transaction gate is deliberate: full-page undo must not
clobber another writer's updates. Coarse range locks prevent phantoms;
record locks alone will not justify a serializability claim.

The six planned milestones are implemented and verified; dynamic tables/catalogs,
SQL, MVCC, parallel writers and production hardening remain outside this scope.
Recovery rebuilds from an immutable baseline plus all committed log history;
incremental checkpoints, compensation logging and log truncation are deferred.
`TransactionalDatabase` maintains heap/index consistency with WAL. Raw heap/index
operations remain the caller's responsibility.
Benchmarks report measured small-workload results with their cache/durability
limits. See [verification notes](docs/verification.md) for actual test results.
