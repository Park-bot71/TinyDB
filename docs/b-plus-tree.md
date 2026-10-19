# Persistent B+ tree (Phase 3)

`BPlusTree` implements a unique signed 64-bit integer-key index mapping to stable
heap record IDs. Nodes and root metadata are persisted in custom 4096-byte pages
through the existing CLOCK buffer pool. Search, insertion, deletion, incremental
splits/merges/redistribution, and ordered range scans are implemented. There is
no in-memory map standing in for the index and no rebuild of the whole tree on
mutation. The test oracle uses `std::map`; the engine does not.

## API and ownership

```cpp
#include "index/b_plus_tree.h"
#include "storage/heap_file.h"
#include "storage/record.h"

tinydb::HeapFile heap("accounts.db", 8);
tinydb::BPlusTree index("accounts.idx", {.buffer_frames = 8});
auto rid = heap.insert(tinydb::serialize({1, "Alice", 1000}));
bool inserted = index.insert(1, rid); // false for a duplicate; original value remains.
auto found = index.find(1);          // optional<RecordId>.
if (found) {
  auto row = tinydb::deserialize(heap.get(*found));
}
auto rows = index.range(0, 100);     // Sorted half-open [0, 100).
auto first_ten = index.scan({}, {}, 10); // Omitted bounds cover the full key domain.
auto invariants = index.validate();
heap.close();
index.close(); // Independent durable closes, not an atomic transaction.
```

There is one index per index file. Page 0 remains allocator metadata, page 1 is
an index header, and all other allocated pages must be reachable tree nodes.
The heap and index currently own independent files, disk managers and pools.
The transactional table layer binds an index to its heap and maintains both
within one transaction. Standalone calls do not automatically index `HeapFile`
operations, enforce key uniqueness inside a raw heap, validate RID referential
integrity, or persist a heap-file identity in the index. Supply record IDs from
one intended heap and keep its keys consistent with the index.

Phase 4 now provides that coordination for a single fixed-schema table through
`TransactionalDatabase` (transactions.md). The raw APIs in this guide retain
their independent-call semantics. The coordinator's physical rollback reloads
the index header and validates the restored tree after disk undo/cache discard;
structural event counters may include work later rolled back.

`insert` returns false for a duplicate without replacing its value. `erase`
returns false for an absent key. Invalid RID page/generation fields raise
`InvalidArgument`; the index treats slot numbers as opaque. Range bounds are
inclusive lower/exclusive upper; equal bounds yield no rows, reversed bounds
raise `InvalidArgument`, and a zero limit yields no rows. Unbounded scans include
both `INT64_MIN` and `INT64_MAX` without sentinel-key arithmetic. Results are
copied to vectors while holding the tree's shared latch.

`BPlusTreeOptions::max_keys` is zero by default: a new index uses 184, while
reopen loads its persisted capacity. Explicit capacities 3–184 are supported
and must match on reopen. Small capacities are useful for structural testing;
normal use should keep the default. Capacity is the maximum number of keys in
both leaf and internal nodes. Internal pages could physically hold more keys,
but the same configured bound is used for both types in this implementation.
Buffer capacity can change on reopen, and a one-frame index is supported.

## Structure and algorithms

Leaf nodes contain sorted `(key, RID)` entries and the next leaf page ID.
Internal nodes contain sorted separator keys and one more child pointer than
keys. A separator is exactly the minimum key of its right subtree. Equality
therefore follows the right child (`upper_bound`) during descent. Every node
stores its parent page ID; the root has no parent. All leaves have equal depth.

A nonroot leaf requires at least `ceil(max_keys / 2)` entries. A nonroot internal
node requires at least `floor(max_keys / 2)` keys, or one more child. The root
may contain fewer keys, but an internal root must have at least two children.
An empty index has no root node; only the index header remains allocated.

