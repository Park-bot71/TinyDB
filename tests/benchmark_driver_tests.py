"""Negative-path tests for the measurement evidence checks; no recorded metrics.

Usage: python3 tests/benchmark_driver_tests.py /path/to/tinydb_validation
Test samples come from an actual small C++ workload in a temporary directory.
"""
import argparse
import csv
import importlib.util
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("benchmark_driver", ROOT / "scripts/validate_benchmarks.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)
WORKER = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / "build-validation/tinydb_validation"


class BenchmarkDriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="tinydb-driver-tests-")
        cls.root = Path(cls.temporary.name)
        cls.data = cls.root / "actual-data"
        for phase in ("prepare", "lookup"):
            result = subprocess.run([str(WORKER), "--phase", phase, "--dir", str(cls.data),
                                     "--rows", "64", "--lookups", "32", "--frames", "1"],
                                    text=True, capture_output=True, check=True)
        cls.measured = json.loads(result.stdout)
        result = subprocess.run([str(WORKER), "--phase", "crashes", "--dir", str(cls.root / "crashes")],
                                text=True, capture_output=True, check=True)
        cls.crashes = [json.loads(line) for line in result.stdout.splitlines()]

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.scratch = Path(tempfile.mkdtemp(dir=self.root))
        self.data_copy = self.scratch / "samples"
        self.data_copy.mkdir()
        for name in ("keys.txt", "indexed-latencies.csv"):
            shutil.copyfile(self.data / name, self.data_copy / name)
        self.suite = driver.Suite.__new__(driver.Suite)
        self.suite.args = argparse.Namespace(lookups=32, datasets=[64], frames=1, seed=20261019,
                                            setup_timeout=90, lookup_timeout=240, phase_timeout=180, transactions=4)
        self.suite.output = self.scratch
        self.suite.evidence = self.scratch / "evidence"
        self.suite.evidence.mkdir()

    def test_accepts_real_worker_samples(self):
        row = dict(self.measured)
        self.suite.verify_samples(self.data_copy, "indexed", row)
        self.assertIn("keys_sha256=", row["details"])

    def test_rejects_tampered_keys_and_missing_samples(self):
        path = self.data_copy / "indexed-latencies.csv"
        original = path.read_text().splitlines()
        changed = original.copy()
        key, latency = changed[1].split(",")
        changed[1] = f"{int(key) + 1},{latency}"
        path.write_text("\n".join(changed) + "\n")
        with self.assertRaisesRegex(ValueError, "different keys"):
            self.suite.verify_samples(self.data_copy, "indexed", dict(self.measured))
        path.write_text("\n".join(original[:-1]) + "\n")
        with self.assertRaisesRegex(ValueError, "count mismatch"):
            self.suite.verify_samples(self.data_copy, "indexed", dict(self.measured))

    def test_rejects_incorrect_median_and_throughput(self):
        for field, message in (("median_latency_us", "median mismatch"), ("throughput_ops_s", "throughput mismatch")):
            row = dict(self.measured)
            row[field] = float(row[field]) * 2
            with self.assertRaisesRegex(ValueError, message):
                self.suite.verify_samples(self.data_copy, "indexed", row)

    def saved_fixture(self):
        row = dict.fromkeys(driver.FIELDS, "") | self.measured
        row["evidence"] = "evidence/worker.log"
        self.suite.verify_samples(self.data_copy, "indexed", row)
        (self.suite.evidence / "worker.log").write_text(json.dumps(self.measured) + "\n")
        (self.suite.evidence / "run-config.json").write_text(json.dumps(vars(self.suite.args)))
        (self.suite.evidence / "environment.txt").write_text("Started (Asia/Kolkata): 2026-10-19T00:00:00+05:30\n")
        self.suite.rows = [row]
        return row

    def test_saved_measurements_match_both_worker_log_and_samples(self):
        row = self.saved_fixture()
        driver.check_saved(self.suite)
        row["median_latency_us"] = str(float(row["median_latency_us"]) * 2)
        with self.assertRaisesRegex(ValueError, "raw worker mismatch"):
            driver.check_saved(self.suite)
        # Altering the CSV and log together must still fail the sample check.
        (self.suite.evidence / "worker.log").write_text(json.dumps({key: row[key] for key in self.measured}) + "\n")
        with self.assertRaisesRegex(ValueError, "median mismatch"):
            driver.check_saved(self.suite)

    def test_render_rejects_altered_csv_without_replacing_report(self):
        row = self.saved_fixture()
        row["throughput_ops_s"] = str(float(row["throughput_ops_s"]) * 2)
        with (self.scratch / "results.csv").open("w", newline="") as destination:
            writer = csv.DictWriter(destination, fieldnames=driver.FIELDS)
            writer.writeheader(); writer.writerow(row)
        report = self.scratch / "report.md"
        report.write_text("original verified report")
        with self.assertRaisesRegex(ValueError, "raw worker mismatch"):
            driver.render_saved(self.scratch, self.suite.evidence)
        self.assertEqual(report.read_text(), "original verified report")

    def test_checks_stay_enabled_under_python_optimization(self):
        row = dict(self.measured)
        row["median_latency_us"] = str(float(row["median_latency_us"]) * 2)
        program = """import importlib.util,json,sys
from pathlib import Path
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('driver',sys.argv[1])
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
module.check_samples(Path(sys.argv[2]),32,json.load(sys.stdin))
"""
        result = subprocess.run([sys.executable, "-O", "-c", program, str(ROOT / "scripts/validate_benchmarks.py"), str(self.data_copy)],
                                input=json.dumps(row), text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("median mismatch", result.stderr)

    def test_crash_result_counts_and_exit_status_must_agree(self):
        driver.check_crash_protocol(self.crashes, 0)
        with self.assertRaisesRegex(ValueError, "exit status"):
            driver.check_crash_protocol(self.crashes, 1)
        with self.assertRaisesRegex(ValueError, "incomplete/duplicate"):
            driver.check_crash_protocol(self.crashes[:-1], 0)
        with self.assertRaisesRegex(ValueError, "incomplete/duplicate"):
            driver.check_crash_protocol(self.crashes[:-1] + self.crashes[:1], 0)
        invalid = [dict(record) for record in self.crashes]
        invalid[0]["correctness"] = "fail"
        with self.assertRaisesRegex(ValueError, "correctness state"):
            driver.check_crash_protocol(invalid, 0)

    def test_real_process_deadline_is_not_success(self):
        code, expired, _ = driver.execute([sys.executable, "-c", "import time; time.sleep(30)"], self.scratch / "timeout.log", 0.03)
        self.assertTrue(expired)
        self.assertEqual(code, -signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
