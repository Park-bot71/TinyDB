# B+ tree index

`BPlusTree` stores unique signed 64-bit keys mapped to heap record IDs in a
separate file. It supports lookup, insertion, deletion and ordered range scans.

```cpp
tinydb::BPlusTree index("accounts.idx", {.buffer_frames = 8});
auto found = index.find(1);         // optional<RecordId>
auto rows = index.range(0, 100);    // [0, 100)
index.close();
```

## Behavior

- `insert(key, rid)` returns false for duplicates; `erase(key)` returns false
  when absent. The raw index does not verify the referenced heap record.
- Bounds are lower-inclusive and upper-exclusive. Omitted bounds cover the full
  key domain; a zero limit returns no entries.
- Node capacity defaults to 184 keys. Capacities 3–184 are supported and persisted;
  an explicit capacity must match on reopen.
- A one-frame pool works because operations hold at most one page guard at a time.

## Structure

Leaves hold sorted key/RID pairs and a next-leaf pointer. Internal separators are
exactly the smallest key in their right subtree. Inserts split full nodes and
may create a root. Deletes borrow or merge, update separators and may collapse
the root. Freed pages return to the allocator.

A shared tree latch allows concurrent readers and excludes mutations. Startup
and `validate()` check ordering, occupancy, parent pointers, equal leaf depth,
reachability, entry counts and the leaf chain.

## Persistence

Raw `sync()`/`close()` flush and synchronize the file, but raw structural changes
are not crash atomic. A failed mutation makes the handle unusable. Use
`TransactionalDatabase` to coordinate heap/index updates and get WAL recovery.
Raw destructors do not flush.

See the [API](../include/index/b_plus_tree.h), [page format](storage-format.md),
and [transactions](transactions.md). Run `./build/index_demo HEAP_FILE INDEX_FILE`
for the independent heap/index example.
