# Transactions

`TransactionalDatabase` coordinates the heap and index as one table. Use a
separate transaction handle for each concurrent client.

```cpp
#include "transaction/database.h"

tinydb::TransactionalDatabase db("accounts");
auto tx = db.begin();
tx.insert({1, "Alice", 1000});
tx.insert({2, "Bob", 500});
tx.commit();
db.close();
```

## API

| Method | Behavior |
| --- | --- |
| `get(key)` | Optional record; missing keys are allowed |
| `get_for_update(key)` | Read while reserving exclusive write access |
| `insert(record)` | Reject duplicate keys |
| `update(key, record)` | Replace an existing record; primary key cannot change |
| `erase(key)` | Delete an existing record |
| `scan(lower, upper, limit)` | Ordered, half-open range; bounds may be omitted |
| `commit()` / `rollback()` | Finish all operations together |

Destroying an unfinished handle rolls it back. Normal operation errors, including
constraint failures and lock timeouts, abort the whole transaction. There are no
nested transactions or savepoints. `close()`, `sync()` and `validate()` require
all transactions to be finished.

## Locking and durability

Point reads take shared key locks; writes take exclusive key locks. Locks remain
held until completion. Scans take a table lock, including empty ranges, to prevent
phantoms. This provides serializable isolation through this single-table API.
A global writer gate prevents one transaction's physical rollback from overwriting
another writer's work. `get_for_update()` also takes that gate.

Lock waits time out after 1000 ms by default; this resolves deadlocks without a
wait-for graph. Buffer capacity defaults to 64 frames per file. Both are configurable
through `DatabaseOptions`.

Commit forces its WAL record before success, while data pages may remain cached.
Rollback forces ABORT, restores physical before-images and discards cached changes.
An I/O or `TransactionFailed` error makes the coordinator unavailable: reopen for
recovery. A failed commit can have an indeterminate outcome; inspect recovered
state before retrying.

Keep the entire database directory. Raw `HeapFile`/`BPlusTree` calls have independent
semantics. See [recovery](recovery.md) and the [API header](../include/transaction/database.h).
