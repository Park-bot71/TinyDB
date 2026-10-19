# Storage format v1

All integers use explicit little-endian encoding. No C++ object layout is
persisted. A database is exactly `page_count * 4096` bytes. Page IDs are unsigned
32-bit numbers; 0 is metadata and `UINT32_MAX` is invalid. Files require 64-bit
POSIX offsets. Checksums cover every byte with bytes 12–15 treated as zero.
The checksum is CRC-32/ISO-HDLC (reflected polynomial `0xedb88320`, initial and
final XOR `0xffffffff`). It detects accidental corruption, not malicious edits.

## Common prefix (all pages)

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `TDB1` |
| 4 | 2 | Format version (1) |
| 6 | 2 | Type: metadata 0, heap 1, free 2, index header 3, leaf 4, internal 5 |
| 8 | 4 | Page ID, must match file offset |
| 12 | 4 | CRC-32 |
| 16 | 8 | Page LSN; zero for unlogged pages, assigned by WAL under the coordinator |

## Superblock (page 0)

| Offset | Bytes | Field |
| --- | --- | --- |
| 24 | 4 | Total page count including superblock |
| 28 | 4 | Free-chain head or `UINT32_MAX` |
| 32 | 8 | Next record generation (starts at 1) |
| 40 | 4056 | Zero/reserved |

Reserving a record generation updates the superblock before writing the record.
Gaps are allowed. Generations never wrap: exhaustion rejects inserts. Allocating
a page first consumes the free-chain head, otherwise extends the file. Freeing
an empty heap page pushes it onto that chain. Free pages retain their page IDs;
no file shrinking occurs. A free page stores its next-free page ID at offset 24
and zero at offset 28. Startup validates chain bounds, cycles, page types, page
checksums and exact file length. The heap layer also validates every slot layout.
A full-file validation on open is intentionally conservative and O(file size).

## Heap pages

| Offset | Bytes | Field |
| --- | --- | --- |
| 24 | 2 | Number of directory slots |
| 26 | 2 | Start of packed payload (`free_end`) |
| 28 | 2 | Live record count |
| 30 | 2 | Reserved zero |
| 32 | 12 × slot count | Slot directory growing upward |
| directory end | variable | Free space |
| free_end | variable | Packed record payloads growing downward |

Each slot is `(offset:u16, length:u16, generation:u64)`. An unused/deleted slot
has offset and length zero and retains its old generation. New records reuse
these slots before growing the directory and receive a newly reserved generation.
Zero-length records are disallowed. A live record fits entirely within its page;
maximum size on an empty page is **4052 bytes**. Compaction rewrites payloads
but preserves all slot indexes and generations. Dead directory slots are retained
until the whole page becomes empty and is freed.

Record IDs are `(page:u32, slot:u16, generation:u64)` as logical values; they are
not dumped as padded structs. A mismatching generation, out-of-range slot or
unallocated page returns `NotFound`. IDs are scoped to their database file.
An update may grow only if its current page has enough space; otherwise it
returns `PageFull` without changing the original record. There is no forwarding
record or relocation yet.

## Typed example record codec

Opaque heap records can contain arbitrary bytes. The optional account-like codec
uses a 20-byte header followed by exactly the declared number of name bytes:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | Codec version (1) |
| 2 | 2 | Name byte length |
| 4 | 8 | Signed 64-bit key, two's-complement bits |
| 12 | 8 | Signed 64-bit balance, integer units |
| 20 | name length | Name bytes, no terminator |

Empty names and embedded NUL bytes work. UTF-8 is not validated. Maximum name
length is 4032 bytes. Duplicate keys are permitted in this unindexed heap; unique
primary keys are enforced by the B+ tree/transactional table layer.

## Index pages (Phase 3 extension)

The common v1 page prefix and all existing heap/allocator layouts are unchanged.
Index page type values 3–5 extend the type set; earlier readers reject them.
An index lives in its own file with allocator metadata at page 0, its header at
page 1, and node IDs starting at 2. A nonempty heap file and an index file cannot
be opened interchangeably. No migration is needed for existing heap files.

The index header has the following fields after the common prefix:

