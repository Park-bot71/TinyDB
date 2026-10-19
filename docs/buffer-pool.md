# Buffer pool (Phase 2)

`BufferPool` sits between the heap and `DiskManager`. It allocates a fixed array
of 4096-byte frames once at construction, with a page-to-frame hash table. The
capacity must be positive. Heap files use 64 frames by default; pass a different
count as the second `HeapFile` constructor argument. A one-frame heap is supported.
The slotted-page format and record IDs are unchanged.

## Guards and replacement

`fetch_read(id)` returns a movable, noncopyable `ReadPageGuard` holding a shared
page latch and a pin. `fetch_write(id)` returns a `WritePageGuard` with an
exclusive latch and pin. A cache hit increments the existing frame's pin count;
a miss reads and validates the disk page into a replaceable frame. Multiple
clients can read the same page concurrently. Writers exclude readers and other
writers for that page. Do not recursively acquire the same page latch or attempt
read-to-write upgrades. For multi-page work, acquire latches in ascending page ID
order and release them before waiting on logical transaction locks. Heap calls
currently hold at most one page guard at a time.

Guards unlock the latch before decrementing the pin count, including on exception
unwinding. Moves transfer the existing pin instead of creating another. `drop()`
is idempotent. Access through a moved/dropped guard raises `Closed`. A page
reference is valid only while its guard remains active. Latches must be released
on the same thread that acquired them. Frame storage has shared ownership, so a
live guard can safely be dropped after the pool object is destroyed; there is no
implicit flush in that situation.

The replacement policy is CLOCK. An access sets a reference bit. The hand skips
pinned frames, clears reference bits on the first pass, and selects an unpinned
frame with a cleared bit. Empty frames are used first. At most two full passes
are needed while holding the pool mutex. If every frame is pinned, the request
raises `Busy` immediately. There is no pool-capacity wait or automatic retry;
callers must release guards or retry after releasing other resources. A waiter
for a page latch also owns a pin, protecting its eventual frame from eviction.

`new_page()` checks frame availability and flushes its victim before allocating
a disk page. Thus pin exhaustion and failed WAL force do not allocate a page.
An allocation followed by memory-allocation failure may leave an allocated empty
page, which the heap can use later. `delete_page(id)` rejects pinned pages,
frees the disk page, and removes any cached image without flushing obsolete data.
A reused page ID receives a fresh empty frame image. New-page creation reads the
allocated page back so that a WAL-assigned LSN is preserved. Allocation, free-list
and record-generation writes use the direct disk allocator; the Phase 5
coordinator attaches WAL to this path and forces each update before its write.
Standalone allocators remain unlogged.

Phase 3 adds `new_page(PageType)` for heap, index-header, index-leaf and
index-internal pages. The default remains a heap page. Metadata/free types are
rejected before allocation. Both the disk manager and guard replacement enforce
the allocated type; a page may change type only after being freed/reallocated.

Phase 4 adds `discard_all()` for restoring a quiescent cache after physical disk
undo. It rejects pins, invalidates all images without flushing, and resets the
pool failure flag after disk restoration. This is a destructive cache operation
with an explicit owner-quiescence precondition, not a substitute for sync. The
transaction coordinator calls it under its exclusive storage latch after the
disk manager restores its baseline. Independent users must uphold that contract.

## Dirty state, LSNs and flushing

A write guard exposes only a const page. Modify a local copy and call
`replace(image, lsn)` to install it and mark it dirty. Replacement checks page
identity/type, disallows decreasing the resident LSN, writes the requested LSN
at offset 16 and recalculates/validates the checksum before changing the frame.
Invalid replacement leaves the old image intact. Heap operations use
`SlottedPage` to validate record layout before replacement. With attached WAL,
replacement requires the caller's LSN argument to be zero, appends a full
before/after UPDATE, installs its assigned LSN and only then marks the page dirty.
Standalone heap operations keep LSN zero; standalone pools also support explicit
nondecreasing LSNs with a caller-provided force callback.

Every dirty write, whether triggered by eviction, `new_page()`, `flush_page`,
`flush_all`, `sync` or `close`, uses the same ordering:

1. Read the frame's page LSN under exclusive pool ownership (no pins).
2. For a nonzero LSN, call `ForceWal(lsn)`. It must force the log through that
   LSN before returning, or throw. If no callback exists, reject the write.
