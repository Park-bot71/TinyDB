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
| 6 | 2 | Type: metadata 0, heap 1, free 2 |
| 8 | 4 | Page ID, must match file offset |
| 12 | 4 | CRC-32 |
| 16 | 8 | Page LSN, zero in Phase 1, reserved for future WAL |

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
primary keys will be enforced by the future B+ tree/table layer.

## I/O and durability boundary

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

There is no crash-atomic mutation in Phase 1: allocation and generation changes
write more than one page, and a single page write can tear. A failed write can
leave a file that will not reopen. No automatic repair, transaction rollback or
power-loss durability of unsynchronized calls is promised. WAL must intercept
all heap/index/allocator writes and make superblock repair possible before normal
startup validation in the later recovery phase. The format can evolve with a
version bump; no migration compatibility is promised yet.
