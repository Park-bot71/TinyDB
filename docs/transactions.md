# Transactions and concurrency (Phases 4–5)

`TransactionalDatabase` coordinates the existing heap and persistent B+ tree as
one fixed-schema table. `begin()` returns a movable transaction with explicit
`commit()` and `rollback()`. Inserts, reads, updates, deletes, and range scans use
real disk-backed storage. Strict two-phase locks provide serializable isolation;
volatile physical undo handles live rollback. Phase 5 adds durable WAL decisions
and restart-safe reconstruction for process crashes, including interrupted page
writes. See [recovery.md](recovery.md) for the protocol and its filesystem,
retained-history and failure limits; this is not an unrestricted ACID claim.

## API and files

```cpp
#include "transaction/database.h"

tinydb::TransactionalDatabase db("accounts", {
    .buffer_frames = 8, .index_max_keys = 0,
    .lock_timeout = std::chrono::milliseconds(1000)});
{
  auto tx = db.begin();
  tx.insert({1, "Alice", 1000});
  tx.insert({2, "Bob", 500});
  tx.commit();
}
{
  auto tx = db.begin();
  auto alice = tx.get_for_update(1).value();
  auto bob = tx.get_for_update(2).value();
  alice.balance -= 200;
  bob.balance += 200;
  tx.update(1, alice);
  tx.update(2, bob);
  tx.commit(); // The COMMIT record is durable before locks release.
}
{
  auto tx = db.begin();
  tx.update(1, {1, "temporary", 0});
  tx.rollback(); // Restores the preceding committed record and index state.
}
auto read = db.begin();
auto rows = read.scan(0, 100); // Sorted half-open [0,100).
read.commit();
db.close();
```

A database directory contains `records.db`, `primary.idx`, `database.wal`,
`recovery.meta`, `baseline.heap` and `baseline.index`. Keep the whole directory.
Startup reconstructs the data pair from the baseline and committed WAL, even if
data files are missing or torn. Initial adoption of an existing unlogged pair
requires a consistent heap/index relationship. Startup verifies counts and that
each heap record's key maps to its exact RID. Dynamic tables, DDL, a catalog,
SQL parsing and dynamic schemas are deferred; the CLI exposes the fixed table as
`accounts` (see cli.md). The existing raw
`HeapFile` and `BPlusTree` APIs still perform independent operations; they do not
implicitly join a transaction. The coordinator owns both files and does not
expose direct access that could bypass its locks or undo protocol. Do not edit
or replace the files externally while it is open.

`get(key)` returns an optional typed record. Missing reads are successful and
still take a key lock. `get_for_update(key)` returns the same optional while
reserving exclusive write access, which is useful for read/modify/write loops.
`insert` rejects duplicates with `DuplicateKey`; `update` and `erase` reject
absent keys with `NotFound`. Updating a primary key is disallowed. Record-size
and same-page growth limits remain those of the heap; `PageFull` aborts the
whole transaction, including earlier operations. Scan bounds/limits have the
same meaning as the B+ tree API. Even an empty or zero-limit scan locks the table.

The default pool capacity is 64 frames **per file**; one-frame pools are tested.
Index capacity zero uses the persisted capacity, or 184 for a new index. Explicit
capacities must match on reopen. Lock timeout defaults to 1000 milliseconds and
must be positive.

## Lifecycle and ownership

Transactions have a process-wide monotonically allocated ID and an atomic state:
ACTIVE, COMMITTING, COMMITTED, ABORTING, ABORTED, or FAILED. ID zero is invalid.
Recovery advances the ID allocator above every ID in the accepted WAL prefix.
IDs appearing only in an incomplete discarded final frame may be reused. The
lock manager tracks active registrations and grants.

A normal operation error automatically transitions the transaction through
ABORTING, restores prior work, releases its locks, and leaves it ABORTED before
propagating the original error. This includes constraint errors and timeouts.
Calling data operations or commit on a completed transaction raises
`InvalidTransactionState`. Repeating a successful rollback is harmless; rolling
back a committed transaction is an error. There are no savepoints or nested
transactions.

Destroying an unfinished handle rolls it back. Moving a handle transfers all
ownership; move assignment first rolls back unfinished work in the destination.
A moved-from handle rejects operations. A live handle retains shared ownership
of database internals, so it can finish after the outer database wrapper is
destroyed. Methods on one transaction serialize under its operation mutex; use
separate transactions for concurrent clients. ID/state observation is safe while
an operation is waiting. Normal C++ object-lifetime rules still apply.

`close()`, `sync()` and diagnostic `validate()` reject active transactions with `Busy`.
Successful database close is idempotent and future begin calls fail with `Closed`.
Committed/aborted handles can remain alive after close; they no longer hold locks
or count as active. Unfinished handles must be resolved before reopening the same
files through another owner.

## Strict two-phase locking and isolation

All transaction logical locks are held until commit finishes or rollback
completes. There is no public per-resource unlock. Point reads use table IS and
key S locks. Writes, including insert/missing-key probes, use table IX and key X
locks. Table/key identities are logical resources independent of heap RIDs or
index page locations, so compaction or B+ tree splits cannot change their meaning.
The initial implementation has one table, resource ID zero.

Range scans acquire table S. It conflicts with every other transaction's IX
writer lock, conservatively protecting the whole table against phantoms even
when the requested interval is empty. A transaction combining a range read and
writes upgrades its own table lock to SIX. There are no lock downgrades.

| Held / Requested | IS | IX | S | SIX | X |
| --- | --- | --- | --- | --- | --- |
| IS | yes | yes | yes | yes | no |
| IX | yes | yes | no | no | no |
| S | yes | no | yes | no | no |
| SIX | yes | no | no | no | no |
| X | no | no | no | no | no |

