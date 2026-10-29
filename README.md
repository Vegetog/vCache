# vCache

vCache is a header-only C++17 library for in-process concurrent caching. It provides LRU, LFU, and hash-sharded variants, together with reproducible benchmarks for throughput, sampled latency, and hit rate.

## Build and test

Requirements: CMake 3.16+, a C++17 compiler, and a platform thread library. Python 3 is required for the benchmark runner and its integration tests.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd build && ctest --output-on-failure)
./build/vcache_example
```

To use vCache in another CMake project, add it with `add_subdirectory` and link against `vcache::vcache`. Alternatively, add `include/` to your include path and link against your platform's thread library.

## Usage

```cpp
#include <string>
#include <vcache/LruCache.h>

vcache::LruCache<int, std::string> cache(1024);
cache.put(1, "hello");

std::string value;
if (cache.get(1, value)) {
    // Use the cached value.
}

vcache::ShardedLruCache<int, std::string> sharded(1024, 16);
sharded.put(1, "hello");
```

Prefer `get(key, value)`, which returns `false` on a miss and leaves the output unchanged. The single-argument `get(key)` returns `Value{}` on a miss, so it cannot distinguish a miss from a cached default value. Capacity is an entry limit, not a byte budget. Values are copied by the public API.

## Implementations

| Implementation | Header | Status |
|---|---|---|
| `LruCache` | `vcache/LruCache.h` | LRU eviction with one mutex |
| `ShardedLruCache` | `vcache/LruCache.h` | Independent LRU caches and mutexes selected by key hash |
| `LfuCache` | `vcache/LfuCache.h` | Frequency buckets and frequency aging with one mutex |
| `ShardedLfuCache` | `vcache/LfuCache.h` | Independent LFU caches and mutexes selected by key hash |
| `AdmissionLruCache` | `vcache/LruCache.h` | Experimental threshold admission; API and history bounds require further work |
| `ArcCache` | `vcache/arc/ArcCache.h` | Experimental LRU/LFU hybrid; global capacity and concurrency semantics require further work |

The benchmark suite covers the first four implementations and a mutex-protected `std::list + std::unordered_map` LRU reference. Experimental implementations are excluded from equal-capacity rankings and do not carry the same thread-safety guarantees.

Shard capacities sum exactly to the requested total, including when capacity is not divisible by the shard count. Shards evict independently: unused space cannot be borrowed by another shard, and global LRU/LFU order is not preserved.

## Benchmarks

Run a small end-to-end check:

```bash
python3 benchmark/run.py --quick
```

Run the default benchmark matrix:

```bash
python3 benchmark/run.py
```

Each run creates a separate directory under `benchmark/results/` containing raw per-repetition CSV files, logs, environment metadata, exact commands, and CSV/Markdown summaries. The repository does not ship preset performance scores. Quick mode validates the pipeline; performance conclusions require sufficiently long repeated measurements.

To run a single configuration:

```bash
./build/benchmark/vcache_bench \
  --algorithm all --mode throughput --workload hotspot \
  --capacity 1024 --shards 16 --keyspace 8192 \
  --read-pct 80 --value-size 32 --threads 4 \
  --operations 1000000 --warmup 100000 --repetitions 5 \
  --seed 42 --output benchmark/results/custom.csv
```

`--operations` is the total across all workers. Throughput uses one timing interval for the operation loop. Latency is measured in a separate run with sampled API calls. Hit-rate mode uses deterministic single-threaded read-through replay. See [benchmark methodology](benchmark/README.md) for workloads, timing boundaries, and interpretation.

## Layout

```text
include/vcache/   Cache implementations and interface
examples/         Minimal usage example
tests/            Correctness and benchmark integration tests
benchmark/        Parameterized benchmark and batch runner
```

## License

vCache is distributed under the [GNU General Public License v3.0](LICENSE).
