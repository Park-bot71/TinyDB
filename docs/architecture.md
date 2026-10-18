# Architecture and milestones

## Scope and storage model

Build incrementally in C++20 on Linux. Phases 1 and 2 implement a persistent,
unindexed heap and bounded buffer pool. No ACID, transaction isolation, or
crash-recovery claim applies to this milestone. Each phase must pass its own
tests before the next begins. Later transaction/index/recovery designs below
are contracts to implement and verify, not implemented features.

One database file contains a versioned superblock (page 0), 4096-byte heap pages,
and a persistent free-page chain. Records are opaque nonempty byte strings;
a separate codec represents an integer key, name, and signed integer balance.
A record ID contains page ID, slot ID, and a 64-bit generation. Slots survive
compaction, and generations are unique within a database file, preventing a
stale ID from addressing a new record after slot or whole-page reuse. IDs do not
identify a database file; passing an ID between files is unsupported.

Heap records initially have no key uniqueness constraint. The future table
catalog owns separate heaps and B+ tree roots. The index will reject duplicate
integer keys and map keys to stable heap record IDs. Oversized records are
rejected; overflow chains, blobs, SQL, MVCC, online DDL, compression, replication,
and advanced checkpointing are deferred. The heap scans pages to find space;
a free-space directory is a later optimization.

## Module boundaries and repository

- `include/common`: errors and explicit little-endian binary encoding.
- `include/storage`, `src/storage`: page codec, POSIX disk manager, slotted
  pages, heap CRUD, typed record codec.
- `include/buffer`, `src/buffer`: bounded CLOCK cache and shared/exclusive guards.
- `tests`: GoogleTest unit/integration tests, real file and process tests.
- `examples`: runnable persistent storage demonstration.
- `docs`: format, transaction design, recovery design, milestone gates.
- `scripts`: reproducible local verification.

Later milestones add `index`, `transaction`, `concurrency`, `recovery`,
`src/cli`, and `benchmarks` when they contain working code. Empty modules and
placeholder implementations are intentionally omitted.

```text
Custom_DB/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── storage-format.md
│   ├── buffer-pool.md
│   ├── transactions.md
│   ├── recovery.md
│   └── verification.md
├── include/
│   ├── common/
│   ├── buffer/
│   └── storage/
├── src/
│   ├── buffer/
│   └── storage/
├── tests/
│   ├── storage_tests.cpp
│   └── buffer_tests.cpp
├── examples/storage_demo.cpp
└── scripts/verify.sh
```

## Cross-cutting correctness

The highest risks are stale record IDs, exact capacity accounting, failed or
partial writes, transaction/structure atomicity, latch/lock deadlocks, WAL flush
ordering, and interrupted recovery. Serialization never dumps C++ structs.
Checksums detect accidental page damage but cannot repair it. All offsets,
lengths, IDs, and format versions are validated before use. A failed mutation
at the I/O layer poisons the open disk manager; callers must stop and reopen.
The current engine cannot repair interrupted multi-page writes.

The heap serializes operations with a mutex. The disk manager also has a
mutex and takes an exclusive advisory `flock` for the lifetime of its file
descriptor; multiple owners of one file are rejected. Calls use heap, pool, then
disk synchronization. Page references must not outlive their guards. Low-level
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
2. **Buffer pool (current, verified):** fixed frames, pin/unpin guards, CLOCK, dirty flags,
   page latches, flush callback carrying page LSN. Test eviction, pinned-pool
   exhaustion, concurrent access, and an instrumented WAL-order assertion.
3. **Persistent B+ tree:** internal/leaf pages, unique integer keys, ordered
   leaves, root metadata, splits, redistribution, merges. Initially serialized
   structural operations. Compare random operations against `std::map`, check
   occupancy/order/parent/leaf-chain invariants after every operation, reopen.
   Crash consistency is not claimed until milestone 5.
4. **Transactions and locking:** implement the transaction/WAL interfaces from
   the jointly designed protocol, strict locks and timeout abort, lifecycle,
   multi-operation rollback. Test reader blocking, lost updates, phantoms,
   deadlock timeout, lock cleanup. Durability remains gated on milestone 5.
5. **WAL and recovery:** checksummed log frames, before/after page images,
   commit force, page-LSN ordering, compensation logging, restart-safe recovery,
   structural and allocator logging. Use subprocess crashes at named failpoints,
   including mid-flush and recovery. No transactions are advertised as durable
   until this gate passes.
6. **CLI and evaluation:** table catalog and custom command grammar, graceful
   shutdown, concurrency stress, subprocess crash matrix, reproducible benchmarks
   for insert/lookup/range/transactions/clients/cache hits/recovery-log size.
   Record hardware, compiler, configurations and measured results. Do not invent
   benchmark claims or compare to a nonexistent index.

If scope becomes excessive, keep coarse table locks and serialized B+ tree
structural changes, retain the whole WAL without checkpoint truncation, and
ship a single-table CLI first. Do not defer durable commit, rollback correctness,
or real crash tests while calling the result transactional.