Compatibility excludes the transaction's own grants, so an upgrade does not
conflict with itself. Multiple transactions upgrading S to X retain their S
locks while waiting and can deadlock; the timeout policy resolves that case.
An already-held stronger lock satisfies a weaker request without surrendering it.
Pending requests are FIFO per resource; a new reader does not bypass a queued
writer. This prevents queue bypass, not starvation under every workload.

Under this API, strict key locks prevent dirty/nonrepeatable reads and lost
updates, and table S scan locks prevent phantoms. Successful live-process
transactions therefore provide serializable isolation within the one table.
This claim does not apply to raw storage API calls or external file edits.
Failed I/O makes the coordinator unavailable until recovery; the WAL protocol
defines which transactions survive a crash separately from live locking.

## Writer gate and timeouts

The first write intent acquires a database-wide writer resource in X mode and
retains it through transaction completion. This spans both heap and index files.
It serializes write transactions so physical page undo cannot overwrite a second
writer's changes to a shared page. Readers on unrelated keys can proceed, while
range readers conservatively exclude writers. `get_for_update` takes the writer
gate even if the caller eventually makes no change.

A write acquires writer gate, table IX, and key X **before** acquiring the short
database storage latch or any heap/index/page latch. Reads acquire their logical
locks first as well. Each acquisition has its own steady-clock deadline. If the
condition is not satisfied before it, `LockTimeout` removes the pending request;
existing grants remain until the coordinator undoes and completes the victim.
The coordinator always aborts the victim, rather than allowing a timed-out
operation to continue.

This is a documented timeout policy, not wait-for-graph deadlock detection.
Total transaction duration and storage I/O time are not bounded by the lock
wait timeout. Holding prior read locks while waiting for the writer gate can
form cycles with a writer waiting for those keys. Tests exercise that cycle
with prior real updates and verify undo occurs before locks are released.
A client may retry an entire aborted transaction after `LockTimeout`; retries
should be bounded and use backoff. `get_for_update` avoids an S-to-X upgrade
when an operation is known to be a write in advance.

## Live physical undo

A writer first flushes/synchronizes both file caches to establish a committed
on-disk baseline. It then arms one volatile undo journal per disk manager. Before
`pwrite` can overwrite an existing baseline page, the journal captures its first
validated before-image. It includes superblock/free-list changes, slot and
record generation reservations, index headers, leaf/internal pages, parent
updates, splits/merges and page reuse. New pages beyond baseline file length
need no old image; rollback truncates the extension.

Dirty pages may remain cached, or CLOCK eviction may write them before commit.
Both cases are handled: rollback first forces its WAL ABORT decision, then takes
the database storage latch exclusively,
restores captured disk pages, truncates extensions, reloads allocator/type/free
metadata, and discards all cached images. The B+ tree reloads its baseline header
and validates the restored structure. Record generation counters keep their
highest issued value so IDs from aborted inserts cannot be reused. Existing
records, RIDs and index root state are preserved.

There is only one writer and no client accesses storage during restoration, so
first-before-images restore a file baseline in any order. This is not a reverse
per-update WAL chain. The journal is a live-process optimization; durable crash
recovery uses the separate immutable baseline and WAL. Memory grows with distinct
overwritten baseline pages; dirty-only changes are undone by discarding their
cache images. Changes
on newly allocated pages disappear with truncation or restored free pages.

The short storage latch makes each heap/index pair operation coherent and
prevents readers from observing intermediate rollback state. Its lock order is:
transaction operation mutex, logical locks (all waits here), database storage
latch, heap/index latch, pool mutex, disk mutex. No logical lock wait occurs while
holding a storage or page latch. Completion drops storage latches before
releasing logical locks. The database lifecycle mutex protects begin/active-count/
close; no lock-manager condition wait runs while holding it.

## Commit, I/O errors and recovery

Every transaction logs BEGIN and a terminal COMMIT or ABORT, including read-only
transactions. Commit transitions to COMMITTING, appends and forces COMMIT, drops
the volatile journals, marks COMMITTED, then releases logical locks. It does not
force dirty data pages. Acknowledged commits survive the tested process crashes
because recovery replays their logged page images. `sync()` flushes both data
files when no transaction is active; it does not checkpoint or truncate WAL.
`wal_stats()` exposes append/force and recovery diagnostics.

Cache mutations log full before/after images before becoming dirty. Direct
allocator, generation and free-list writes force their records before disk I/O.
Dirty eviction forces through the page LSN before writing. The writer gate
ensures that a committed full-page image cannot contain another writer's
uncommitted changes.

ABORT is a durable decision before live physical restoration starts. Restoration
does not emit additional UPDATE records; if it is interrupted, reconstruction
from the immutable baseline discards that transaction. Record-generation high
water marks include valid loser log images to avoid reusing issued generations.

An I/O/corruption error makes the database unavailable even if live undo succeeds.
Resolve remaining handles and destroy the database before reopening for recovery.
If WAL or undo fails, the transaction can enter FAILED and the caller receives
`TransactionFailed`. This is an indeterminate outcome: a complete COMMIT may
already exist or have become durable even when the caller saw an exception.
The coordinator never undoes a transaction after logging COMMIT. Reopen and
inspect recovered state before deciding whether to retry business operations.
Destructors cannot report cleanup failures; explicit completion is required.

Recovery always rebuilds from the immutable baseline and the complete retained
log. It uses committed after-images to remove incomplete/aborted effects rather
than incremental undo or compensation records. Repeated recovery is safe because
neither input is changed. The protocol retains all history and has startup and
memory costs proportional to that history; checkpointing and log recycling are
deferred. See [recovery.md](recovery.md) for exact formats, ordering and crash tests.
