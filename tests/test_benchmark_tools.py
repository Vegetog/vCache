"""Validate benchmark statistics and failure records to prevent incomplete runs from appearing successful."""

import argparse
import csv
import io
import math
import os
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("vcache_benchmark_runner", ROOT / "benchmark/run.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
BINARY = ROOT / "build/benchmark/vcache_bench"


def row(repetition, throughput, p99=0, mode="throughput"):
    result = {key: "1" for key in RUNNER.GROUP_FIELDS}
    result.update({
        "case": "sample", "algorithm": "lru", "mode": mode,
        "workload": "uniform", "repetition": str(repetition),
        "ops_per_second": str(throughput), "hit_rate": "0.5",
        "samples": "100" if mode == "latency" else "0",
        "p50_ns": "10" if mode == "latency" else "",
        "p95_ns": "20" if mode == "latency" else "",
        "p99_ns": str(p99) if mode == "latency" else "",
    })
    return result


class StatisticsTests(unittest.TestCase):
    def test_repeated_measurements_keep_variation(self):
        result = RUNNER.summarize([row(0, 100), row(1, 300), row(2, 200)])[0]
        self.assertEqual(result["ops_per_second_median"], 200)
        self.assertEqual(result["ops_per_second_min"], 100)
        self.assertEqual(result["ops_per_second_max"], 300)
        self.assertEqual(result["p99_ns_median_across_repetitions"], "")

    def test_latency_is_median_of_per_run_percentiles(self):
        result = RUNNER.summarize([
            row(0, 100, 100, "latency"), row(1, 300, 1000, "latency"),
            row(2, 200, 200, "latency"),
        ])[0]
        self.assertEqual(result["p99_ns_median_across_repetitions"], 200)
        self.assertEqual(result["samples_min"], 100)

    def test_duplicate_or_nonfinite_measurements_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "repetition"):
            RUNNER.summarize([row(0, 100), row(0, 200)])
        with self.assertRaisesRegex(ValueError, "non-finite"):
            RUNNER.summarize([row(0, "nan")])

    def test_missing_raw_schema_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "raw.csv"
            path.write_text("algorithm,mode\nlru,throughput\n")
            with self.assertRaisesRegex(ValueError, "missing fields"):
                RUNNER.load_rows(path, "case")


class RunnerTests(unittest.TestCase):
    def make_binary(self, directory, source):
        path = Path(directory) / "fake_benchmark.py"
        path.write_text("#!/usr/bin/env python3\n" + textwrap.dedent(source))
        path.chmod(0o755)
        return path

    def test_subprocess_failure_retains_log_and_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = self.make_binary(temporary, '''
                import sys
                print("test failure evidence")
                sys.exit(9)
            ''')
            output = Path(temporary) / "results"
            status = RUNNER.main(["--quick", "--binary", str(binary), "--output-dir", str(output)])
            self.assertEqual(status, 1)
            metadata = json.loads((output / "metadata.json").read_text())
            self.assertEqual(metadata["status"], "failed")
            self.assertEqual(metadata["commands"][0]["returncode"], 9)
            self.assertEqual(metadata["commands"][0]["argv"][0], str(binary.resolve()))
            self.assertTrue(metadata["build"]["binary_sha256"])
            log = next((output / "raw").glob("*.log"))
            self.assertIn("test failure evidence", log.read_text())
            self.assertFalse((output / "summary.csv").exists())

class BinaryIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file():
            raise unittest.SkipTest("No built benchmark executable is available; pass --binary /path/to/vcache_bench")

    def invoke(self, overrides=None):
        settings = {
            "algorithm": "all", "mode": "throughput", "workload": "hotspot",
            "threads": 1, "capacity": 128, "shards": 8, "keyspace": 1024,
            "read-pct": 80, "value-size": 32, "operations": 5000,
            "warmup": 500, "repetitions": 2, "seed": 123, "sample-every": 8,
        }
        settings.update(overrides or {})
        command = [str(BINARY)]
        for key, value in settings.items():
            command.extend(["--" + key, str(value)])
        completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        rows = list(csv.DictReader(io.StringIO(completed.stdout)))
        self.assertTrue(rows)
        return rows

    def test_all_algorithms_share_trace_and_exact_operation_budget(self):
        rows = self.invoke()
        self.assertEqual(len(rows), 10)
        self.assertEqual({row["algorithm"] for row in rows}, RUNNER.EXPECTED_ALGORITHMS)
        for repetition in ("0", "1"):
            group = [row for row in rows if row["repetition"] == repetition]
            self.assertEqual(len(group), 5)
            self.assertEqual(len({row["trace_hash"] for row in group}), 1)
            self.assertEqual(len({(row["gets"], row["puts"]) for row in group}), 1)
            self.assertEqual({int(row["seed"]) for row in group}, {123 + int(repetition)})
            for row in group:
                self.assertEqual(int(row["gets"]) + int(row["puts"]), int(row["operations"]))
                self.assertLessEqual(int(row["hits"]), int(row["gets"]))
                self.assertEqual(int(row["operations"]), 5000)
                self.assertEqual(row["semantics"], "explicit-write")
                self.assertEqual(row["samples"], "0")
                self.assertEqual(row["p99_ns"], "")
                elapsed = float(row["elapsed_seconds"])
                self.assertGreater(elapsed, 0)
                self.assertTrue(math.isclose(float(row["ops_per_second"]), 5000 / elapsed, rel_tol=1e-10))
            compared = {row["algorithm"]: row for row in group}
            self.assertEqual(compared["lru"]["hits"], compared["baseline-lru"]["hits"])
            self.assertEqual(compared["lru"]["checksum"], compared["baseline-lru"]["checksum"])

    def test_seed_replays_trace_and_single_thread_cache_behavior(self):
        first = self.invoke()
        second = self.invoke()
        fields = ("algorithm", "repetition", "seed", "trace_hash", "gets", "puts", "hits", "checksum")
        self.assertEqual([tuple(row[field] for field in fields) for row in first],
                         [tuple(row[field] for field in fields) for row in second])
        changed = self.invoke({"seed": 124})
        self.assertNotEqual(first[0]["trace_hash"], changed[0]["trace_hash"])

    def test_parallel_latency_records_samples_and_remainder_operations(self):
        rows = self.invoke({"mode": "latency", "threads": 3, "operations": 1003, "repetitions": 1})
        for row in rows:
            self.assertEqual(int(row["operations"]), 1003)
            self.assertEqual(int(row["gets"]) + int(row["puts"]), 1003)
            self.assertEqual(int(row["samples"]), (1003 + 7) // 8)
            self.assertGreaterEqual(int(row["p99_ns"]), int(row["p95_ns"]))
            self.assertGreaterEqual(int(row["p95_ns"]), int(row["p50_ns"]))
            self.assertGreaterEqual(int(row["p50_ns"]), 0)

    def test_read_only_prefill_and_read_through_semantics(self):
        for mode in ("throughput", "hit-rate"):
            with self.subTest(mode=mode):
                rows = self.invoke({"mode": mode, "read-pct": 100, "capacity": 128,
                                    "keyspace": 128, "warmup": 0, "repetitions": 1})
                for row in rows:
                    self.assertEqual(row["prefill"], "128")
                    self.assertEqual(row["gets"], "5000")
                    self.assertEqual(row["puts"], "0")
                    self.assertEqual(row["hits"], "5000")
                    self.assertEqual(float(row["hit_rate"]), 1.0)
                    self.assertEqual(row["semantics"], "read-through" if mode == "hit-rate" else "explicit-write")
        rows = self.invoke({"mode": "hit-rate", "workload": "scan", "read-pct": 100,
                            "capacity": 128, "keyspace": 256, "warmup": 0, "repetitions": 1})
        for row in rows:
            if row["algorithm"] in {"lru", "sharded-lru", "baseline-lru"}:
                self.assertEqual(int(row["hits"]), 128)
            self.assertLess(int(row["hits"]), int(row["gets"]))

    def test_invalid_options_and_unwritable_output_fail(self):
        cases = [
            ["--threads", "0"], ["--mode", "unknown"], ["--capacity", "-1"],
            ["--seed", "1", "--seed", "2"], ["--mode", "hit-rate", "--threads", "2"],
            ["--read-pct", "101"], ["--operations", "0"], ["--sample-every", "0"],
        ]
        for flags in cases:
            with self.subTest(flags=flags):
                result = subprocess.run([str(BINARY), *flags], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "readonly"
            directory.mkdir()
            directory.chmod(0o555)
            try:
                if not os.access(directory, os.W_OK):
                    result = subprocess.run([str(BINARY), "--output", str(directory / "raw.csv")],
                                            capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                else:
                    # root may bypass chmod; a regular-file parent is always invalid.
                    parent_file = Path(temporary) / "parent-file"
                    parent_file.write_text("x")
                    result = subprocess.run([str(BINARY), "--output", str(parent_file / "raw.csv")],
                                            capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
            finally:
                directory.chmod(0o755)

    def test_real_quick_runner_completes_and_preserves_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "results"
            command = [sys.executable, str(ROOT / "benchmark/run.py"), "--quick",
                       "--binary", str(BINARY), "--output-dir", str(output),
                       "--operations", "2000", "--warmup", "128", "--repetitions", "1"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            metadata = json.loads((output / "metadata.json").read_text())
            self.assertEqual(metadata["status"], "complete")
            self.assertEqual(metadata["raw_rows"], 20)
            self.assertEqual(len(metadata["commands"]), 4)
            self.assertTrue(all(entry["status"] == "complete" for entry in metadata["commands"]))
            self.assertEqual(len(list((output / "raw").glob("*.csv"))), 4)
            with (output / "summary.csv").open(newline="") as stream:
                summaries = list(csv.DictReader(stream))
            self.assertEqual(len(summaries), 20)
            self.assertEqual({row["repetitions"] for row in summaries}, {"1"})
            self.assertIn("median of per-repetition sampled percentiles", (output / "summary.md").read_text())
            rejected = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(json.loads((output / "metadata.json").read_text())["status"], "complete")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--binary", type=Path, default=BINARY)
    arguments, unittest_arguments = parser.parse_known_args()
    BINARY = arguments.binary.resolve()
    unittest.main(argv=[sys.argv[0], *unittest_arguments])
