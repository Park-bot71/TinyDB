# Verification

Recorded on 2026-10-19 with GCC 13.3, CMake 3.28.3 and GoogleTest 1.14 on Linux.

| Check | Result |
| --- | --- |
| Release CTest | 126/126 entries passed: 125 GoogleTests plus the Python driver suite |
| Python driver | 8 tests passed, including corrupted evidence and Python `-O` checks |
| ASan/UBSan, new benchmark worker tests | 2/2 passed |
| Real crash matrix | 16 distinct scenarios passed |

Tests cover storage boundaries, B+ tree invariants, transaction rollback,
concurrency, interrupted page writes and restart recovery. Crash tests exec
separate processes, exit without cleanup and verify exact state after restart.

## Reproduce

```bash
scripts/verify.sh
TINYDB_BUILD_DIR="$PWD/build-sanitize" scripts/verify.sh -DTINYDB_SANITIZERS=ON
python3 scripts/validate_benchmarks.py
```

For a GoogleTest source checkout, add
`-DTINYDB_GTEST_SOURCE_DIR=/path/to/googletest` to the build/verification command.
In a process-traced environment, LeakSanitizer may require
`ASAN_OPTIONS=detect_leaks=0`; no leak-free or race-detector result is claimed.
The recorded sanitizer result above applies to the new worker tests, not a new
full-suite sanitizer run.

## Benchmark evidence

The [report](../benchmarks/report.md) and [CSV](../benchmarks/results.csv) retain
measurements, commands, hardware, individual lookup samples and JUnit links.
The latest correctness review preserves the original performance observations.

1K and 10K datasets completed 10,000 lookups per method. All worker counts through
100 passed, totaling 22,700 committed transfers with zero failures. The 100K and
1M setups hit their 90-second limits; their lookups are incomplete, not estimated.
See [benchmark options](benchmark-validation.md) to change those limits.
