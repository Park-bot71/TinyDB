# Architecture and milestones

## Scope and storage model

Build incrementally in C++20 on Linux. All six phases implement a persistent heap,
bounded buffer pool, B+ tree index, transactional coordinator, WAL/recovery,
interactive CLI and reproducible evaluation.
The coordinator provides strict locking, live rollback and serializable isolation
for its single-table API, including phantom protection. Durable commit records
and immutable-baseline/full-history reconstruction recover committed state after
tested process crashes. No unrestricted ACID/media-failure claim applies; see
recovery.md for assumptions and limits. Each phase must pass its tests before
the next begins.

One database file contains a versioned superblock (page 0), 4096-byte heap pages,
and a persistent free-page chain. Records are opaque nonempty byte strings;
a separate codec represents an integer key, name, and signed integer balance.
A record ID contains page ID, slot ID, and a 64-bit generation. Slots survive
compaction, and generations are unique within a database file, preventing a
stale ID from addressing a new record after slot or whole-page reuse. IDs do not
identify a database file; dereference each ID only in its originating heap.

Raw heap records have no key uniqueness constraint. The current B+ tree rejects
duplicate integer keys and maps keys to stable heap record IDs in a separate
index file. `TransactionalDatabase` binds one fixed-schema heap/index pair under
live transactions. A dynamic table catalog is deferred. Oversized records are
rejected; overflow chains, blobs, SQL, MVCC, online DDL, compression, replication,
and advanced checkpointing are deferred. The heap scans pages to find space;
a free-space directory is a later optimization.

## Module boundaries and repository

- `include/common`: errors and explicit little-endian binary encoding.
- `include/storage`, `src/storage`: page codec, POSIX disk manager, slotted
  pages, heap CRUD, typed record codec.
- `include/buffer`, `src/buffer`: bounded CLOCK cache and shared/exclusive guards.
- `include/index`, `src/index`: index-page codecs, persistent B+ tree and validation.
- `include/concurrency`, `src/concurrency`: strict multigranularity lock manager.
- `include/transaction`, `src/transaction`: transaction lifecycle and database coordinator.
- `include/recovery`, `src/recovery`: full-page WAL and baseline reconstruction.
- `include/cli`, `src/cli`: complete-line parser, shell session and executable.
- `benchmarks`: checked C++ workloads with CSV timing/cache/recovery counters.
- `tests`: GoogleTest unit/integration tests, real file and process tests.
- `examples`: runnable storage/index/transaction demonstrations and CLI script.
- `docs`: format, transaction design, recovery design, milestone gates.
- `scripts`: reproducible local verification.

All listed modules contain working code. The CLI uses the existing fixed table;
dynamic table/catalog support remains deferred.

```text
Custom_DB/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── storage-format.md
│   ├── buffer-pool.md
│   ├── b-plus-tree.md
│   ├── transactions.md
│   ├── recovery.md
│   ├── cli.md
│   ├── benchmarks.md
│   ├── benchmark-results/
│   └── verification.md
├── include/
│   ├── common/
│   ├── cli/
│   ├── buffer/
│   ├── index/
│   ├── concurrency/
│   ├── transaction/
│   ├── recovery/
│   └── storage/
├── src/
│   ├── buffer/
│   ├── cli/
│   ├── index/
│   ├── concurrency/
│   ├── transaction/
│   ├── recovery/
│   └── storage/
├── tests/
│   ├── storage_tests.cpp
│   ├── buffer_tests.cpp
│   ├── index_tests.cpp
│   ├── transaction_tests.cpp
│   ├── recovery_tests.cpp
│   ├── recovery_worker.cpp
│   ├── cli_tests.cpp
│   └── stress_tests.cpp
├── examples/
│   ├── storage_demo.cpp
│   ├── index_demo.cpp
│   ├── transaction_demo.cpp
│   └── accounts.tdb
├── benchmarks/benchmark.cpp
└── scripts/
    ├── verify.sh
    ├── benchmark.sh
    └── summarize_benchmarks.py
```

## Cross-cutting correctness

