# B+ tree index

`BPlusTree` maps unique signed 64-bit keys to heap record IDs in a persistent
file. It supports `find`, `insert`, `erase` and ordered `range` scans.

- Duplicate inserts and missing deletes return false. The raw index does not
  check whether a referenced heap record exists.
- Ranges include the lower bound and exclude the upper bound. Omitted bounds
  scan the full key space.
- Nodes split on insert and borrow or merge on delete. Capacity defaults to
  184 keys; supported values are 3–184 and are saved in the file.
- Readers share a tree lock; changes take exclusive access. Startup and
  `validate()` check ordering, occupancy, links and entry counts.

Call `sync()` or `close()` to save raw index changes; destructors do not flush.
Raw mutations are not crash atomic. Use [transactions](transactions.md) for
consistent heap/index updates and recovery.

See the [API](../include/index/b_plus_tree.h) or run
`./build/index_demo HEAP_FILE INDEX_FILE`.
