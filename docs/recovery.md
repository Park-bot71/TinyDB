# WAL and recovery

Commit forces a persistent COMMIT record before acknowledging success. Data
pages may still be dirty in memory; restart recovery restores committed state
before normal heap/index validation.

## Required files

| Files | Role |
| --- | --- |
| `records.db`, `primary.idx` | Materialized data; recovery can rebuild these |
| `baseline.heap`, `baseline.index` | Immutable starting images |
| `database.wal` | Checksummed transaction and full-page records |
| `recovery.meta` | Manifest tying the log to its baselines |

Keep the whole directory. Do not delete or rotate the log or baselines; there
is no checkpoint/log-recycling API. Back up the directory while closed.
Only one coordinator may own it at a time.

## Write ordering

1. BEGIN starts a logged transaction. UPDATE records contain full before/after
   page images, transaction chains and stable file/page IDs.
2. Cache changes are logged before becoming dirty. Eviction forces WAL through
   the page LSN before writing. Direct allocator/metadata changes also force WAL first.
3. COMMIT is appended and forced with `fsync` before success is returned.
4. Live rollback forces ABORT before restoring physical snapshots and discarding
   cached changes. A crash during rollback is handled on the next open.

## Restart

Recovery validates the manifest, baselines and log; trims only a provably
incomplete final record; then rebuilds temporary heap/index files from the
baselines plus **committed after-images in log order**. Aborted and incomplete
transactions are excluded. Generation high water includes valid loser metadata.

It synchronizes and publishes the rebuilt files, then validates allocator,
heap and index invariants. If recovery crashes, the next attempt starts again
from the unchanged baseline and WAL. This is full-history reconstruction,
not incremental ARIES/CLR undo. Log size, startup I/O and memory grow with history.

## Failure limits

A complete corrupt WAL record is fatal, including the final record. Missing or
corrupt WAL, manifest or baselines cannot be repaired from data files alone.
An error after COMMIT was written may leave an indeterminate outcome; stop using
the owner, reopen and inspect state before retrying.

The protocol assumes exclusive ownership and a local Linux filesystem/device
honoring `fsync`. Tests cover process exits and partial writes, not physical
power cuts or arbitrary media loss. Raw storage APIs have no automatic recovery.

The WAL uses a 64-byte header and 80-byte record headers with CRCs and optional
4096-byte images. See [wal.cpp](../src/recovery/wal.cpp) for exact encoding and
[verification](verification.md) for crash-test evidence. Fault hooks are test-only
and must not reenter storage or transaction APIs.
