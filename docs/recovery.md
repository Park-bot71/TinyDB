# WAL and crash recovery (Phase 5)

`TransactionalDatabase` now logs every heap/index cache mutation and direct
allocator page write. Commit forces a persistent COMMIT record before releasing
locks or acknowledging success. Recovery runs before ordinary disk, allocator,
heap or B+ tree validation, so torn headers cannot prevent repair.

The implementation uses **immutable-baseline reconstruction with full-history
redo**. It is deliberately simpler than ARIES. Recovery reconstructs the committed
state from a validated baseline plus WAL, thereby removing incomplete/aborted
transactions' disk effects. It does not use CLRs or incremental page-LSN redo
skipping. Rebuilding from the same durable inputs makes recovery restart-safe.
This trades log space, startup I/O and memory for a small, testable protocol.

## Files, ownership and initialization

The database directory contains:

| File | Purpose |
| --- | --- |
| `records.db`, `primary.idx` | Materialized heap and B+ tree, repairable by recovery |
| `baseline.heap`, `baseline.index` | Immutable initial images, required for all future recovery |
| `database.wal` | Header plus append-only transaction/page frames |
| `recovery.meta` | Durable copy of the WAL header, binds the required baseline sizes/CRCs |
| `recovery.heap.tmp`, `recovery.index.tmp` | Disposable recovery staging files |

Keep the WAL, manifest and **both baselines**. Do not delete, truncate, rotate or
replace them to reclaim space. There is no checkpoint/log-truncation API yet.
A consistent backup copies the directory while closed/quiescent; staging files
can be omitted. Copying just the two materialized data files is not a complete
backup. WAL-stamped files without their recovery metadata are rejected rather
than silently adopted as a new unlogged database.

The WAL file has an exclusive advisory flock for the coordinator lifetime, in
addition to existing data-file locks. Recovery acquires both data-file locks
before publishing repairs. Checked `close()` releases data and WAL ownership,
even if completed transaction handles still exist. Callbacks and file descriptors
have RAII ownership; destructors cannot report flush/close failures.

On first adoption, an existing unlogged heap/index pair is opened and validated
against actual records/RIDs, or a new empty pair is initialized. Caches and files
are synchronized, immutable baseline copies are written and synchronized, the
WAL header is forced, and the matching manifest is atomically published and the
directory synchronized before clients can start transactions. Newly created
directory entries, including nested parents, are synchronized too. Bootstrap can
resume before the manifest is published if no transaction frames exist. A
nonempty unmatched pair is rejected. A complete but invalid initialization
header is rejected conservatively; no repair of arbitrary initialization damage
is promised.

## WAL records and binary format

All fields use explicit little-endian encoding. LSNs are sequential unsigned
64-bit record numbers starting at 1. Transaction IDs are allocated above all
IDs in the accepted log prefix, including incomplete and aborted transactions.
The process-wide counter also prevents reuse between simultaneously open
independent databases. Each transaction has a previous-record LSN chain.
Stable file IDs are 1 for the heap and 2 for the index within this directory.

The 64-byte WAL header is copied verbatim into `recovery.meta`:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `TWL1` |
| 4 | 4 | Version 1 |
| 8 | 4 | Page size 4096 |
| 12 | 4 | Reserved zero |
| 16 | 8 | Baseline heap length |
| 24 | 8 | Baseline index length |
| 32 | 4 | CRC-32 of baseline heap |
| 36 | 4 | CRC-32 of baseline index |
| 40 | 4 | CRC-32 of this header, with this field zeroed |
| 44 | 20 | Reserved zero |

Every subsequent record starts with an 80-byte frame header:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `TWR1` |
| 4 | 4 | Total frame size (80, 4176 or 8272) |
| 8 | 2 | Version 1 |
| 10 | 2 | BEGIN=1, UPDATE=2, COMMIT=3, ABORT=4 |
| 12 | 4 | Header CRC over bytes 0–75, this field zeroed |
| 16 | 8 | LSN |
| 24 | 8 | Transaction ID |
| 32 | 8 | Previous transaction-record LSN, zero for BEGIN |
| 40 | 4 | File ID, zero for transaction-only records |
| 44 | 4 | Page ID, zero for transaction-only records |
| 48 | 4 | Before-image-present flag (0 or 1) |
| 52 | 24 | Reserved zero |
| 76 | 4 | CRC of complete frame, this field zeroed |
| 80 | variable | Optional 4096-byte before-image, then required UPDATE after-image |

UPDATE after-images have the record LSN at page offset 16 and a recomputed page
checksum. Before-images are validated pages; newly extended pages can have no
before-image. BEGIN/COMMIT/ABORT have no payload. Full before/after images include
superblocks, free pages, generation reservations, index headers, leaf/internal
nodes and structural changes. The current recovery algorithm uses after-images
and transaction decisions; it does not apply WAL before-images incrementally.
They remain actual recorded before states, not synthetic undo placeholders.

The parser validates protected lengths, both checksums, supported types/flags,
page identities/checksums, after-image LSNs, consecutive record LSNs and transaction
chain/state transitions. It truncates only an incomplete final header or a final
payload whose protected header proves the expected length. It fails on a corrupt
complete frame, even the final frame. It forces the accepted prefix before using
it for recovery, including complete frames left visible by an unacknowledged
process-crash commit. Checksums detect accidental damage, not malicious edits.

## Write-ahead ordering

On the first write intent, the existing database writer gate excludes other
write transactions. Logical table/key locks still protect readers. The first
write flushes the preceding committed cache baseline and arms the fast volatile
undo journals retained from Phase 4.