The highest risks are stale record IDs, exact capacity accounting, failed or
partial writes, transaction/structure atomicity, latch/lock deadlocks, WAL flush
ordering, and interrupted recovery. Serialization never dumps C++ structs.
Checksums detect accidental page damage but cannot repair it. All offsets,
lengths, IDs, and format versions are validated before use. A failed mutation
at the I/O layer poisons the open disk manager; callers must stop and reopen.
The coordinator repairs interrupted multi-page writes from WAL and its immutable
baseline; raw standalone storage still has no automatic recovery.

The heap serializes operations with a mutex. The disk manager also has a
mutex and takes an exclusive advisory `flock` for the lifetime of its file
descriptor; multiple owners of one file are rejected. Calls acquire the heap
mutex or index latch before pool and disk synchronization. Page references must
not outlive their guards. Low-level
page values require external synchronization if shared.

Heap I/O now uses pinned page guards. Guards own page latches and unpin via RAII.
The pool protects its frame map and replacement state; page latches protect page
bytes. Eviction cannot choose pinned frames. The pool never waits on a page latch
while holding its map mutex. Disk I/O is synchronous under that mutex; flushes
reject pinned frames rather than waiting. See buffer-pool.md for full rules.
Never wait for a transaction lock while holding a page latch or buffer-map
mutex. The WAL force path must never acquire page latches.

## Independently testable milestones

1. **Storage foundation (verified):** checked binary format, page allocation and
   reuse, stable slotted records, CRUD, codec, restart tests, invalid inputs,
   full pages, file errors. Gate: CMake build and all GoogleTests pass.
2. **Buffer pool (verified):** fixed frames, pin/unpin guards, CLOCK, dirty flags,
   page latches, flush callback carrying page LSN. Test eviction, pinned-pool
   exhaustion, concurrent access, and an instrumented WAL-order assertion.
3. **Persistent B+ tree (verified):** internal/leaf pages, unique integer keys, ordered
   leaves, root metadata, splits, redistribution, merges. Initially serialized
   structural operations. Random operations are compared against `std::map`, checking
   occupancy/order/parent/leaf-chain invariants after every operation, reopen.
   Crash consistency is not claimed until milestone 5.
4. **Transactions and locking (verified):** strict locks and timeout
   abort, lifecycle, single-writer gate, volatile page undo across heap/index,
   and multi-operation rollback. Tests cover reader blocking, lost updates,
   phantoms, deadlock timeout and lock cleanup.
5. **WAL and recovery (verified):** checksummed log frames, full
   before/after page images, commit force, page-LSN ordering, and structural/
   allocator logging. Recovery reconstructs from an immutable baseline and
   committed log history, discarding losers without incremental CLR undo. Tests
   exec separate processes and crash during updates, append/flush/abort and
   recovery. Full-history replay is restart-safe; CLR/checkpoint/log truncation
   are deferred rather than claimed implemented.
6. **CLI and evaluation (current, verified):** fixed-table custom command grammar, graceful
   shutdown, concurrency stress, subprocess crash matrix, reproducible benchmarks
   for insert/lookup/range/transactions/clients/cache hits/recovery-log size.
   Record hardware, compiler, configurations and measured results. Do not invent
   benchmark claims or compare to a nonexistent index.

The shell parses a full line before opening/mutating a transaction. It retains
one explicit handle or uses per-command autocommit. Signal handlers set a flag
and write a nonblocking wakeup pipe; input polls it to avoid blocking after an
already delivered signal. Rollback/close run in ordinary control flow. See cli.md
for error/exit semantics.
Stress tests use concurrent C++ clients under one coordinator. Separate CLI
processes cannot share a directory because ownership is exclusive.

Benchmark code validates results, uses fresh owned directories, separates raw
heap/index measurements from forced-COMMIT transactions, and measures full
constructor recovery at controlled retained-history sizes. The wrapper records
build/hardware settings, independent process samples and raw CSV. See benchmarks.md
for timing boundaries, warm-cache assumptions and interpretation limits.

If scope becomes excessive, keep coarse table locks and serialized B+ tree
structural changes, retain the whole WAL without checkpoint truncation, and
ship a single-table CLI first. Do not defer durable commit, rollback correctness,
or real crash tests while calling the result transactional.
