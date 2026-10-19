# Interactive and batch CLI

Build with CMake, then run `./build/tinydb DATABASE_DIRECTORY`. On a terminal,
the shell prints `db>` or `tx>` when a transaction is active. Redirected input
automatically suppresses prompts; `--batch` explicitly does the same.

```bash
./build/tinydb /tmp/accounts
./build/tinydb /tmp/new-accounts --batch < examples/accounts.tdb
printf 'SELECT accounts *\nCHECK\nEXIT\n' | ./build/tinydb /tmp/new-accounts --batch
```

The example requires a fresh directory because it inserts keys 1 and 2. Reopen
with SELECT to inspect the persisted Alice 800/Bob 700 balances. The database
opens/reconstructs before accepting commands. Keep its whole directory, including
WAL, manifest and immutable baselines; see [recovery.md](recovery.md).

## Commands

| Command | Behavior |
| --- | --- |
| `CREATE TABLE accounts` | Idempotently acknowledges the existing fixed table |
| `BEGIN` | Start an explicit transaction |
| `COMMIT` | Force its COMMIT record, then release locks |
| `ROLLBACK` | Durably abort and restore all its work |
| `INSERT accounts KEY "NAME" BALANCE` | Insert a unique integer key |
| `UPDATE accounts KEY "NAME" BALANCE` | Replace the name/balance of an existing key |
| `DELETE accounts KEY` | Delete an existing key |
| `SELECT accounts KEY` | Read one key; missing keys produce zero rows |
| `SELECT accounts * [LIMIT N]` | Read rows in key order |
| `RANGE accounts LOWER UPPER [LIMIT]` | Half-open range `[LOWER, UPPER)` |
| `STATS` | WAL records, updates, durable LSN and recovery counters |
| `CHECK` | Validate heap/index/allocator consistency; requires no active transaction |
| `FLUSH` | Synchronize materialized files; requires no active transaction; no WAL truncation |
| `HELP` | Print the grammar |
| `EXIT` / `QUIT` | Roll back unfinished work and close |

`accounts` is the only table, available as soon as a database opens. CREATE is an
idempotent declaration, not dynamic DDL, and requires no active transaction. There
is no catalog, schema customization, DROP, JOIN, SQL parser or multiple-table
support. Each row is a signed 64-bit key, variable-length name and signed 64-bit
integer balance; balance units belong to the application.

Outside BEGIN, each CRUD/query command runs in its own transaction and commits
before printing success/results. Inside BEGIN, successful commands print `OK`
or rows but are provisional until COMMIT; explicit ROLLBACK removes all writes.
Transactions use the engine's strict 2PL and table-wide scan locks. There is one
shell session per process, with exclusive directory ownership; concurrent clients
use the C++ API within one coordinator, not separate CLI processes.

## Input and errors

Use one command per line. Keywords are case insensitive; the table name is exactly
`accounts`. An optional trailing semicolon is supported. `#` starts a comment
outside a quoted name. Commands cannot be combined on a line. Names must use
double quotes, including empty names, and support `\"`, `\\`, `\n`, `\r`, `\t`
and `\0`. Unsupported escapes, unterminated quotes and adjacent tokens are errors.
Keys/balances must fit signed 64-bit integers; limits must fit nonnegative
`size_t`. Numeric tokens cannot be quoted. Output escapes newline, carriage return,
tab, NUL and backslash so one record occupies one line; it is human-readable,
not a CSV/export interchange format.

The parser validates the entire command before executing it. A grammar error
does not alter data or abort an already active transaction. An engine operation
error, such as DuplicateKey, NotFound or PageFull, aborts the entire active
transaction, reports that rollback and clears the session's transaction handle.
Later commands can run normally. Lifecycle/diagnostic misuse is reported without
silently committing or discarding active work. I/O, corruption or indeterminate
transaction errors stop the session; reopen for recovery before deciding to retry.

EOF, EXIT and QUIT explicitly roll back unfinished work and perform checked
close. SIGINT/SIGTERM set a signal-safe flag and wake a polled input pipe, avoiding
a lost signal immediately before a blocking read. A woken input wait or the end
of the current command takes the same shutdown path. A signal during an operation
does not cancel its I/O: an autocommit already in progress may commit. SIGKILL or
process failure instead uses startup recovery. No signal handler calls database,
allocation or stream APIs.

Exit status is 0 for an error-free session, 1 if any command failed (even if later
commands succeeded) or a fatal error occurred, 2 for executable argument misuse,
and `128 + signal` after successful graceful signal shutdown. Diagnostics go to
stderr; rows/acknowledgments go to stdout. Batch errors continue except for fatal
storage failures, and unfinished work is rolled back at the end.
