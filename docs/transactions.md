# Transactions

Use `TransactionalDatabase` to update the heap and index together. Give each
concurrent client its own transaction handle.

```cpp
#include "transaction/database.h"

tinydb::TransactionalDatabase db("accounts");
auto tx = db.begin();
tx.insert({1, "Alice", 1000});
tx.commit();
db.close();
```

Transactions support `get`, `get_for_update`, `insert`, `update`, `erase` and
`scan`. Scans use `[lower, upper)` bounds. Keys are unique and cannot be changed
by an update. See the [API header](../include/transaction/database.h).

- `commit()` makes changes durable; `rollback()` cancels them.
- Destroying an unfinished handle rolls it back. Normal operation errors abort
  the whole transaction. There are no savepoints or nested transactions.
- Reads and writes hold locks until completion. Scans lock the table, preventing
  new matching rows. Writers, including `get_for_update()`, run one at a time.
- Defaults are a 1000 ms lock timeout and 64 buffer frames per file, configurable
  through `DatabaseOptions`.

Finish transactions before `close()`, `sync()` or `validate()`. After an I/O or
`TransactionFailed` error, reopen for [recovery](recovery.md). A failed commit
may already be durable: inspect recovered state before retrying.
