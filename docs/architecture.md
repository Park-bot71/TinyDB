# Architecture

TinyDB has one fixed-schema table, `accounts`, backed by a heap file and a
persistent B+ tree. The transaction layer keeps them consistent.

```text
CLI / C++ client
      ↓
Transactions + strict two-phase locks
      ↓
Heap records + B+ tree index
      ↓
CLOCK buffer pools → disk pages
      ↘ WAL → restart recovery
```

## Main components

| Directory | Responsibility |
| --- | --- |
| `src/storage` | 4 KB pages, records, allocation and checked file I/O |
| `src/buffer` | Fixed cache, CLOCK replacement and page guards |
| `src/index` | Integer-key B+ tree and ordered scans |
| `src/concurrency` | Shared/exclusive locks and lock timeouts |
| `src/transaction` | Commit, rollback and heap/index coordination |
| `src/recovery` | Full-page WAL and recovery |
| `src/cli` | Interactive commands and batch input |
| `tests`, `benchmarks` | Correctness checks and measured workloads |

## Important choices

- One writer runs at a time. Readers on unrelated keys can proceed.
- Range reads lock the table to prevent phantoms.
- Each data file has a bounded cache; pages use checksums and stable record IDs.
- Commit forces the WAL. Recovery rebuilds committed state from an immutable
  baseline and the full retained log.
- One coordinator owns the directory. Raw storage calls do not join transactions.

Lock acquisition order is transaction → logical locks → storage latch →
heap/index latch → buffer pool → disk. Logical lock waits happen before storage
or page latches; WAL forcing must not acquire page latches.

For details, see [transactions](transactions.md), [recovery](recovery.md), and
[the storage format](storage-format.md). SQL, multiple tables, parallel writers,
MVCC and checkpointing are outside the current scope.
