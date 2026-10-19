# Verification

Recorded on 2026-10-19 with GCC 13.3 and CMake 3.28.3 on Linux.

| Check | Result |
| --- | --- |
| Release CTest | 126/126: 125 GoogleTests and the Python driver suite |
| Python driver | 8 tests passed, including evidence validation |
| ASan/UBSan, new benchmark worker tests only | 2/2 passed |
| Real process-crash scenarios | 16 passed |

Run checks or rebuild with sanitizers:

```bash
scripts/verify.sh
TINYDB_BUILD_DIR="$PWD/build-sanitize" scripts/verify.sh -DTINYDB_SANITIZERS=ON
```

For a local GoogleTest checkout, add `-DTINYDB_GTEST_SOURCE_DIR=/path/to/googletest`.
Under process tracing, LeakSanitizer may require `ASAN_OPTIONS=detect_leaks=0`.
No leak-free or race-detector result is claimed.

The [benchmark report](../benchmarks/report.md) contains raw evidence links.
1K and 10K datasets completed 10,000 lookups per method. Workers through 100
completed 22,700 transfers with zero failures. The 100K and 1M setups timed out
after 90 seconds; their lookups remain incomplete.

See [benchmark instructions](benchmark-validation.md) to rerun or change limits.
