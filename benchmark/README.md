# Benchmark methodology

The vCache benchmark suite establishes a reproducible local baseline for cache policies and sharding trade-offs. It uses C++17 and the Python 3 standard library, without external services or downloaded datasets.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
python3 benchmark/run.py --quick
python3 benchmark/run.py --operations 1000000 --warmup 100000 --repetitions 5
```

The runner defaults to `build/benchmark/vcache_bench`. Use `--binary` to select another build and `--output-dir` to select a new results directory. Nonempty output directories are rejected to preserve previous measurements.

The default matrix contains 21 configurations, each covering five implementations and three repetitions. Each repetition executes 300,000 measured operations and 20,000 warmup operations. Quick mode runs four configurations with two repetitions, 20,000 measured operations, and 2,000 warmup operations.

The default configurations cover:

- Single-threaded uniform, hotspot, and scan workloads with 100%, 80%, and 50% reads.
- Uniform and hotspot throughput with two, four, and eight workers, plus a 1 KiB value configuration.
- Sampled hotspot latency with one and eight workers.
- Single-threaded read-through hit rate for all three workloads.

Other defaults are capacity 1024, 16 shards, keyspace 8192, and 32-byte values. The matrix is a selected set of configurations, not a Cartesian product. Use quick mode to validate the pipeline. For performance analysis, increase operation counts and repetitions, and inspect measurement durations and variation.

Single-configuration examples:

```bash
# Throughput: one million total operations across all workers; fresh cache per run.
./build/benchmark/vcache_bench --algorithm all --mode throughput \
  --workload hotspot --threads 4 --capacity 1024 --shards 16 \
  --keyspace 8192 --read-pct 80 --value-size 32 \
  --operations 1000000 --warmup 100000 --repetitions 5 --seed 42 \
  --output benchmark/results/throughput.csv

# Latency: a separate instrumented run, with additional sampling overhead.
./build/benchmark/vcache_bench --algorithm all --mode latency \
  --threads 4 --sample-every 32 --operations 1000000 \
  --output benchmark/results/latency.csv

# Hit rate: deterministic single-threaded replay, filling on read misses.
./build/benchmark/vcache_bench --algorithm all --mode hit-rate \
  --workload scan --threads 1 --read-pct 100 \
  --output benchmark/results/hit-rate.csv
