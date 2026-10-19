# Storage format

Files use 4096-byte pages encoded in little-endian order. Each page has a
24-byte header with its type, ID, checksum and WAL position. Page 0 tracks
allocation and free pages; index page 1 stores the tree header.

| Page type | Contents |
| --- | --- |
| Heap | Slot directory and variable-length records |
| Index leaf | Sorted key/record-ID pairs and next-leaf link |
| Index internal | Separator keys and child pointers |
| Free | Link to the next reusable page |

Record IDs contain a page, slot and generation. Generations prevent stale IDs
from reading reused slots. Compaction keeps slot IDs stable.

A record must fit one page: at most 4052 bytes. Accounts store a 64-bit key,
64-bit balance and up to 4032 name bytes. Updates fail if the record cannot
grow in place.

Startup checks checksums and structure. Transactional recovery runs first to
repair interrupted writes; raw storage mutations are not crash atomic.

For exact layouts, see [page headers](../include/storage/page.h),
[heap pages](../src/storage/slotted_page.cpp) and
[index pages](../src/index/index_page.cpp).
