# TinyDB benchmark and correctness report

Run started: 2026-10-19T01:53:53.561386+05:30 (Asia/Kolkata).

Evidence: [evidence/20261019T015353+0530](evidence/20261019T015353+0530/environment.txt). [Commands](evidence/20261019T015353+0530/commands.txt), [CSV](results.csv).

## Hardware and compilation

```text
Started (Asia/Kolkata): 2026-10-19T01:53:53.561386+05:30
Work directory: /tmp/tinydb-validation-rahht2w1
Python: 3.12.3 (main, Aug 31 2026, 10:18:26) [GCC 13.3.0]
Platform: Linux-6.17.0-35-generic-x86_64-with-glibc2.39

$ lscpu
Architecture:                            x86_64
CPU op-mode(s):                          32-bit, 64-bit
Address sizes:                           48 bits physical, 48 bits virtual
Byte Order:                              Little Endian
CPU(s):                                  12
On-line CPU(s) list:                     0-11
Vendor ID:                               AuthenticAMD
Model name:                              AMD Ryzen 5 7640HS w/ Radeon 760M Graphics
CPU family:                              25
Model:                                   116
Thread(s) per core:                      2
Core(s) per socket:                      6
Socket(s):                               1
Stepping:                                1
Frequency boost:                         enabled
CPU(s) scaling MHz:                      49%
CPU max MHz:                             5017.8340
CPU min MHz:                             409.6200
BogoMIPS:                                8584.35
Flags:                                   fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx mmxext fxsr_opt pdpe1gb rdtscp lm constant_tsc rep_good amd_lbr_v2 nopl xtopology nonstop_tsc cpuid extd_apicid aperfmperf rapl pni pclmulqdq monitor ssse3 fma cx16 sse4_1 sse4_2 x2apic movbe popcnt aes xsave avx f16c rdrand lahf_lm cmp_legacy svm extapic cr8_legacy abm sse4a misalignsse 3dnowprefetch osvw ibs skinit wdt tce topoext perfctr_core perfctr_nb bpext perfctr_llc mwaitx cpuid_fault cpb cat_l3 cdp_l3 hw_pstate ssbd mba perfmon_v2 ibrs ibpb stibp ibrs_enhanced vmmcall fsgsbase bmi1 avx2 smep bmi2 erms invpcid cqm rdt_a avx512f avx512dq rdseed adx smap avx512ifma clflushopt clwb avx512cd sha_ni avx512bw avx512vl xsaveopt xsavec xgetbv1 xsaves cqm_llc cqm_occup_llc cqm_mbm_total cqm_mbm_local user_shstk avx512_bf16 clzero irperf xsaveerptr rdpru wbnoinvd cppc arat npt lbrv svm_lock nrip_save tsc_scale vmcb_clean flushbyasid decodeassists pausefilter pfthreshold vgif x2avic v_spec_ctrl vnmi avx512vbmi umip pku ospke avx512_vbmi2 gfni vaes vpclmulqdq avx512_vnni avx512_bitalg avx512_vpopcntdq rdpid overflow_recov succor smca fsrm flush_l1d amd_lbr_pmc_freeze
Virtualization:                          AMD-V
L1d cache:                               192 KiB (6 instances)
L1i cache:                               192 KiB (6 instances)
L2 cache:                                6 MiB (6 instances)
L3 cache:                                16 MiB (1 instance)
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-11
Vulnerability Gather data sampling:      Not affected
Vulnerability Ghostwrite:                Not affected
Vulnerability Indirect target selection: Not affected
Vulnerability Itlb multihit:             Not affected
Vulnerability L1tf:                      Not affected
Vulnerability Mds:                       Not affected
Vulnerability Meltdown:                  Not affected
Vulnerability Mmio stale data:           Not affected
Vulnerability Old microcode:             Not affected
Vulnerability Reg file data sampling:    Not affected
Vulnerability Retbleed:                  Not affected
Vulnerability Spec rstack overflow:      Mitigation; Safe RET
Vulnerability Spec store bypass:         Mitigation; Speculative Store Bypass disabled via prctl
Vulnerability Spectre v1:                Mitigation; usercopy/swapgs barriers and __user pointer sanitization
Vulnerability Spectre v2:                Mitigation; Enhanced / Automatic IBRS; IBPB conditional; STIBP always-on; PBRSB-eIBRS Not affected; BHI Not affected
Vulnerability Srbds:                     Not affected
Vulnerability Tsa:                       Mitigation; Clear CPU buffers
Vulnerability Tsx async abort:           Not affected
Vulnerability Vmscape:                   Mitigation; IBPB before exit to userspace

$ free -b
               total        used        free      shared  buff/cache   available
Mem:     15903645696  7878922240  1053569024    58535936  7413211136  8024723456
Swap:     2147479552       20480  2147459072

$ df -T /tmp/tinydb-validation-rahht2w1
Filesystem     Type 1K-blocks     Used Available Use% Mounted on
/dev/nvme0n1p5 ext4  57573412 32771872  21844536  61% /tmp

$ uname -a
Linux parth-LOQ-15APH8 6.17.0-35-generic #35~24.04.1-Ubuntu SMP PREEMPT_DYNAMIC Tue May 26 19:30:42 UTC 2 x86_64 x86_64 x86_64 GNU/Linux
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++
CMAKE_CXX_FLAGS:STRING=
CMAKE_CXX_FLAGS_DEBUG:STRING=-g
CMAKE_CXX_FLAGS_MINSIZEREL:STRING=-Os -DNDEBUG
CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG
CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG
TINYDB_SANITIZERS:BOOL=OFF
CMAKE_CXX_FLAGS-ADVANCED:INTERNAL=1
CMAKE_CXX_FLAGS_DEBUG-ADVANCED:INTERNAL=1
CMAKE_CXX_FLAGS_MINSIZEREL-ADVANCED:INTERNAL=1
CMAKE_CXX_FLAGS_RELEASE-ADVANCED:INTERNAL=1
CMAKE_CXX_FLAGS_RELWITHDEBINFO-ADVANCED:INTERNAL=1
c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
Copyright (C) 2023 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

binary_sha256=e2f1a4342972a291dbe64d2e1b4de9028d06cb2a6dbf65f0f874a296666fb0cb

Options: {'output': PosixPath('/home/parth/Desktop/Custom_DB/benchmarks'), 'build_dir': PosixPath('/home/parth/Desktop/Custom_DB/build-validation'), 'gtest_source': PosixPath('/tmp/custom-db-tools/root/usr/src/googletest'), 'work_parent': PosixPath('/tmp'), 'datasets': [1000, 10000, 100000, 1000000], 'workers': [1, 2, 4, 8, 16, 32, 64, 100], 'buffer_frames': [1, 16, 64, 128], 'lookups': 10000, 'transactions': 100, 'frames': 64, 'seed': 20261019, 'setup_timeout': 90, 'lookup_timeout': 240, 'phase_timeout': 180}

```

