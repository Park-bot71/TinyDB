# Transaction and concurrency design (planned)

Phase 1 implements none of BEGIN/COMMIT/ROLLBACK, transaction locks or isolation.
Its mutex protects individual heap calls only.

The planned engine uses strict two-phase locking: shared key locks for point
reads and exclusive key locks for writes, including missing-key/insert probes.
All logical locks remain held until commit is durable or rollback completes.
Transactions have monotonically allocated IDs and ACTIVE, COMMITTING,
COMMITTED, ABORTING and ABORTED states. An operation error makes a transaction
abort-only. Transactions are not used concurrently by multiple threads.

Use table intention-shared/exclusive locks for point operations, a table shared
lock for range scans, and a table exclusive lock for DDL. A range lock conflicts
with all table intention-exclusive writers. This conservatively prevents
phantoms; it is slower than key-range locking. Under these rules, the target is
serializable isolation, which must be tested before being advertised. If range
locking is omitted, advertise repeatable point reads only and document phantoms.

Use a configurable steady-clock lock timeout. Timeout marks the victim
abort-only, rolls it back and releases its locks; acquiring a lock must not
silently continue after timeout. This bounds waits, but is not graph-based
deadlock detection and does not guarantee starvation freedom. Never hold page
latches while waiting for logical locks. Acquire logical locks before the short
storage mutation latch. Commit/abort cleanup must run on exceptions.

A first correct implementation will serialize write transactions with a database
writer gate held from first write through commit/rollback. Readers still use
logical locks and concurrent clients can wait. Physical page-image undo cannot
then overwrite a different writer's changes to the same page. This deliberately
limits write throughput. Dropping the writer gate requires record-level undo or
a proven system-transaction protocol; record locks alone are insufficient.

Under that gate, each heap, B+ tree, root and allocator mutation participates in
the same transaction and WAL chain. Splitting a tree node or updating a free-list
head cannot bypass logging. Rollback restores before images in reverse LSN order
and records compensation before making each restored page eligible for flush.
Release logical locks only after durable commit, or after undo is complete.
