# TinyDB — disk-backed database engine in C++20

An incremental implementation of database internals on Linux. **Phases 1 and 2
(storage foundation and buffer pool) are implemented.** B+ trees, transactions,
WAL, recovery, the interactive CLI and benchmarks are planned milestones, not
available features. This milestone makes no ACID or crash-recovery claim.

The engine currently provides:

- Persistent 4096-byte pages with explicit little-endian encoding and CRC-32.
- Page allocation and a free-page chain that survives reopen.
- Slotted heap records with insert, read, variable-length update, delete and scan.
- Stable record IDs with generation checks against stale slot/page reuse.
- Checked POSIX I/O, explicit sync/close, and exclusive file ownership.
- A bounded CLOCK page cache, RAII pin guards, shared/exclusive page latches,
  dirty tracking, cache statistics, and a WAL-before-page-write callback contract.
- A typed integer-key/name/integer-balance codec and a persistent storage demo.
- GoogleTest coverage of boundaries, random CRUD, concurrency, reopen, subprocess
  exit, corruption detection, real write failure, pin exhaustion, eviction and
  WAL force ordering/failure.

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

## Try persistence

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
CRUD calls alone do not promise persistence. An interrupted multi-page operation
may leave the database unusable until WAL/recovery is implemented.

## Design and next steps

Read [architecture and milestone gates](docs/architecture.md), the
[exact storage format](docs/storage-format.md), the [buffer pool contract](docs/buffer-pool.md), the jointly planned
[transaction protocol](docs/transactions.md), and [WAL/recovery](docs/recovery.md).
The initial write-transaction gate is deliberate: full-page undo must not
clobber another writer's updates. Coarse range locks will prevent phantoms;
record locks alone will not justify a serializability claim.

The next milestone is the persistent B+ tree: integer-key lookup, splits,
deletion/rebalancing, ordered scans and structural invariants. Buffer pool I/O is
currently synchronous under one mutex; allocation metadata remains unlogged.
Performance claims and benchmark results are deferred until workloads and the
relevant subsystems exist. See [verification notes](docs/verification.md) for
actual test results.