Insertion rejects duplicates before modifying tree contents. A leaf overflow
splits its entries in half and links the new right leaf into the leaf chain.
Its first key is inserted into the parent. An internal overflow promotes the
middle key, splits keys and children around it, and repairs moved children's
parent pointers. Splits propagate up, creating a new root if needed. Separator
minima are refreshed along the affected ancestor chain, including when a new
smallest key does not split a node.

Deletion first removes the leaf entry and updates ancestor minima. An underfull
node borrows from a sibling with more than minimum occupancy, preferring the
left sibling. Internal redistribution rotates the parent separator and moves a
child pointer, updating the child's parent. When neither sibling can lend, the
adjacent nodes merge into the left member of the pair. The parent loses a key
and child, the obsolete page is freed, and underflow repair continues upward.
An empty internal root is replaced by its only child; the final empty leaf is
freed. Freed pages return to the persistent allocator and can be reused as a
different node type.

Point lookup follows one root-to-leaf path using binary searches. Scans descend
once and then follow leaf links. Structural operations copy only affected nodes
and their paths, not the whole index. Exact-minimum repair can descend subtrees
again at each ancestor, so it trades additional I/O for a straightforward
implementation; no optimized insertion/deletion complexity or throughput claim
is made. Startup validation and explicit `validate()` do read the entire index.

## Synchronization and failure behavior

A per-index `shared_mutex` permits concurrent point/range readers and excludes
all other tree calls during a mutation. Mutations acquire it before accessing
the buffer pool. Node images are copied while a page guard is active; the guard
is dropped before another page is fetched. At most one page is pinned by each
tree call at a time. This avoids latch-order cycles and allows structural work
with a one-frame pool. It does not implement transaction locks or isolation
across multiple calls. A scan sees one structurally consistent index state;
it does not atomically read matching heap records.

A failure during mutation or synchronization marks the tree failed; subsequent
ordinary operations raise `Io`. Invalid arguments, duplicate insertion and an
absent deletion detected before mutation leave it usable. Explicit `sync()` and
`close()` flush all dirty pages and synchronize the file. Close is idempotent
on success. Destructors do not flush and cannot establish durability. A new
index synchronizes its initial empty header before the constructor succeeds.

Standalone calls have no WAL or crash-atomic structural operation. Node
allocation, parent/leaf-link updates, root/size changes and free-list writes can
reach disk at different times. A failed standalone mutation can leave an index
that needs rebuilding; opening the raw index does not repair it. Do not catch a
failed structural mutation and continue using that object. The Phase 5
transactional coordinator logs all these changes with stable file/page IDs and
reconstructs the committed tree before opening it, including allocation metadata.
See [recovery.md](recovery.md) for recovery requirements and limits.

## Validation and diagnostics

Opening an existing index validates it before exposing the object. `validate()`
checks the cached header against the in-memory root/size/capacity and recursively
checks type, checksums, sorted unique keys, capacity, minimum occupancy, parent
pointers, exact separator minima, disjoint child ranges, and equal leaf depth.
It detects cycles/shared children, missing/unallocated references, orphan pages,
entry-count disagreement, and any mismatch between the DFS leaf order and the
next-leaf chain. Traversals have a height bound as an additional corruption guard.

`TreeValidation` reports height, node count, leaf count and entries; an empty
index has height zero. `stats()` exposes per-open split, merge, borrow and root
change counters for structural tests. They are not persisted and a failed
mutation can leave counters reflecting partial work. `buffer_stats()` exposes
actual buffer activity, including validation fetches. These are diagnostics,
not measured throughput or performance improvements.

## Try the real heap/index demo

```bash
./build/index_demo /tmp/accounts.db /tmp/accounts.idx
./build/index_demo /tmp/accounts.db /tmp/accounts.idx
```

For a fresh pair of files, both invocations print the same records in key order:

```text
1 | Alice | 1000
2 | Bob | 500
3 | Carol | 750
```

The demo seeds an empty pair and follows persisted RID pointers into the heap.
Use a matched pair of files; there is no repair or table/catalog binding yet.
