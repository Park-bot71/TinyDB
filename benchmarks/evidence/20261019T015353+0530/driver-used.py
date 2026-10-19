#!/usr/bin/env python3
"""Run real C++ benchmark processes, retain evidence, and render measured results.

Zero timeout disables that limit. A timeout never becomes a successful metric.
No database files are synthesized, copied into a scan cache, or rewritten here.
"""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
FIELDS = "category,name,dataset_size,workers,capacity_per_file,seed,status,correctness,operations,successes,failures,timeouts,seconds,wall_seconds,median_latency_us,throughput_ops_s,median_latency_speedup,throughput_speedup,heap_hits,heap_misses,heap_hit_rate,heap_evictions,index_hits,index_misses,index_hit_rate,index_evictions,details,evidence".split(",")


def command_text(command):
    return shlex.join(map(str, command))


def execute(command, log, seconds=0):
    """Isolate a complete process group, including crash-test grandchildren."""
    started = time.perf_counter()
    with log.open("w") as output:
        process = subprocess.Popen(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            code = process.wait(timeout=seconds or None)
            timeout = False
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            code = process.wait()
            timeout = True
    return code, timeout, time.perf_counter() - started


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Suite:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(ZoneInfo("Asia/Kolkata")).strftime("%Y%m%dT%H%M%S%z")
        self.evidence = self.output / "evidence" / stamp
        self.evidence.mkdir(parents=True, exist_ok=False)
        self.work = Path(tempfile.mkdtemp(prefix="tinydb-validation-", dir=args.work_parent)).resolve()
        self.binary = args.build_dir.resolve() / "tinydb_validation"
        self.rows = []
        self.commands = []
        self.started = datetime.now(ZoneInfo("Asia/Kolkata")).isoformat()

    def persist(self):
        temporary = self.output / "results.csv.tmp"
        with temporary.open("w", newline="") as destination:
            writer = csv.DictWriter(destination, fieldnames=FIELDS, extrasaction="raise")
            writer.writeheader()
            writer.writerows(self.rows)
        temporary.replace(self.output / "results.csv")

    def add(self, row):
        self.rows.append(row)
        self.persist()
        print(f'{row["category"]}/{row["name"]} rows={row.get("dataset_size", "")} workers={row.get("workers", "")}: {row["status"]}', flush=True)

    def stage(self, phase, path, *, rows=2, workers=1, frames=None, method=None, timeout=None):
        frames = self.args.frames if frames is None else frames
        name = method or phase
        key = f"{phase}-{rows}-{workers}-{frames}-{name}"
        log = self.evidence / f"{key}.log"
        command = [self.binary, "--phase", phase, "--dir", path, "--rows", rows,
                   "--lookups", self.args.lookups, "--workers", workers, "--frames", frames,
                   "--transactions", self.args.transactions, "--seed", self.args.seed]
        if method:
            command += ["--method", method]
        self.commands.append(command_text(command))
        code, expired, wall = execute(command, log, self.args.phase_timeout if timeout is None else timeout)
        context = dict(category={"prepare": "setup", "lookup": "lookup", "concurrency": "concurrency", "buffer": "buffer", "crashes": "crash"}[phase],
                       name=name, dataset_size=str(rows), workers=str(workers), capacity_per_file=str(frames), seed=str(self.args.seed))
        parsed = []
        for line in log.read_text(errors="replace").splitlines():
            if line.startswith("{"):
                try:
                    parsed.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        if expired or not parsed:
            row = context | dict(status="timed_out" if expired else "fail", correctness="not_completed",
                                 wall_seconds=wall, details=f"process deadline {timeout if timeout is not None else self.args.phase_timeout}s exceeded" if expired else f"worker returned {code} without results",
                                 evidence=str(log.relative_to(self.output)))
            self.add(row)
            return None
        for record in parsed:
            row = context | record | dict(wall_seconds=wall, evidence=str(log.relative_to(self.output)))
            if phase != "crashes" and code != 0:
                row.update(status="fail", correctness="fail", details=f"exit {code}: {row.get('details', '')}")
            if row.get("status") == "pass" and phase == "lookup":
                try:
                    self.verify_samples(path, method, row)
                except (ValueError, OSError, KeyError, AssertionError) as error:
                    row.update(status="fail", correctness="fail", details=f"latency/probe evidence verification failed: {error}")
            self.add(row)
        if phase == "crashes":
            names = [record.get("name") for record in parsed]
            if len(names) != 16 or len(set(names)) != 16 or code not in (0, 1):
                self.add(context | dict(status="fail", correctness="not_completed", details="incomplete/duplicate crash scenario protocol", evidence=str(log.relative_to(self.output))))
        return self.rows[-1] if phase != "crashes" else None

    def verify_samples(self, path, method, row):
        with (path / f"{method}-latencies.csv").open(newline="") as source:
            samples = list(csv.DictReader(source))
        keys = (path / "keys.txt").read_text().splitlines()
        assert len(samples) == len(keys) == self.args.lookups == int(row["operations"]), "lookup count mismatch"
        assert all(sample["key"] == key for sample, key in zip(samples, keys)), "method used different keys"
        values = [float(sample["latency_us"]) for sample in samples]
        assert all(math.isfinite(value) and value > 0 for value in values), "invalid latency sample"
        assert math.isclose(statistics.median(values), float(row["median_latency_us"]), rel_tol=1e-10), "median mismatch"
        assert math.isclose(len(samples) / float(row["seconds"]), float(row["throughput_ops_s"]), rel_tol=1e-10), "throughput mismatch"
        destination = self.evidence / f"dataset-{row['dataset_size']}"
        destination.mkdir(exist_ok=True)
        for name in ("keys.txt", f"{method}-latencies.csv"):
            (destination / name).write_bytes((path / name).read_bytes())
        row["details"] = f"1000-probe warmup; OS cache not dropped; keys_sha256={sha(path / 'keys.txt')}; latency samples retained"

    def build_and_test(self):
        configure = ["cmake", "-S", ROOT, "-B", self.args.build_dir, "-DCMAKE_BUILD_TYPE=Release",
                     "-DBUILD_TESTING=ON", "-DTINYDB_SANITIZERS=OFF"]
        if self.args.gtest_source:
            configure += [f"-DTINYDB_GTEST_SOURCE_DIR={self.args.gtest_source.resolve()}"]
        for name, command in [("configure", configure), ("build", ["cmake", "--build", self.args.build_dir, "--parallel", "2"])]:
            self.commands.append(command_text(command))
            code, expired, wall = execute(command, self.evidence / f"{name}.log", 600)
            if code != 0:
                self.add(dict(category="correctness", name=name, status="timed_out" if expired else "fail", correctness="not_completed", wall_seconds=wall,
                              details=f"exit {code}; see build log", evidence=str((self.evidence / f"{name}.log").relative_to(self.output))))
                return False
        junit = self.evidence / "correctness.xml"
        command = ["ctest", "--test-dir", self.args.build_dir, "--output-on-failure", "--output-junit", junit]
        self.commands.append(command_text(command))
        code, expired, wall = execute(command, self.evidence / "correctness.log", 600)
        cases = ET.parse(junit).findall(".//testcase") if junit.exists() else []
        failed = [case.get("name", "unknown") for case in cases if case.find("failure") is not None or case.find("error") is not None]
        skipped = [case for case in cases if case.find("skipped") is not None]
        passed = len(cases) - len(failed) - len(skipped)
        okay = code == 0 and bool(cases) and not skipped and not failed
        self.add(dict(category="correctness", name="ctest", status="pass" if okay else "timed_out" if expired else "fail",
                      correctness="pass" if okay else "fail", operations=len(cases), successes=passed, failures=len(failed), wall_seconds=wall,
                      details=f"failed: {', '.join(failed) or 'none'}; skipped={len(skipped)}; exit={code}", evidence=str(junit.relative_to(self.output))))
        return okay

    def metadata(self):
        output = self.evidence / "environment.txt"
        with output.open("w") as destination:
            destination.write(f"Started (Asia/Kolkata): {self.started}\nWork directory: {self.work}\nPython: {sys.version}\nPlatform: {platform.platform()}\n")
            for command in (["lscpu"], ["free", "-b"], ["df", "-T", self.work], ["uname", "-a"]):
                destination.write(f"\n$ {command_text(command)}\n"); destination.flush()
                subprocess.run(list(map(str, command)), stdout=destination, stderr=subprocess.STDOUT, check=False)
            cache = self.args.build_dir / "CMakeCache.txt"
            if cache.exists():
                compiler = None
                for line in cache.read_text().splitlines():
                    if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_FLAGS", "CMAKE_CXX_COMPILER:", "TINYDB_SANITIZERS:")):
                        destination.write(line + "\n")
                    if line.startswith("CMAKE_CXX_COMPILER:FILEPATH="):
                        compiler = line.split("=", 1)[1]
                destination.flush()
                if compiler:
                    subprocess.run([compiler, "--version"], stdout=destination, stderr=subprocess.STDOUT, check=False)
            if self.binary.exists():
                destination.write(f"binary_sha256={sha(self.binary)}\n")
            destination.write(f"\nOptions: {vars(self.args)}\n")
        (self.evidence / "commands.txt").write_text("\n".join(self.commands) + "\n")
        sources = [ROOT / "CMakeLists.txt", Path(__file__).resolve(), *sorted((ROOT / "include").rglob("*.h")), *sorted((ROOT / "src").rglob("*.cpp")), *sorted((ROOT / "benchmarks").glob("*.cpp"))]
        (self.evidence / "source-sha256.txt").write_text("".join(f"{sha(path)}  {path.relative_to(ROOT)}\n" for path in sources))

    def run(self):
        try:
            if not self.build_and_test():
                return
            for size in self.args.datasets:
                path = self.work / f"dataset-{size}"
                prepared = self.stage("prepare", path, rows=size, timeout=self.args.setup_timeout)
                if prepared and prepared["status"] == "pass":
                    measured = {}
                    for method in ("indexed", "sequential"):
                        measured[method] = self.stage("lookup", path, rows=size, method=method, timeout=self.args.lookup_timeout)
                    if all(row and row["status"] == "pass" for row in measured.values()):
                        indexed, scan = measured["indexed"], measured["sequential"]
                        indexed["median_latency_speedup"] = float(scan["median_latency_us"]) / float(indexed["median_latency_us"])
                        indexed["throughput_speedup"] = float(indexed["throughput_ops_s"]) / float(scan["throughput_ops_s"])
                        self.persist()
                else:
                    for method in ("indexed", "sequential"):
                        self.add(dict(category="lookup", name=method, dataset_size=size, capacity_per_file=self.args.frames,
                                      status="not_run", correctness="not_completed", details="dataset preparation did not complete; no latency/throughput/speedup reported"))
            for workers in self.args.workers:
                self.stage("concurrency", self.work / f"workers-{workers}", workers=workers)
            for frames in self.args.buffer_frames:
                self.stage("buffer", self.work / f"buffer-{frames}", rows=128, frames=frames)
            self.stage("crashes", self.work / "crashes", frames=1)
        finally:
            self.metadata()
            self.report()

    def report(self):
        # Only measured passing results can supply resume metrics or ratios.
        good = [row for row in self.rows if row["status"] == "pass" and row.get("correctness") == "pass"]
        lines = ["# TinyDB benchmark and correctness report", "", f"Run started: {self.started} (Asia/Kolkata).", "",
                 f"Evidence: [{self.evidence.relative_to(self.output)}]({self.evidence.relative_to(self.output)}/environment.txt). "
                 f"[Commands]({self.evidence.relative_to(self.output)}/commands.txt), [CSV](results.csv).", "",
                 "## Hardware and compilation", "", "```text", (self.evidence / "environment.txt").read_text(), "```", "",
                 "## Methodology", "",
                 "C++20 Release, sanitizers OFF. All database code/APIs are unchanged by this suite. "
                 "Fresh real heap/index files are populated through HeapFile.insert and BPlusTree.insert with shuffled unique keys, "
                 "name=benchmark and balance=key. Raw lookup results have no transaction/crash-atomicity guarantee. "
                 "Each method independently opens/fully validates the same files, verifies every heap/index binding, and warms the same first 1000 probes. "
                 "Identical retained key streams are used, in indexed-then-sequential order. OS caches are not dropped; CLOCK capacity is bounded per file. "
                 "Sequential lookup calls the existing materializing heap.scan API and filters until the key matches; it does not scan a preloaded array. "
                 "Per-request steady_clock timings include lookup, decoding and record verification. Throughput uses complete loop wall time (including timer/sample bookkeeping); "
                 "startup, warmup, invariant checks, and writing latency files are outside it. Single measured pass per method; no confidence interval or cold-cache claim. "
                 "Speedup is reported separately as median sequential/indexed latency and indexed/sequential throughput. "
                 "The Python driver recomputes medians/counts from retained samples and checks identical probes before accepting a measurement.", "",
                 f"Requested datasets: {self.args.datasets}; lookups **{self.args.lookups} per method/dataset**; "
                 f"frames/file={self.args.frames}, seed={self.args.seed}. Setup limit={self.args.setup_timeout}s; lookup limit={self.args.lookup_timeout}s; "
                 f"other phase limit={self.args.phase_timeout}s (zero disables the limit). Timeouts are actual attempted-but-incomplete runs, "
                 "not unsupported engine capacities and never extrapolated into a metric. No numeric metric is filled for unmeasured work.", "",
                 "## B+ tree: measured comparisons", "", "| Records | Method | Operations | Median latency, µs | Throughput, ops/s | Median speedup | Throughput speedup | Status |",
                 "| ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |"]
        for row in self.rows:
            if row["category"] == "lookup":
                def numeric(name, precision=3):
                    return f"{float(row[name]):.{precision}f}" if row.get(name) not in (None, "") else "—"
                lines.append(f"| {row.get('dataset_size')} | {row['name']} | {row.get('operations', '—')} | {numeric('median_latency_us')} | {numeric('throughput_ops_s')} | {numeric('median_latency_speedup')} | {numeric('throughput_speedup')} | {row['status']} |")
        lines += ["", "## Transaction concurrency", "", "| Workers | Attempted | Successful | Failed | Timeouts | Successful tx/s | Correctness | Status |",
                  "| ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"]
        for row in self.rows:
            if row["category"] == "concurrency":
                rate = f"{float(row['throughput_ops_s']):.2f}" if row.get("throughput_ops_s") else "—"
                lines.append(f"| {row.get('workers')} | {row.get('operations', '—')} | {row.get('successes', '—')} | {row.get('failures', '—')} | {row.get('timeouts', '—')} | {rate} | {row.get('correctness')} | {row['status']} |")
        lines += ["", f"Each worker attempts {self.args.transactions} two-record transactions (Alice -= 1, Bob += 1). "
                  "Threads wait on one latch; timing starts at release and ends after join. No retries. "
                  "BEGIN, strict locks, reads, two updates and forced COMMIT are included. Failed attempts/timeouts are counted separately. "
                  "Every worker-count run verifies exact balances against acknowledged successes, conservation of total balance, "
                  "full tree/heap binding invariants, lock cleanup, then closes/reopens and verifies again outside timing. "
                  "The engine has a global writer gate: these are contention/correctness measurements, not parallel write scaling. "
                  "A 30-second lock timeout is used. No transactional cache statistics are claimed because that API does not expose them.", "",
                  "## Buffer pool", "", "| Workload | Records/pages | Frames/file | Heap hits | Heap misses | Heap hit rate | Heap evictions | Index hit rate | Index evictions |",
                  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for row in good:
            if row["category"] in ("lookup", "buffer"):
                rate = lambda name: f"{float(row[name]):.2%}" if row.get(name) else "—"
                lines.append(f"| {row['name']} | {row.get('dataset_size')} | {row.get('capacity_per_file')} | {row.get('heap_hits', '—')} | {row.get('heap_misses', '—')} | {rate('heap_hit_rate')} | {row.get('heap_evictions', '—')} | {rate('index_hit_rate')} | {row.get('index_evictions', '—')} |")
        lines += ["", "Lookup counters are deltas over the timed loop, excluding validation/warmup; no index fetches in sequential scans is N/A, not an index miss. "
                  "The dedicated buffer test allocates 128 real pages and alternates hot-set accesses (16 pages repeated eight times) with a 128-page traversal. "
                  "It validates every fetched page; counters cover actual BufferPool calls. OS cache hits and allocator I/O are not CLOCK hit statistics.", "",
                  "## Real process crashes", "", "| Distinct scenario | Status | Correctness |", "| --- | --- | --- |"]
        crashes = [row for row in self.rows if row["category"] == "crash"]
        for row in crashes:
            lines.append(f"| {row['name']} | {row['status']} | {row.get('correctness')} |")
        passed = len({row["name"] for row in crashes if row["status"] == "pass"})
        lines += ["", f"**{passed} distinct crash scenarios passed** out of {len({row['name'] for row in crashes})} reported. "
                  "Each scenario seeds its own database, execs a child and requires the selected hook to terminate it with _exit(86), without cleanup. "
                  "The parent reopens twice and checks full exact records, invariants and expected winner/loser outcomes. "
                  "Recovery-crash cases first force a committed transfer, then crash a second child during reconstruction/publication. "
                  "An offline pre-recovery read proves a data page remains stale after durable COMMIT. "
                  "This counts distinct crash points, not repeated restarts, GoogleTest names, or exception injections. "
                  "It tests process death and partial writes, not device power cuts or media-loss recovery.", "",
                  "## Failed, incomplete and unsupported work", ""]
        bad = [row for row in self.rows if row["status"] != "pass"]
        lines += [f"- {row['category']}/{row['name']} (rows={row.get('dataset_size', '—')}, workers={row.get('workers', '—')}): **{row['status']}**, {row.get('details', '')}" for row in bad] or ["None in this run."]
        lines += ["", "Unsupported features are not benchmarked: parallel writers (single writer gate), dynamic tables/SQL, incremental checkpointing/log recycling, "
                  "cold-cache device latency and transactional cache counters. A dataset timeout is not evidence that the record count is unsupported. "
                  "Run with --setup-timeout 0 --lookup-timeout 0 to remove those limits; large scans/materializing heap inserts may take substantial time. "
                  "Child output/progress logs and every completed lookup's raw key/latency samples are retained in evidence; unfinished scans have no fabricated summaries.", "",
                  "## Strongest verified resume metrics", ""]
        core = next((row for row in good if row["category"] == "correctness" and row["name"] == "ctest"), None)
        if core:
            lines.append(f"- Passed **{core['successes']} automated correctness tests** in the recorded Release build (see JUnit); no sanitizer/race-detector result is inferred.")
        pairs = [row for row in good if row.get("median_latency_speedup")]
        if pairs:
            best = max(pairs, key=lambda row: int(row["dataset_size"]))
            lines.append(f"- Verified **{float(best['median_latency_speedup']):.2f}× lower median lookup latency** ({float(best['median_latency_us']):.2f} µs indexed) versus materializing heap scans over **{best['dataset_size']} records**, with **{best['operations']} identical-key lookups per method**, warm OS caches, {best['capacity_per_file']} frames/file.")
        concurrent = [row for row in good if row["category"] == "concurrency" and int(row.get("failures", 1)) == 0]
        if concurrent:
            largest = max(concurrent, key=lambda row: int(row["workers"]))
            lines.append(f"- Validated **{largest['workers']} concurrent transaction workers**, **{largest['successes']} committed two-record transfers**, **0 failed attempts**, exact recovered balances and invariants; **{float(largest['throughput_ops_s']):.1f} successful tx/s** on this workload (serialized writers).")
        if passed:
            lines.append(f"- Passed **{passed} distinct real process-crash scenarios**, including durable-COMMIT-before-data-flush and interrupted recovery; verified exact state after two restarts per case.")
        cache_rows = [row for row in good if row["category"] == "buffer"]
        if len(cache_rows) >= 2:
            low = min(cache_rows, key=lambda row: int(row["capacity_per_file"]))
            high = max(cache_rows, key=lambda row: int(row["capacity_per_file"]))
            lines.append(f"- Measured CLOCK hit rates of **{float(low['heap_hit_rate']):.1%} at {low['capacity_per_file']} frames** and **{float(high['heap_hit_rate']):.1%} at {high['capacity_per_file']} frames**, with **{low['operations']} verified page fetches** per configuration; workload-specific, not a general speedup.")
        lines += ["", "Use the workload qualifiers and measured scope in resume bullets. Do not claim million-record results, linear scaling, "
                  "production ACID certification, or zero failures for incomplete/unsupported tests. The CSV is the source of truth; this report derives every metric from accepted rows.", ""]
        (self.output / "report.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "benchmarks")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-validation")
    parser.add_argument("--gtest-source", type=Path)
    parser.add_argument("--work-parent", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--datasets", type=int, nargs="+", default=[1000, 10000, 100000, 1000000])
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 2, 4, 8, 16, 32, 64, 100])
    parser.add_argument("--buffer-frames", type=int, nargs="+", default=[1, 16, 64, 128])
    parser.add_argument("--lookups", type=int, default=10000)
    parser.add_argument("--transactions", type=int, default=100)
    parser.add_argument("--frames", type=int, default=64)
    parser.add_argument("--seed", type=int, default=20261019)
    parser.add_argument("--setup-timeout", type=float, default=90)
    parser.add_argument("--lookup-timeout", type=float, default=240)
    parser.add_argument("--phase-timeout", type=float, default=180)
    args = parser.parse_args()
    if args.lookups < 10000 or args.lookups > 1000000 or not 1 <= args.transactions <= 100000 or not 1 <= args.frames <= 65536:
        parser.error("lookups must be 10000..1000000; positive bounded transaction/frame counts required")
    if any(not 1 <= size <= 1000000 for size in args.datasets) or any(not 1 <= size <= 1000 for size in args.workers) or any(not 1 <= size <= 65536 for size in args.buffer_frames):
        parser.error("invalid dataset/worker/buffer counts")
    if any(not math.isfinite(value) or value < 0 for value in (args.setup_timeout, args.lookup_timeout, args.phase_timeout)):
        parser.error("timeouts must be finite and nonnegative")
    if any(len(set(values)) != len(values) for values in (args.datasets, args.workers, args.buffer_frames)):
        parser.error("duplicate workload sizes are not allowed")
    args.build_dir = args.build_dir.resolve()
    suite = Suite(args)
    suite.run()
    if any(row["status"] in ("fail", "completed_with_failures") for row in suite.rows):
        return 1
    return 2 if any(row["status"] != "pass" for row in suite.rows) else 0


if __name__ == "__main__":
    sys.exit(main())