3. Write the page through `DiskManager`.
4. Clear dirty state and count the successful page write.

LSN zero means unlogged data and skips the callback. The Phase 5 coordinator
attaches its durable WAL force implementation to both pools, with another LSN
force check at the disk boundary. Standalone callback tests inspect the actual
data file during callback execution to verify ordering. Fatal log failure keeps
rejecting subsequent forces; the pool cannot decide whether an arbitrary
provider's log error is recoverable.

If WAL force throws, the frame stays resident, unpinned and dirty, and the page
write does not occur. Callers may retry if the WAL provider permits it. If a data
write or file sync throws, the pool enters a failed state; even cache hits then
fail with `Io`. Stop using it and reopen the database. Standalone pools detect
damage without repairing it; the transactional coordinator runs WAL recovery
before constructing its pools.

`flush_page` requires that page to have no pins. A valid nonresident page is
already outside the cache, so flushing it validates it and does nothing.
`flush_all`, `sync`, and `close` reject **any** live pin before writing any page.
They do not wait on page latches. A later I/O/WAL failure can still leave a
partially completed flush with already-written frames clean and others dirty.
Concurrent callers can continue fetching after a successful flush returns.

Flushing transfers dirty bytes to the OS; it does **not** promise stable storage.
`sync()` flushes all frames and then calls disk `fsync`. `close()` does the same
and marks the pool closed, rejecting future operations; it leaves the borrowed
disk manager open. `HeapFile::close()` closes both in that order. Repeated
successful close is harmless. Destructors discard unflushed cache contents and
close descriptors without claiming durability. Always use checked `sync()` or
`close()` for standalone writes that must survive process exit. Transactional
writes become durable through the coordinator's forced COMMIT instead.

## Synchronization and lifetime

The pool mutex protects the page map, pins, CLOCK bits, counters and lifecycle.
All disk I/O and WAL callbacks run synchronously under that mutex. This keeps
frame publication, eviction and deletion atomic with respect to other pool
operations, at the cost of serializing misses and flushes. There is no background
flusher, asynchronous I/O, or sharding in this milestone.

Acquiring a guard increments its pin under the pool mutex, releases that mutex,
and only then waits for its page latch. The pool **never acquires a page latch
while holding its mutex**. Zero pins therefore lets replacement/flush safely
access a frame without acquiring its latch. Heap calls acquire the heap mutex,
then use the pool; disk operations acquire the disk mutex. Guard replacement
accesses page bytes only while exclusively latched. Dirty state is atomic so
`stats()` can observe it even during an active writer. The WAL callback must not
reenter the pool, acquire page latches, or wait for a logical transaction lock.

The borrowed `DiskManager` must outlive pool operations. Do not mutate, allocate,
free, or close it through another path while the cache is active. Pool wrappers
provide allocation metadata and generation access for the heap. Read-only disk
inspection is used in tests to verify actual write boundaries. Startup disk
validation remains a full-file scan; heap layout validation now fetches through
the bounded cache. This does not eliminate startup I/O.

## Statistics

`stats()` reports frame capacity, resident/dirty/pinned pages, total pins,
fetch hits/misses, successful fetch disk reads, successful dirty page writes,
and evictions. Hits include fetches waiting on a latch; misses include failed
fetch requests. New-page creation does not count as a fetch. Evictions count
replacement of resident pages, not explicit deletion. Allocation/metadata I/O
and disk-manager startup reads are not included. `hit_rate()` is hits divided
by attempted fetches, or zero for no fetches. `HeapFile::buffer_stats()` exposes
the same counters, including heap startup layout-validation fetches. These are
measurements of cache activity, not benchmark throughput results.

## Example

```cpp
#include "buffer/buffer_pool.h"
#include "storage/record.h"

tinydb::DiskManager disk("pages.db");
tinydb::BufferPool pool(disk, 8);
auto guard = pool.new_page();
tinydb::SlottedPage page(guard.page());
auto rid = page.insert(tinydb::serialize({1, "Alice", 1000}), pool.reserve_generation());
guard.replace(page.page());
guard.drop();
pool.sync();
auto reader = pool.fetch_read(rid.page);
auto record = tinydb::deserialize(tinydb::SlottedPage(reader.page()).get(rid));
reader.drop();
pool.close();
disk.close();
```
