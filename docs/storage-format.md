# Storage format

Each data file consists of 4096-byte pages, encoded explicitly in little-endian
order. Page 0 is allocator metadata; `UINT32_MAX` is the invalid page ID.
CRC-32 covers a page with its checksum field zeroed (ISO-HDLC polynomial
`0xedb88320`, initial/final XOR `0xffffffff`).

## Common page header

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | Magic `TDB1` |
| 4 | 2 | Version 1 |
| 6 | 2 | Type: metadata=0, heap=1, free=2, index header=3, leaf=4, internal=5 |
| 8 | 4 | Page ID |
| 12 | 4 | CRC-32 |
| 16 | 8 | WAL LSN; zero for unlogged pages |

## Layouts after the header

- **Metadata:** page count at 24 (`u32`), free-chain head at 28 (`u32`), next
  record generation at 32 (`u64`). Free pages link to the next free page at 24.
- **Heap:** slot count, packed-payload start and live count are `u16` fields at
  24/26/28. The slot directory starts at 32. Each 12-byte slot contains
  `(offset:u16, length:u16, generation:u64)`; payload grows backward from page end.
- **Index header (page 1):** root at 24 (`u32`), node capacity at 28 (`u16`),
  entry count at 32 (`u64`).
- **Index nodes:** key count at 24 (`u16`), parent at 28 (`u32`), next leaf at
  32 (`u32`), entries from 40. Leaves use 22-byte key/RID pairs; internal nodes
  store a first child followed by `(key:i64, child:u32)` pairs.

Record IDs are `(page:u32, slot:u16, generation:u64)` values. Generations prevent
stale IDs from addressing reused slots. Compaction preserves slot IDs. A record
fits within one page, up to 4052 bytes; an update that cannot grow in place fails.

The account codec stores version/name length (`u16` each), key/balance (`i64`
each), then name bytes: a 20-byte header and at most 4032 name bytes.

Startup validates checksums, lengths, slot bounds, allocation chains and index
structure. Under the coordinator, [WAL recovery](recovery.md) runs first to
repair interrupted data writes. Raw files have no crash-atomic mutation guarantee.
For exact validation rules, see [slotted pages](../src/storage/slotted_page.cpp)
and [index pages](../src/index/index_page.cpp).