```

Run `./build/benchmark/vcache_bench --help` for all options. Read percentage is an integer from 0 to 100. Warmup and value size may be zero; other count parameters must be positive. Hit rate is blank when there are no reads, and latency percentiles are blank when there are no samples.

## Fairness and timing

All implementations receive integer keys, equal-length string values, and the same pregenerated key/read/write sequence. Repetition `r` uses `seed + r`. The trace is shared across implementations within a repetition. A fixed random engine and integer rejection sampling avoid dependence on standard-library distribution implementations. `trace_hash` covers both warmup and measured operations so inputs can be compared directly.

Values contain the requested number of `x` characters. Compression, serialization, and backend fetch costs are outside this benchmark. Each fresh cache is prefilled in key order with `[0, min(capacity, keyspace))`, then warmed up by replaying the warmup trace on one thread. Initialization, prefill, warmup, trace generation, thread creation, result-buffer allocation, and sample sorting are excluded from the timed interval. Implementation order rotates across repetitions to reduce fixed ordering effects.

Shard capacities are allocated using a quotient and remainder, so their sum equals the requested total. Each shard has its own eviction state; it cannot borrow another shard's unused capacity. This may change hit rate even at equal total capacity. Both LFU variants use the same frequency-aging threshold of 1,000,000.

The reference LRU is a local mutex-protected `std::list + std::unordered_map` implementation with the same public value-copy and locking contract. It is not a performance claim about another library. Experimental admission and hybrid policies are excluded from the matrix.

### Throughput

The measured trace is divided into contiguous worker ranges, with remainder operations assigned to the first workers. Timing starts immediately before releasing all ready workers and ends at the latest worker completion timestamp. Thread joins are excluded. Aggregate throughput is completed logical operations divided by elapsed wall-clock seconds.

The loop includes cache calls, trace traversal, thread-local counters, and a checksum. It does not generate random numbers or read a clock per operation. Worker wakeup and scheduling overhead remain part of the interval, so very short runs are unsuitable for strong conclusions. Read misses do not fill the cache; only explicit writes insert entries. `gets + puts = operations`, where `puts` counts explicit writes in the input stream.

### Latency

Latency runs time a sample of individual public `get` and `put` calls. Clock reads surround only the API call; counters, checksums, and sample recording are outside that interval. Samples include lock waiting, value copying, and method execution. Clock overhead is not estimated and subtracted, so small differences at nanosecond scales require caution.

Sample selection happens before timing, using an independent random engine to choose one position in every `sample-every`-operation block, including the final partial block. This avoids alignment with periodic scan keys and does not change the operation trace or its hash. `--sample-every 1` measures every operation at the cost of greater measurement interference.

P50, P95, and P99 use nearest rank: sort the samples and select index `ceil(p * N) - 1`. The actual sample count is recorded. This is a closed-loop benchmark, not end-to-end service latency under a fixed request-arrival rate; it does not model a separate queueing system. The instrumented run's ops/s must not be treated as uninstrumented throughput.

### Hit rate

Hit-rate mode requires one worker and replays operations in order. A read miss inserts the supplied value, modeling read-through caching. This extra insertion does not increase the logical operation count or the explicit-write count. Hit rate is read hits divided by read operations, expressed as a ratio from 0 to 1. Warmup operations and explicit writes are excluded from that denominator.

The mode's ops/s includes miss-fill work and is not directly comparable to explicit-write throughput. Multithreaded modes use the same trace and worker partitioning across implementations, but scheduler-dependent lock ordering can change hits and checksums. Use single-threaded hit-rate mode for deterministic policy comparisons.

## Workloads

| Workload | Key distribution |
|---|---|
| `uniform` | Uniform over `[0, keyspace)` |
| `hotspot` | 80% sampled from the first `max(1, keyspace / 100)` keys; 20% from the entire keyspace |
| `scan` | Cycles sequentially through the entire keyspace, continuing the cursor after warmup |

The hotspot range is also included in the full-keyspace component, so the total hot-key probability is slightly above 80%. Read/write selection is independent of key selection. These are synthetic workloads, not production traces.

## Output and interpretation

Each batch run creates:

- `metadata.json`: UTC timestamps, completion status, platform, CPU model and logical core count, Python/compiler/CMake information, Git revision and working-tree status, binary SHA256, exact subprocess arguments, and exit codes.
- `raw/*.csv`: numeric per-configuration, per-implementation, per-repetition observations, including elapsed seconds, ops/s, read/write counts, hits, hit rate, checksum, trace hash, sample count, and percentiles.
- `raw/*.log`: subprocess progress and errors.
- `summary.csv` and `summary.md`: throughput median/minimum/maximum and median hit rate, grouped by configuration and implementation. Latency columns are the median of per-run percentiles, not percentiles computed from pooled samples.

Failures preserve available raw files and logs, mark the metadata as failed, and return a nonzero status. Partial runs are not summarized as complete experiments. Direct C++ execution produces a CSV only; use the Python runner to capture the environment and full run record.

The CSV revision is captured at CMake configuration time. The runner separately records the runtime Git revision and working-tree status. Reconfigure and rebuild after changes when collecting a new baseline. Version metadata can be unknown in a directory without commits.

Keep the machine, build configuration, workload parameters, and operation semantics consistent when comparing results. Inspect hit rate, measurement duration, and variation alongside throughput. The suite does not pin CPU affinity, frequency, power mode, or background load. A single developer-machine result does not establish a general speedup. Debug and sanitizer builds validate correctness and must not be used as Release performance baselines.

## Future work

Potential extensions include validated production trace replay, isolated-process memory measurements, and service comparisons with explicitly defined resource budgets. Allocation and lock-contention improvements should be evaluated against reproducible baselines.