## Methodology

C++20 Release, sanitizers OFF. All database code/APIs are unchanged by this suite. Fresh real heap/index files are populated through HeapFile.insert and BPlusTree.insert with shuffled unique keys, name=benchmark and balance=key. Raw lookup results have no transaction/crash-atomicity guarantee. Each method independently opens/fully validates the same files, verifies every heap/index binding, and warms the same first 1000 probes. Identical retained key streams are used, in indexed-then-sequential order. OS caches are not dropped; CLOCK capacity is bounded per file. Sequential lookup calls the existing materializing heap.scan API and filters until the key matches; it does not scan a preloaded array. Per-request steady_clock timings include lookup, decoding and record verification. Throughput uses complete loop wall time (including timer/sample bookkeeping); startup, warmup, invariant checks, and writing latency files are outside it. Single measured pass per method; no confidence interval or cold-cache claim. Speedup is reported separately as median sequential/indexed latency and indexed/sequential throughput. The Python driver recomputes medians/counts from retained samples and checks identical probes before accepting a measurement.

Requested datasets: [1000, 10000, 100000, 1000000]; lookups **10000 per method/dataset**; frames/file=64, seed=20261019. Setup limit=90s; lookup limit=240s; other phase limit=180s (zero disables the limit). Timeouts are actual attempted-but-incomplete runs, not unsupported engine capacities and never extrapolated into a metric. No numeric metric is filled for unmeasured work.

