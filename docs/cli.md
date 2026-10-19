# CLI

```bash
./build/tinydb DATABASE_DIRECTORY
./build/tinydb /tmp/fresh-example --batch < examples/accounts.tdb
```

There is one fixed table, `accounts`, with an integer key, name and integer
balance. `CREATE TABLE accounts` acknowledges it rather than creating a schema.

```text
CREATE TABLE accounts
BEGIN | COMMIT | ROLLBACK
INSERT accounts KEY "NAME" BALANCE
UPDATE accounts KEY "NAME" BALANCE
DELETE accounts KEY
SELECT accounts KEY
SELECT accounts * [LIMIT N]
RANGE accounts LOWER UPPER [LIMIT]
STATS | CHECK | FLUSH | HELP | EXIT | QUIT
```

Use one command per line. Keywords ignore case; the table name is `accounts`.
Trailing semicolons and `#` comments are allowed. Ranges use `[LOWER, UPPER)`.

Commands outside `BEGIN` commit automatically. Syntax errors leave an active
transaction intact; operation errors abort it. Storage failures stop the session.
`CHECK` and `FLUSH` require no active transaction; flushing does not trim WAL.

EOF, EXIT and graceful shutdown roll back unfinished work. An executing operation
may finish before shutdown. Only one CLI process may open a directory; concurrent
clients use the [C++ API](transactions.md).

Exit codes: 0 success, 1 command error, 2 bad arguments, or `128 + signal`.
