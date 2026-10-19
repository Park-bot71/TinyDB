# Recovery

Commit saves the COMMIT log record with `fsync` before returning success. Data
pages may still be cached. On restart, recovery restores committed changes and
discards unfinished or aborted transactions before validating the database.

**Keep the whole database directory:** `records.db`, `primary.idx`,
`baseline.heap`, `baseline.index`, `database.wal` and `recovery.meta`.
Only one database owner may use it. Back it up while closed.

## How it works

- The WAL records full page images before and after each change. Log records
  reach disk before changed data pages, including during eviction.
- Live rollback saves ABORT before restoring the original pages.
- Restart rebuilds data files from the unchanged baselines and committed log
  images. If recovery crashes, the next restart repeats this process safely.

## Limits

Recovery reads the full history. There are no checkpoints or log recycling;
log size, startup work and memory grow with use. Do not delete recovery files.

Only an incomplete final log record can be trimmed. A complete corrupt record,
or damaged/missing log, manifest or baselines, stops recovery. A failed commit
may have succeeded: reopen and inspect state before retrying.

Tests cover real process crashes and partial writes, not physical power cuts
or media loss. Durability assumes a local Linux filesystem honoring `fsync`.
Raw `HeapFile` and `BPlusTree` calls do not provide transaction recovery.

See [test results](verification.md) and [WAL implementation](../src/recovery/wal.cpp).
