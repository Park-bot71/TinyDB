# Architecture

TinyDB has one fixed table, `accounts`. The transaction layer keeps its heap
records and B+ tree index consistent.

| Directory | Responsibility |
| --- | --- |
| `src/storage` | Pages, records and file I/O |
| `src/buffer` | Page cache and CLOCK replacement |
| `src/index` | B+ tree lookups and range scans |
| `src/concurrency` | Locks and timeouts |
| `src/transaction` | Commit and rollback |
| `src/recovery` | WAL and restart recovery |
| `src/cli` | Interactive and batch commands |
| `tests`, `benchmarks` | Correctness checks and measurements |

One writer runs at a time; readers on unrelated keys can proceed. Range reads
lock the table. Commit saves the WAL to disk; recovery rebuilds committed state
from a baseline and the retained log. One database owner manages the directory.

See [transactions](transactions.md), [recovery](recovery.md) and
[storage](storage-format.md). SQL, multiple tables, parallel writers, MVCC and
checkpointing are not implemented.
