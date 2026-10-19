# Buffer pool

The buffer pool caches 4096-byte pages. Heap and index files each have their
own pool, with 64 frames per file by default.

- `fetch_read()` gives shared access; `fetch_write()` gives exclusive access.
  Their guards keep pages pinned until released. Page references must not
  outlive the guard; `replace(image)` marks a changed page dirty.
- CLOCK replacement skips pinned pages. If every frame is pinned, fetching or
  allocating returns `Busy`.
- `flush_all()` writes dirty pages to the OS; `sync()` also calls `fsync`.
  Use checked `close()` to save changes and receive errors. These operations
  reject live pins; standalone destructors do not flush.
- Transactional writes save WAL before dirty pages reach disk.
- `stats()` reports capacity, hits, misses, reads, writes and evictions. Counters
  exclude allocator I/O and the OS cache.

The disk manager must outlive the pool. Do not bypass the cache to change pages
or reenter the pool from WAL callbacks.

See the [API](../include/buffer/buffer_pool.h) and [recovery](recovery.md).