A buffer write guard validates the new page image, appends UPDATE with current
before-image and new after-image, assigns the new LSN, then installs the dirty
image. It does not force the log just to retain a dirty page in RAM. Eviction,
explicit flush, next-writer baseline flush and close force the WAL through the
frame's LSN before calling pwrite. Committed frames can remain dirty in memory.

Allocation, deallocation and record-generation metadata bypass the cache. Their
raw disk write path independently appends a full-page UPDATE and forces it before
writing any bytes. Buffer initialization reads the newly allocated image back so
it preserves the allocator-assigned LSN. Disk and buffer attachment preserve
page types and prevent an unlogged zero-LSN page reaching WAL-backed disk writes.
Standalone raw storage APIs remain unlogged unless explicitly integrated.

COMMIT is appended and forced through fsync before success is returned. There is
no data-file force at commit. Thus a crash can leave one/both files behind the
committed log, and recovery must redo them. Read-only transactions also log
BEGIN/COMMIT, which makes durable ID/state tracking straightforward but adds
unoptimized overhead. `sync()` flushes materialized files with no active clients;
it does not checkpoint or shorten the WAL. Close performs checked data flushes
and WAL close after all active transactions finish.

ABORT is appended/forced **before** live physical undo. It records a durable
abort decision, not completion of undo. Only then may the old per-file before
snapshots restore baseline pages without generating new UPDATEs. Cache images
are discarded and index state reloaded before locks release. If a crash interrupts
this undo, reconstruction excludes that transaction and restores the same final
state. CLRs are unnecessary for this full-reconstruction protocol; they would be
needed for a different incremental undo design. Generation reservations remain
above every observed WAL metadata after-image, including losers, so IDs from
aborted work do not reappear.

## Restart algorithm

1. Lock the WAL and validate its manifest/header. Parse/validate the full log and
   identify committed, aborted and incomplete transactions. Trim only provably
   incomplete tail bytes and force the accepted log prefix.
2. Lock both raw data files without trying to interpret their potentially torn
   headers. Validate baseline lengths and whole-file CRCs before publishing any
   data. Initialize two staging files from those immutable baselines.
3. Replay UPDATE after-images of **committed transactions only**, in LSN order,
   into staging files. The single-writer gate ensures each committed after-image
   includes no other writer's uncommitted changes. Ignoring aborted/incomplete
   updates and replacing existing materialized files removes their effects even
   if their pages had already been stolen/flushed.
4. Derive final file lengths from reconstructed superblocks, truncate stale
   extensions, and preserve the maximum observed heap generation reservation.
   Validate every reconstructed page checksum and synchronize staging files.
5. Publish reconstructed pages into the locked data files, truncate and fsync
   both, and synchronize the directory. A crash during any staging/publishing
   step simply causes the next open to start again from the unchanged baselines
   and WAL; no partially published state is accepted as a new baseline.
6. Open normal disk managers/caches, validate allocator chains, heap slots, B+
   tree invariants and exact heap/index bindings, attach the WAL and accept clients.

Recovery always replays full history; it does not skip by existing data-page
LSNs. That deliberate choice repairs torn pages as well as ahead/behind files.
There is no incremental checkpoint, WAL recycling, group commit, background
flusher, concurrent writer recovery, log replication or media-redundancy scheme.
Startup I/O and RAM scale with baseline/log history; the current parser retains
records while reconstructing. The WAL grows without bound until a separately
verified checkpoint design is implemented. No recovery-speed claim is made.

## Failure outcomes and assumptions

A log append/force failure fails the WAL and database. No commit success is
returned. If COMMIT was fully written or forced before an error reached the
client, the outcome can be **indeterminate**, and recovery may commit it. The
coordinator never physically undoes a transaction after a COMMIT decision has
entered the log. Such errors use FAILED/`TransactionFailed`, not a false ABORTED
claim; resolve remaining handles, destroy the owner and reopen to recover.
Normal constraint/timeout errors durably abort and undo before locks release.
A failed ABORT force prevents unlogged physical restoration and also fails closed.

Assume exclusive cooperating ownership, local Linux filesystems and devices
honoring successful fsync (including file sizes and directory entries). Tests
exercise actual process exits and partial writes, not storage-device power cuts.
Loss/corruption of the WAL, manifest or immutable baselines is detected but cannot
be repaired from the materialized data. A complete checksummed-frame failure is
conservatively fatal, including an unforced full-length torn tail. Do not claim
recovery from arbitrary media damage or unrestricted ACID certification.
Raw HeapFile/BPlusTree usage has no automatic crash recovery and must not be mixed
with coordinator file access while it owns the directory.

## Test evidence and fault points

`recovery_worker` is exec'd as a separate process by GoogleTests. Fault hooks
terminate it with `_exit(86)` while real file bytes remain partially written.
Coverage includes between account updates, before commit, after durable COMMIT
before the final data flush, acknowledged commit without close, half data writes,
partial BEGIN/UPDATE/COMMIT frames, durable ABORT during undo, index splits/free
page reuse, torn superblocks, missing data files and repeated recovery crashes.
An offline raw heap read after the durable-COMMIT crash proves one balance is
still old before reopen; recovery then returns the entire committed pair.

Recovery is killed repeatedly after baseline copy, during redo, between halves
of a published page, and between heap/index publication; final repeated opens
must yield identical records and valid allocator/index structures. Additional
tests reject damaged complete frames, bad payload CRCs, missing required files,
corrupt baselines, duplicate ownership and data-only copies with WAL stamps.
Exception injection is used separately for force-error outcomes, not as a
substitute for these process crashes. See verification.md for actual results.

`DatabaseOptions::fault_hook` is empty in production. When explicitly configured,
it observes named points and may exit/throw for testing. It must not reenter
storage/transaction APIs or take locks; some hooks execute under internal latches
or the WAL mutex. It is not a database event callback for application logic.