| Offset | Bytes | Field |
| --- | --- | --- |
| 24 | 4 | Root ID or `UINT32_MAX` for an empty tree |
| 28 | 2 | Maximum keys per node (3–184, default 184) |
| 30 | 2 | Reserved zero |
| 32 | 8 | Total leaf entries |
| 40 | 4056 | Padding written as zero |

A zero entry count requires an invalid root, and a nonzero count requires a
root ID at least 2. All allocated pages except the header must be reachable
from that root. Root/capacity/count are checksummed with the rest of the page.

Leaf and internal nodes share this layout after the common prefix:

| Offset | Bytes | Field |
| --- | --- | --- |
| 24 | 2 | Number of keys |
| 26 | 2 | Reserved zero |
| 28 | 4 | Parent ID or `UINT32_MAX` for the root |
| 32 | 4 | Next leaf ID, `UINT32_MAX` at end or for an internal node |
| 36 | 4 | Reserved zero |
| 40 | variable | Entries/children |

Leaf entries are exactly 22 bytes each: signed key bits (8), RID page (4), slot
(2), and generation (8). These are encoded field-by-field with no alignment
padding. At 184 entries the page uses 4088 bytes, leaving 8 padding bytes.
An internal node stores its first child ID (4 bytes) at offset 40, followed by
`(separator key:8, next child:4)` pairs starting at offset 44. Internal nodes
also use the persisted maximum of 184 keys, even though their physical layout
has room for more. Keys use two's-complement signed 64-bit bits and compare
as signed integers in memory. Remaining padding is written as zero and ignored
when decoding. Header/node LSNs are assigned by WAL under the coordinator;
standalone index operations use zero.

Both the disk manager and write guards preserve the allocated page type.
Freeing and reallocating a page may legitimately change its data-page type.
See b-plus-tree.md for occupancy, separator, leaf-chain and failure rules.

## I/O and durability boundary

The page layout is unchanged through Phase 5. Heap and index data I/O go through
the bounded buffer pool (see buffer-pool.md), while allocation and generation
metadata use direct disk writes. Under the transactional coordinator, those
writes append and force full-page WAL first. Standalone raw calls remain
unlogged. Heap `sync()`/`close()`
flush the pool before synchronizing/closing the disk file. Unflushed heap changes
are discarded on destruction; even read-after-write success in one process
does not establish durability.

The coordinator uses `records.db` and `primary.idx` within a database
directory. It captures first-before-images of overwritten disk pages in RAM,
including allocator writes, after a clean cache baseline. Live rollback restores
those images, truncates new pages, reloads allocator state, invalidates caches,
and retains the record-generation high water mark. The physical file format has
no new fields: the reserved page LSN now carries the UPDATE record's LSN.
Phase 5 also adds `database.wal`, `recovery.meta`, `baseline.heap` and
`baseline.index`. Recovery reconstructs data files before their normal startup
validation. See [recovery.md](recovery.md) for their formats and durable ordering,
and transactions.md for the undo/discard APIs' quiescence requirements.

`pread`/`pwrite` retry EINTR and loop over short transfers. EOF inside a page is
corruption; write or sync failure throws `Error` and poisons the handle. Once
poisoned, all ordinary operations fail and the destructor closes the descriptor.
Validation/capacity errors before writing do not poison it. The destructor never
claims durability and cannot report close errors. Use explicit `sync()` or
`close()` (which synchronizes) to receive persistence errors. `close()` is
idempotent after success. Successful writes alone reach the OS page cache.
Creating a file synchronizes both its initial superblock and parent directory.
The implementation assumes a local Linux filesystem and device honoring fsync.
Advisory exclusive flock excludes other cooperating owners, including a second
handle in the same process. Do not share a live handle across fork or replace,
truncate, or mutate its file through other software.

Standalone raw mutations are not crash atomic: allocation, generation and index
changes write more than one page, and a single write can tear. A raw failed write
can leave a file that will not reopen. The Phase 5 coordinator logs every such
mutation and can reconstruct torn data pages and superblocks from its immutable
baseline and committed log history. It requires intact recovery inputs and the
documented filesystem assumptions. The format can evolve with a version bump;
no migration compatibility is promised yet.
