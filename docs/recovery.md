# WAL and recovery design (planned)

Phase 2 adds cached heap pages and explicit flush/sync/close over checked disk
I/O. Nonzero page LSNs require a WAL-force callback before dirty page writes,
but no WAL exists yet, heap LSNs are zero, and allocator writes remain direct.
The engine is not crash atomic. An interrupted update can leave corrupt data or
inconsistent allocation metadata. Reopening rejects detected inconsistencies;
this is detection, not recovery. Keep expendable databases until phase 5.

## Joint transaction/buffer/index contract

Use a steal/no-force buffer pool. Every logged page mutation carries a page LSN.
Before a frame is written, force the WAL through that LSN. Before acknowledging
COMMIT, append and force COMMIT with `fdatasync`/`fsync`. A dirty page may still
be absent from the data file after a successful commit. Clean shutdown forces
WAL, flushes pages, then synchronizes the data file.

Start with physical full-page before/after images. Each log frame has a version,
length, checksum, monotonic LSN, transaction ID, previous transaction LSN, type,
page ID and optional images. Types include BEGIN, UPDATE, COMMIT, ABORT,
compensation (CLR), and END. An ABORT means undo has begun, not that it finished.
Allocation, free-list metadata, RID generation reservation, catalog changes and
B+ tree split/merge/root changes are logged as well. Newly extended pages have
an explicit allocation image. A writer gate held until transaction completion
makes full-page physical undo safe against other writers (see transactions.md).

## Restart protocol

1. Scan complete validated frames. Ignore/truncate only a provably incomplete
   final frame; fail on interior corruption. Rebuild transaction chains and the
   last valid LSN. Keep the full log initially; no truncation/checkpoint claims.
2. Redo UPDATE and CLR after images in LSN order, including loser updates, using
   page LSNs to skip already-applied writes. Full images permit restoring torn
   pages. Recovery must open raw storage before normal superblock/free-chain
   validation: a damaged superblock cannot prevent WAL repair. Allocation redo
   must restore file length and metadata before normal storage access resumes.
3. Walk incomplete transaction chains backwards. Before restoring a before
   image, append a CLR with redo image and undo-next LSN; force it before flushing
   the restored page. CLRs are redone and never undone. On restart, undo resumes
   at undo-next. Finish with END. Recovery must reach a fixed point across
   repeated crashes, with no duplicate undo or accidental resurrection.
4. Synchronize recovered pages and validate allocator, heap and index invariants
   before accepting clients. New transaction and generation IDs must exceed all
   durable IDs, including aborted transactions.

## Durability and test gates

Assume local Linux filesystems honoring successful `fsync`/`fdatasync`, exclusive
process ownership, and storage that honors flushes. Persist a new file's parent
directory entry too. Page checksum detection alone does not provide torn-write
repair. Never acknowledge a commit on a failed log flush. Log I/O failure puts
the engine into a failed state; outcome may be indeterminate to the client.

Use fork/exec child processes terminated by `_exit`/SIGKILL at failpoints:
before commit; between two account updates; after durable commit before page
flush; halfway through a physical page write; after CLR force; during redo;
and between undo actions. Reopen repeatedly and assert atomic balances, index
invariants, allocator integrity and recovery idempotence. Exceptions alone are
not a crash test. These tests belong to milestone 5, not the current milestone.
