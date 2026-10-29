#!/usr/bin/env python3
"""Run a reproducible vCache benchmark matrix and retain raw data from every repetition using only the Python standard library."""

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
GROUP_FIELDS = (
    "case", "algorithm", "mode", "workload", "threads", "capacity", "shards",
    "keyspace", "read_pct", "value_size", "operations", "warmup",
)
SUMMARY_FIELDS = GROUP_FIELDS + (
    "repetitions", "ops_per_second_median", "ops_per_second_min",
    "ops_per_second_max", "hit_rate_median", "p50_ns_median_across_repetitions",
    "p95_ns_median_across_repetitions", "p99_ns_median_across_repetitions",
    "samples_min", "samples_max",
)
EXPECTED_ALGORITHMS = {"lru", "lfu", "sharded-lru", "sharded-lfu", "baseline-lru"}


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def probe(command, cwd=ROOT):
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=10)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def cpu_model():
    if sys.platform == "darwin":
        return probe(["sysctl", "-n", "machdep.cpu.brand_string"]) or probe(["sysctl", "-n", "hw.model"])
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.partition(":")[2].strip()
    except OSError:
        pass
    return platform.processor() or None


def build_metadata(binary):
    cache = {}
    for parent in binary.parents:
        path = parent / "CMakeCache.txt"
        if path.is_file():
            for line in path.read_text(errors="replace").splitlines():
                if line.startswith(("CMAKE_CXX_COMPILER:", "CMAKE_BUILD_TYPE:", "CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:")):
                    key, _, value = line.partition("=")
                    cache[key.partition(":")[0]] = value
            cache["cache_path"] = str(path)
            break
    compiler = cache.get("CMAKE_CXX_COMPILER")
    return {
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "cmake_cache": cache,
        "compiler_version": probe([compiler, "--version"]) if compiler else None,
        "cmake_version": probe(["cmake", "--version"]),
    }


def write_metadata(path, metadata):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def matrix(quick=False):
    """Run each case in a separate process with a fixed total operation count across thread counts."""
    if quick:
        return [
            {"mode": "throughput", "workload": "hotspot", "threads": 1},
            {"mode": "throughput", "workload": "hotspot", "threads": 2},
            {"mode": "latency", "workload": "hotspot", "threads": 2},
            {"mode": "hit-rate", "workload": "scan", "threads": 1, "read_pct": 100},
        ]
    cases = [
        {"mode": "throughput", "workload": workload, "threads": 1, "read_pct": reads}
        for workload in ("uniform", "hotspot", "scan")
        for reads in (100, 80, 50)
    ]
    cases.append({"mode": "throughput", "workload": "hotspot", "threads": 1, "value_size": 1024})
    cases.extend(
        {"mode": "throughput", "workload": workload, "threads": threads}
        for workload in ("uniform", "hotspot")
        for threads in (2, 4, 8)
    )
    cases.extend(
        {"mode": "latency", "workload": "hotspot", "threads": threads}
        for threads in (1, 8)
    )
    cases.extend(
        {"mode": "hit-rate", "workload": workload, "threads": 1, "read_pct": 100}
        for workload in ("uniform", "hotspot", "scan")
    )
    return cases


def load_rows(path, case):
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        required = set(GROUP_FIELDS) - {"case"}
        required.update({"repetition", "ops_per_second", "hit_rate", "samples", "p50_ns", "p95_ns", "p99_ns"})
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path}: CSV is missing fields {', '.join(sorted(missing))}")
        rows = [{"case": case, **row} for row in reader]
    if not rows:
        raise ValueError(f"{path}: CSV contains no data rows")
    return rows


def numbers(rows, field):
    values = []
    for row in rows:
        text = row.get(field, "")
        if text in ("", "NA", "N/A"):
            continue
        value = float(text)
        if not math.isfinite(value):
            raise ValueError(f"Field {field} contains a non-finite value: {text}")
        values.append(value)
    return values


