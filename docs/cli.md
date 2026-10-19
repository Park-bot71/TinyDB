# CLI

```bash
./build/tinydb DATABASE_DIRECTORY
./build/tinydb /tmp/fresh-example --batch < examples/accounts.tdb
```

The shell has one fixed `accounts` table: signed 64-bit key, name and integer
balance. `CREATE TABLE accounts` acknowledges that table; it does not create
additional schemas.

## Commands

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

Ranges are half-open `[LOWER, UPPER)`. CHECK validates the database; FLUSH syncs
data without truncating WAL. Both require no active transaction.

Use one command per line. Keywords are case insensitive, but the table name is
`accounts`. Trailing semicolons and `#` comments are allowed. Double-quoted names
support `\"`, `\\`, `\n`, `\r`, `\t` and `\0` escapes.

Commands outside BEGIN autocommit. Inside BEGIN, changes remain provisional until
COMMIT. Syntax errors leave active work intact; engine operation errors abort the
whole transaction. Storage failures stop the session for recovery.

EOF, EXIT and graceful SIGINT/SIGTERM shutdown roll back unfinished work and close.
An operation already executing may finish before a signal is handled. Separate
CLI processes cannot share a directory; concurrent clients use the C++ API.

Exit codes: 0 for success, 1 if any command failed, 2 for invalid executable
arguments, or `128 + signal` after graceful signal shutdown.