## Correctness validation

| Check | Executed | Passed | Failed | Status | Evidence |
| --- | ---: | ---: | ---: | --- | --- |
| ctest | 125 | 125 | 0 | pass | [log/results](evidence/20261019T015353+0530/correctness.xml) |
| ctest_review | 126 | 126 | 0 | pass | [log/results](evidence/20261019T015353+0530/review/correctness.xml) |

Counts refer to CTest entries; a Python driver entry runs its own unit tests. Separate review rows recheck the suite without remeasuring or replacing the performance samples.

## B+ tree: measured comparisons

| Records | Method | Operations | Median latency, µs | Throughput, ops/s | Median speedup | Throughput speedup | Status |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1000 | indexed | 10000 | 89.790 | 11061.613 | 4.227 | 4.262 | pass |
| 1000 | sequential | 10000 | 379.568 | 2595.288 | — | — | pass |
| 10000 | indexed | 10000 | 94.564 | 9289.788 | 71.155 | 62.868 | pass |
| 10000 | sequential | 10000 | 6728.708 | 147.766 | — | — | pass |
| 100000 | indexed | — | — | — | — | — | not_run |
| 100000 | sequential | — | — | — | — | — | not_run |
| 1000000 | indexed | — | — | — | — | — | not_run |
| 1000000 | sequential | — | — | — | — | — | not_run |

## Transaction concurrency

| Workers | Attempted | Successful | Failed | Timeouts | Successful tx/s | Correctness | Status |
| ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 1 | 100 | 100 | 0 | 0 | 521.24 | pass | pass |
| 2 | 200 | 200 | 0 | 0 | 418.30 | pass | pass |
| 4 | 400 | 400 | 0 | 0 | 431.99 | pass | pass |
| 8 | 800 | 800 | 0 | 0 | 356.11 | pass | pass |
| 16 | 1600 | 1600 | 0 | 0 | 436.43 | pass | pass |
| 32 | 3200 | 3200 | 0 | 0 | 437.51 | pass | pass |
| 64 | 6400 | 6400 | 0 | 0 | 411.28 | pass | pass |
| 100 | 10000 | 10000 | 0 | 0 | 195.65 | pass | pass |

Each worker attempts 100 two-record transactions (Alice -= 1, Bob += 1). Threads wait on one latch; timing starts at release and ends after join. No retries. BEGIN, strict locks, reads, two updates and forced COMMIT are included. Failed attempts/timeouts are counted separately. Every worker-count run verifies exact balances against acknowledged successes, conservation of total balance, full tree/heap binding invariants, lock cleanup, then closes/reopens and verifies again outside timing. The engine has a global writer gate: these are contention/correctness measurements, not parallel write scaling. A 30-second lock timeout is used. No transactional cache statistics are claimed because that API does not expose them.

## Buffer pool

| Workload | Records/pages | Frames/file | Heap hits | Heap misses | Heap hit rate | Heap evictions | Index hit rate | Index evictions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| indexed | 1000 | 64 | 10000 | 0 | 100.00% | 0 | 100.00% | 0 |
| sequential | 1000 | 64 | 110000 | 0 | 100.00% | 0 | — | 0 |
| indexed | 10000 | 64 | 6348 | 3652 | 63.48% | 3652 | 93.41% | 1319 |
| sequential | 10000 | 64 | 33 | 1019967 | 0.00% | 1019967 | — | 0 |
| hot16_scan128 | 128 | 1 | 0 | 10000 | 0.00% | 9999 | — |  |
| hot16_scan128 | 128 | 16 | 4992 | 5008 | 49.92% | 4992 | — |  |
| hot16_scan128 | 128 | 64 | 4992 | 5008 | 49.92% | 4944 | — |  |
| hot16_scan128 | 128 | 128 | 9872 | 128 | 98.72% | 0 | — |  |