def summarize(rows):
    groups = {}
    for row in rows:
        key = tuple(row[field] for field in GROUP_FIELDS)
        groups.setdefault(key, []).append(row)
    summaries = []
    for key, group in groups.items():
        repetitions = [row["repetition"] for row in group]
        if len(set(repetitions)) != len(repetitions):
            raise ValueError(f"Duplicate repetition: {key}")
        item = dict(zip(GROUP_FIELDS, key))
        item["repetitions"] = len(group)
        throughput = numbers(group, "ops_per_second")
        if len(throughput) != len(group) or any(value <= 0 for value in throughput):
            raise ValueError(f"Invalid throughput result: {key}")
        item["ops_per_second_median"] = statistics.median(throughput)
        item["ops_per_second_min"] = min(throughput)
        item["ops_per_second_max"] = max(throughput)
        rates = numbers(group, "hit_rate")
        if any(value < 0 or value > 1 for value in rates):
            raise ValueError(f"hit_rate must be within 0..1: {key}")
        item["hit_rate_median"] = statistics.median(rates) if rates else ""
        for field in ("p50_ns", "p95_ns", "p99_ns"):
            values = numbers(group, field) if item["mode"] == "latency" else []
            item[field + "_median_across_repetitions"] = statistics.median(values) if values else ""
        samples = numbers(group, "samples")
        item["samples_min"] = int(min(samples)) if samples else 0
        item["samples_max"] = int(max(samples)) if samples else 0
        summaries.append(item)
    return summaries


def pretty(value):
    return f"{value:.6g}" if isinstance(value, float) else str(value)


