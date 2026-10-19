# Buffer pool

`BufferPool` caches a fixed number of 4096-byte pages. Heap and index files each
have their own pool; the default is 64 frames per file.

## Access and replacement

- `fetch_read(id)` returns a pinned guard with a shared page latch.
- `fetch_write(id)` returns a pinned guard with an exclusive page latch.
- Modify a page copy, then call `replace(image)` to mark it dirty.
- Dropping or destroying a guard releases its latch and pin. Page references
  must not outlive the guard.
- CLOCK skips pinned frames. If every frame is pinned, allocation/fetch returns
  `Busy`; release guards before retrying.

`new_page(type)` allocates a page; `delete_page(id)` frees an unpinned page.
`discard_all()` is reserved for quiescent rollback restoration and drops images
without flushing.

## Flushing and WAL

Dirty eviction and explicit flushes force WAL through the page's LSN before
writing the data page. Under the transaction coordinator, replacement logs the
page change and assigns its LSN before the frame becomes dirty. Direct allocator
writes follow the same write-ahead rule.

`flush_all()` writes dirty pages to the OS. `sync()` also calls `fsync`;
`close()` synchronizes and closes the pool. These operations reject live pins.
Standalone destructors discard unflushed contents; use checked close to receive
I/O errors. Transactional durability comes from forced COMMIT instead.

## Synchronization and counters

The pool mutex protects mappings, pins, CLOCK state and synchronous I/O. It is
released before waiting for a page latch. WAL callbacks must not reenter the pool
or wait for transaction locks. The borrowed disk manager must outlive the pool;
do not bypass the cache to mutate it.

`stats()` reports capacity, resident/dirty/pinned frames, fetch hits/misses,
disk reads, dirty writes and evictions. Allocator I/O and OS cache hits are excluded.
See the [API](../include/buffer/buffer_pool.h) and [recovery protocol](recovery.md).