Lookup counters are deltas over the timed loop, excluding validation/warmup; no index fetches in sequential scans is N/A, not an index miss. The dedicated buffer test allocates 128 real pages and alternates hot-set accesses (16 pages repeated eight times) with a 128-page traversal. It validates every fetched page; counters cover actual BufferPool calls. OS cache hits and allocator I/O are not CLOCK hit statistics.

## Real process crashes

| Distinct scenario | Status | Correctness |
| --- | --- | --- |
| before_commit | pass | pass |
| between_updates | pass | pass |
| commit.after_force | pass | pass |
| acknowledged_no_close | pass | pass |
| data.heap.after_half_write | pass | pass |
| torn_after_commit | pass | pass |
| abort.after_force | pass | pass |
| torn_during_abort | pass | pass |
| wal.begin.after_half_frame | pass | pass |
| wal.update.after_half_frame | pass | pass |
| wal.commit.after_half_frame | pass | pass |
| recovery.after_baseline | pass | pass |
| recovery.redo_page | pass | pass |
| recovery.after_redo | pass | pass |
| recovery.after_half_page | pass | pass |
| recovery.after_publish_heap | pass | pass |

**16 distinct crash scenarios passed** out of 16 reported. Each scenario seeds its own database, execs a child and requires the selected hook to terminate it with _exit(86), without cleanup. The parent reopens twice and checks full exact records, invariants and expected winner/loser outcomes. Recovery-crash cases first force a committed transfer, then crash a second child during reconstruction/publication. An offline pre-recovery read proves a data page remains stale after durable COMMIT. This counts distinct crash points, not repeated restarts, GoogleTest names, or exception injections. It tests process death and partial writes, not device power cuts or media-loss recovery.

## Failed, incomplete and unsupported work

- setup/prepare (rows=100000, workers=1): **timed_out**, process deadline 90s exceeded
- lookup/indexed (rows=100000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported
- lookup/sequential (rows=100000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported
- setup/prepare (rows=1000000, workers=1): **timed_out**, process deadline 90s exceeded
- lookup/indexed (rows=1000000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported
- lookup/sequential (rows=1000000, workers=): **not_run**, dataset preparation did not complete; no latency/throughput/speedup reported

Unsupported features are not benchmarked: parallel writers (single writer gate), dynamic tables/SQL, incremental checkpointing/log recycling, cold-cache device latency and transactional cache counters. A dataset timeout is not evidence that the record count is unsupported. Run with --setup-timeout 0 --lookup-timeout 0 to remove those limits; large scans/materializing heap inserts may take substantial time. Child output/progress logs and every completed lookup's raw key/latency samples are retained in evidence; unfinished scans have no fabricated summaries.

## Strongest verified resume metrics

- Passed **126 automated correctness tests** in the recorded Release build (see JUnit); no sanitizer/race-detector result is inferred.
- Verified **71.16× lower median lookup latency** (94.56 µs indexed) versus materializing heap scans over **10000 records**, with **10000 identical-key lookups per method**, warm OS caches, 64 frames/file.
- Validated **100 concurrent transaction workers**, **10000 committed two-record transfers**, **0 failed attempts**, exact recovered balances and invariants; **195.7 successful tx/s** on this workload (serialized writers).
- Passed **16 distinct real process-crash scenarios**, including durable-COMMIT-before-data-flush and interrupted recovery; verified exact state after two restarts per case.
- Measured CLOCK hit rates of **0.0% at 1 frames** and **98.7% at 128 frames**, with **10000 verified page fetches** per configuration; workload-specific, not a general speedup.

Use the workload qualifiers and measured scope in resume bullets. Do not claim million-record results, linear scaling, production ACID certification, or zero failures for incomplete/unsupported tests. The CSV is the source of truth; this report derives every metric from accepted rows.