def write_summary(directory, rows):
    summaries = summarize(rows)
    with (directory / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        writer.writerows(summaries)
    lines = [
        "# vCache Benchmark Summary", "",
        "Throughput statistics report the median, minimum, and maximum ops/s across repetitions. Raw data from every repetition is available in raw/.", "",
        "Latency columns report the median of per-repetition sampled percentiles, not percentiles computed from pooled samples.", "",
        "Throughput and latency use separate runs; direct comparisons apply only within the same case. Hit rate uses read operations as the denominator. The hit-rate mode fills read misses; other modes do not.", "",
        "Each repetition prefills min(capacity, keyspace) entries in the same key order before warmup. Both prefill and warmup are excluded from measurement.", "",
        "This matrix establishes an initial baseline. Formal experiments should specify larger operation counts, more repetitions, and a dedicated machine.", "",
        "| Case | Algorithm | Mode | Threads | Median throughput ops/s | Minimum | Maximum | Median hit rate | Median per-repetition P99 ns | Repetitions |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summaries:
        lines.append("| " + " | ".join(pretty(row[field]) for field in (
            "case", "algorithm", "mode", "threads", "ops_per_second_median",
            "ops_per_second_min", "ops_per_second_max", "hit_rate_median",
            "p99_ns_median_across_repetitions", "repetitions",
        )) + " |")
    (directory / "summary.md").write_text("\n".join(lines) + "\n")


def positive(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def nonnegative(text):
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/benchmark/vcache_bench", help="compiled vcache_bench executable")
    parser.add_argument("--output-dir", type=Path, help="new empty result directory; defaults to a generated UTC timestamp directory")
    parser.add_argument("--quick", action="store_true", help="run four small cases to check the build and benchmark pipeline")
    parser.add_argument("--operations", type=positive, help="total operations per repetition; default: 300000, or 20000 with --quick")
    parser.add_argument("--warmup", type=nonnegative, help="total warmup operations per repetition; default: 20000, or 2000 with --quick")
    parser.add_argument("--repetitions", type=positive, help="repetitions per case; default: 3, or 2 with --quick")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    binary = args.binary.resolve()
    if not binary.is_file():
        print(f"Error: benchmark executable not found at {binary}; build it first.", file=sys.stderr)
        return 2
    directory = args.output_dir
    if directory is None:
        timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        directory = ROOT / "benchmark/results" / f"{timestamp}-{uuid.uuid4().hex[:8]}"
    directory = directory.resolve()
    if directory.exists() and any(directory.iterdir()):
        print(f"Error: result directory is not empty; refusing to overwrite an existing experiment: {directory}", file=sys.stderr)
        return 2
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "raw").mkdir(exist_ok=True)
    metadata = {
        "schema_version": 1,
        "status": "running",
        "started_at_utc": utc_now(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_cpus": os.cpu_count(),
        "python": sys.version,
        "git_revision": probe(["git", "rev-parse", "HEAD"]),
        "git_status_porcelain": probe(["git", "status", "--porcelain", "--untracked-files=normal"]),
        "runner_command": [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]] if argv is None else [str(Path(__file__).resolve()), *argv],
        "build": build_metadata(binary),
        "quick": args.quick,
        "commands": [],
    }
    metadata["git_dirty"] = bool(metadata["git_status_porcelain"]) if metadata["git_status_porcelain"] is not None else None
    metadata_path = directory / "metadata.json"
    write_metadata(metadata_path, metadata)
    rows = []
    cases = matrix(args.quick)
    try:
        for index, case in enumerate(cases, 1):
            settings = {
                "mode": "throughput", "workload": "hotspot", "threads": 1,
                "capacity": 1024, "shards": 16, "keyspace": 8192,
                "read_pct": 80, "value_size": 32,
                "operations": args.operations or (20000 if args.quick else 300000),
                "warmup": args.warmup if args.warmup is not None else (2000 if args.quick else 20000),
                "repetitions": args.repetitions or (2 if args.quick else 3),
                "seed": 20261029, "sample_every": 32,
                **case,
            }
            name = f"{index:02d}-{settings['mode']}-{settings['workload']}-t{settings['threads']}-r{settings['read_pct']}-v{settings['value_size']}"
            output = directory / "raw" / f"{name}.csv"
            command = [str(binary), "--algorithm", "all"]
            for key, value in settings.items():
                command.extend(["--" + key.replace("_", "-"), str(value)])
            command.extend(["--output", str(output)])
            entry = {"case": name, "argv": command, "started_at_utc": utc_now(), "status": "running"}
            metadata["commands"].append(entry)
            write_metadata(metadata_path, metadata)
            print(f"[{index}/{len(cases)}] {name}", flush=True)
            with (directory / "raw" / f"{name}.log").open("w") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            entry.update({"returncode": result.returncode, "finished_at_utc": utc_now()})
            if result.returncode:
                entry["status"] = "failed"
                raise RuntimeError(f"Case {name} failed with exit code {result.returncode}; see raw/{name}.log")
            case_rows = load_rows(output, name)
            counts = {}
            for row in case_rows:
                counts[row["algorithm"]] = counts.get(row["algorithm"], 0) + 1
            if set(counts) != EXPECTED_ALGORITHMS or any(count != settings["repetitions"] for count in counts.values()):
                raise ValueError(f"Case {name}: raw row counts do not match the expected algorithms and repetitions: {counts}")
            rows.extend(case_rows)
            entry.update({"status": "complete", "rows": len(case_rows)})
            write_metadata(metadata_path, metadata)
        write_summary(directory, rows)
        metadata["status"] = "complete"
        metadata["raw_rows"] = len(rows)
    except KeyboardInterrupt:
        if metadata["commands"] and metadata["commands"][-1]["status"] == "running":
            metadata["commands"][-1].update(status="interrupted", finished_at_utc=utc_now())
        metadata["status"] = "interrupted"
        metadata["error"] = "Run interrupted by the user"
    except (OSError, ValueError, RuntimeError) as error:
        if metadata["commands"] and metadata["commands"][-1]["status"] == "running":
            metadata["commands"][-1].update(status="failed", finished_at_utc=utc_now())
        metadata["status"] = "failed"
        metadata["error"] = str(error)
        print(f"Error: {error}", file=sys.stderr)
    finally:
        metadata["finished_at_utc"] = utc_now()
        write_metadata(metadata_path, metadata)
    print(f"Results directory: {directory}")
    return 0 if metadata["status"] == "complete" else 1


if __name__ == "__main__":
    raise SystemExit(main())
