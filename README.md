# TinyDB

A disk-backed transactional database engine built from scratch in **C++20** for
Linux. TinyDB stores records in custom binary files and supports persistent
indexing, concurrent transactions, and recovery after process crashes.

## Features

- **Storage:** 4 KB pages, variable-length records, checksums, and page reuse.
- **Buffer pool:** bounded CLOCK cache with page guards and dirty-page tracking.
- **B+ tree:** persistent integer-key index with inserts, deletes, and range scans.
- **Transactions:** BEGIN, COMMIT, and ROLLBACK with strict two-phase locking.
- **Recovery:** write-ahead logging, durable commits, and restart-safe recovery.
- **Tools:** interactive CLI, automated tests, and reproducible benchmarks.

## Build and test

Requires Linux, a C++20 compiler, CMake 3.20+, and GoogleTest. On Ubuntu/Debian:

```bash
sudo apt-get install build-essential cmake libgtest-dev python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

The engine uses the C++ standard library and Linux/POSIX APIs. GoogleTest is for
tests; Python 3 is for the benchmark tools. Additional build and sanitizer
instructions are in [verification.md](docs/verification.md).

## Try it

Start the CLI with a new database directory:

```bash
./build/tinydb /tmp/tinydb-demo
```

Then enter:

```text
CREATE TABLE accounts
BEGIN
INSERT accounts 1 "Alice" 1000
INSERT accounts 2 "Bob" 500
COMMIT
SELECT accounts *
EXIT
```

Reopen the same directory to read the saved records. Commands outside BEGIN
commit automatically; ROLLBACK cancels an active transaction. Type HELP for all
commands, or see the [CLI guide](docs/cli.md).

For a scripted transfer and rollback demo, use a fresh directory:

```bash
./build/tinydb /tmp/tinydb-example --batch < examples/accounts.tdb
```

## Benchmarks

```bash
python3 scripts/validate_benchmarks.py
```

The suite measures indexed versus sequential lookup, concurrent transactions,
buffer-pool behavior, and real process-crash recovery. It saves hardware details,
raw evidence, and results in [benchmarks/report.md](benchmarks/report.md) and
[benchmarks/results.csv](benchmarks/results.csv).

The recorded run completed 1K and 10K lookup datasets, worker counts through 100,
and 16 distinct crash scenarios. The 100K and 1M setups exceeded their time limits
and are marked incomplete. See the [methodology and run options](docs/benchmark-validation.md).

## Scope and limitations

TinyDB currently has one fixed-schema `accounts` table: integer ID, name, and
integer balance. Writers are serialized; SQL and multiple tables are not supported.
Recovery retains the full WAL and an immutable baseline, so log size and startup
cost grow with history. Keep the whole database directory, including recovery files.

This is an educational database project. Recovery assumes a local Linux filesystem
that honors `fsync`; it does not cover arbitrary media loss. Read the
[recovery guide](docs/recovery.md) for the tested guarantees and limits.

## Learn more

- [Architecture](docs/architecture.md) and [storage format](docs/storage-format.md)
- [Buffer pool](docs/buffer-pool.md) and [B+ tree](docs/b-plus-tree.md)
- [Transaction API](include/transaction/database.h) and [locking semantics](docs/transactions.md)
- [Test results](docs/verification.md) and [benchmark report](benchmarks/report.md)
