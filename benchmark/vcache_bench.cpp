#include <vcache/LfuCache.h>
#include <vcache/LruCache.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef VCACHE_COMPILER
#define VCACHE_COMPILER "unknown"
#endif
#ifndef VCACHE_BUILD_TYPE
#define VCACHE_BUILD_TYPE "unknown"
#endif
#ifndef VCACHE_REVISION
#define VCACHE_REVISION "unknown"
#endif

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::string algorithm = "all";
    std::string workload = "hotspot";
    std::string mode = "throughput";
    std::string output;
    std::size_t threads = 1;
    std::size_t capacity = 4096;
    std::size_t shards = 16;
    std::size_t keyspace = 65536;
    std::size_t readPct = 80;
    std::size_t valueSize = 64;
    std::size_t operations = 1000000;
    std::size_t warmup = 100000;
    std::size_t repetitions = 5;
    std::uint64_t seed = 42;
    std::size_t sampleEvery = 100;
};

const std::vector<std::string> algorithms = {
    "lru", "lfu", "sharded-lru", "sharded-lfu", "baseline-lru"};

void usage() {
    std::cout
        << "vCache benchmark (C++17, no external dependencies)\n"
        << "  --algorithm all|lru|lfu|sharded-lru|sharded-lfu|baseline-lru\n"
        << "  --workload uniform|hotspot|scan\n"
        << "  --mode throughput|latency|hit-rate\n"
        << "  --threads N       Worker count (hit-rate requires 1)\n"
        << "  --capacity N      Maximum cache entries\n"
        << "  --shards N        Shards for sharded caches\n"
        << "  --keyspace N      Keys in [0, N)\n"
        << "  --read-pct N      Read percentage, 0..100\n"
        << "  --value-size N    Value bytes (including zero)\n"
        << "  --operations N    Total measured operations across all workers\n"
        << "  --warmup N        Total untimed warmup operations\n"
        << "  --repetitions N   Independent repetitions, seed + repetition\n"
        << "  --seed N          Unsigned 64-bit base seed\n"
        << "  --sample-every N  Sample one operation per block of N in latency mode\n"
        << "  --output PATH     CSV output (default: stdout)\n"
        << "\nThroughput uses explicit writes; read misses do not fill the cache.\n"
        << "Hit-rate uses read-through (a read miss inserts the value).\n"
        << "All caches prefill keys [0,min(capacity,keyspace)), then warm up.\n"
        << "Hotspot: 80% of keys from the first max(1,keyspace/100) keys,\n"
        << "         otherwise from the entire keyspace. Scan cycles all keys.\n";
}

std::uint64_t number(const std::string& flag, const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(flag + " requires an unsigned decimal integer");
    std::size_t parsed = 0;
    try {
        const auto result = std::stoull(value, &parsed);
        if (parsed != value.size()) throw std::invalid_argument("trailing characters");
        return result;
    } catch (const std::exception&) {
        throw std::invalid_argument(flag + " is out of range");
    }
}

std::size_t sizeNumber(const std::string& flag, const std::string& value) {
    const auto result = number(flag, value);
    if (result > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(flag + " is out of range for this platform");
    return static_cast<std::size_t>(result);
}

Options parse(int argc, char** argv) {
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            usage();
            std::exit(0);
        }
        if (!seen.insert(flag).second)
            throw std::invalid_argument("duplicate option: " + flag);
        if (++i >= argc) throw std::invalid_argument("missing value for " + flag);
        const std::string value = argv[i];
        if (flag == "--algorithm") o.algorithm = value;
        else if (flag == "--workload") o.workload = value;
        else if (flag == "--mode") o.mode = value;
        else if (flag == "--output") {
            if (value.empty()) throw std::invalid_argument("--output must not be empty");
            o.output = value;
        }
        else if (flag == "--threads") o.threads = sizeNumber(flag, value);
        else if (flag == "--capacity") o.capacity = sizeNumber(flag, value);
        else if (flag == "--shards") o.shards = sizeNumber(flag, value);
        else if (flag == "--keyspace") o.keyspace = sizeNumber(flag, value);
        else if (flag == "--read-pct") o.readPct = sizeNumber(flag, value);
        else if (flag == "--value-size") o.valueSize = sizeNumber(flag, value);
        else if (flag == "--operations") o.operations = sizeNumber(flag, value);
        else if (flag == "--warmup") o.warmup = sizeNumber(flag, value);
        else if (flag == "--repetitions") o.repetitions = sizeNumber(flag, value);
        else if (flag == "--seed") o.seed = number(flag, value);
        else if (flag == "--sample-every") o.sampleEvery = sizeNumber(flag, value);
        else throw std::invalid_argument("unknown option: " + flag);
    }
    if (o.algorithm != "all" &&
        std::find(algorithms.begin(), algorithms.end(), o.algorithm) == algorithms.end())
        throw std::invalid_argument("unknown algorithm: " + o.algorithm);
    if (o.workload != "uniform" && o.workload != "hotspot" && o.workload != "scan")
        throw std::invalid_argument("unknown workload: " + o.workload);
    if (o.mode != "throughput" && o.mode != "latency" && o.mode != "hit-rate")
        throw std::invalid_argument("unknown mode: " + o.mode);
    if (!o.threads || !o.capacity || !o.shards || !o.keyspace || !o.operations ||
        !o.repetitions || !o.sampleEvery)
        throw std::invalid_argument("threads, capacity, shards, keyspace, operations, repetitions and sample-every must be positive");
    if (o.threads > o.operations)
        throw std::invalid_argument("threads must not exceed total operations");
    if (o.readPct > 100) throw std::invalid_argument("read-pct must be in [0, 100]");
    if (o.capacity > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        o.keyspace > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        o.shards > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("capacity, keyspace and shards must fit in int");
    if (o.mode == "hit-rate" && o.threads != 1)
        throw std::invalid_argument("hit-rate requires --threads 1 for deterministic replay");
    if (o.repetitions - 1 > std::numeric_limits<std::uint64_t>::max() - o.seed)
        throw std::invalid_argument("seed + repetitions would overflow");
    return o;
}

struct Operation {
    int key;
    bool read;
};

// Rejection sampling avoids modulo bias and keeps traces independent of the
// standard library's implementation of uniform_int_distribution.
std::uint64_t bounded(std::mt19937_64& rng, std::uint64_t bound) {
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t value;
    do { value = rng(); } while (value < threshold);
    return value % bound;
}

struct Trace {
    std::vector<Operation> warmup;
    std::vector<Operation> measured;
    std::vector<std::uint8_t> sampleMask;
    std::uint64_t hash = 14695981039346656037ULL;
};

void hashNumber(std::uint64_t& hash, std::uint64_t value) {
    // Fixed little-endian FNV-1a encoding, independent of std::hash and endianness.
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= value & 255;
        hash *= 1099511628211ULL;
        value >>= 8;
    }
}

Trace makeTrace(const Options& o, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::size_t cursor = 0;
    auto fill = [&](std::vector<Operation>& target, std::size_t count) {
        target.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const bool read = bounded(rng, 100) < o.readPct;
            std::size_t key;
            if (o.workload == "scan") {
                key = cursor;
                cursor = (cursor + 1) % o.keyspace;
            } else if (o.workload == "hotspot" && bounded(rng, 100) < 80) {
                key = bounded(rng, std::max(std::size_t{1}, o.keyspace / 100));
            } else {
                key = bounded(rng, o.keyspace);
            }
            target.push_back({static_cast<int>(key), read});
        }
    };
    Trace trace;
    fill(trace.warmup, o.warmup);
    fill(trace.measured, o.operations);
    hashNumber(trace.hash, trace.warmup.size());
    hashNumber(trace.hash, trace.measured.size());
    for (const auto& phase : {&trace.warmup, &trace.measured}) {
        for (const auto& op : *phase) {
            hashNumber(trace.hash, static_cast<std::uint64_t>(op.key));
            hashNumber(trace.hash, op.read ? 1 : 0);
        }
    }
    if (o.mode == "latency") {
        // One uniformly chosen sample per block, generated independently from
        // key/operation RNG. Periodic sampling would alias sequential scans.
        std::mt19937_64 sampleRng(seed ^ 0x9e3779b97f4a7c15ULL);
        trace.sampleMask.resize(o.operations, 0);
        for (std::size_t begin = 0; begin < o.operations;) {
            const auto count = std::min(o.sampleEvery, o.operations - begin);
            trace.sampleMask[begin + bounded(sampleRng, count)] = 1;
            begin += count;
        }
    }
    return trace;
}

// Deliberately simple mutex-protected list + map reference, with the same value
// copy and locking contract as vCache. It is a local baseline, not another library.
class BaselineLru {
public:
    explicit BaselineLru(std::size_t capacity) : capacity_(capacity) {}
    void put(int key, std::string value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find(key);
        if (found != index_.end()) {
            found->second->second = value;
            order_.splice(order_.begin(), order_, found->second);
            return;
        }
        order_.emplace_front(key, value);
        try {
            index_.emplace(key, order_.begin());
        } catch (...) {
            order_.pop_front();
            throw;
        }
        if (index_.size() > capacity_) {
            index_.erase(order_.back().first);
            order_.pop_back();
        }
    }
    bool get(int key, std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find(key);
        if (found == index_.end()) return false;
        order_.splice(order_.begin(), order_, found->second);
        value = found->second->second;
        return true;
    }
private:
    using Entries = std::list<std::pair<int, std::string>>;
    std::size_t capacity_;
    Entries order_;
    std::unordered_map<int, Entries::iterator> index_;
    std::mutex mutex_;
};

struct alignas(64) WorkerStats {
    std::size_t operations = 0;
    std::size_t gets = 0;
    std::size_t hits = 0;
    std::uint64_t checksum = 0;
    std::vector<std::uint64_t> samples;
    std::string out;
    Clock::time_point end;
    std::exception_ptr error;
};

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t ready = 0;
    bool start = false;
    bool abort = false;
};

struct Result {
    std::size_t operations = 0;
    std::size_t gets = 0;
    std::size_t hits = 0;
    std::uint64_t checksum = 0;
    double seconds = 0;
    std::vector<std::uint64_t> samples;
};

template <bool Sample = false, class Cache>
void execute(Cache& cache, const Operation& op, const std::string& value,
             bool readThrough, WorkerStats& stats) {
    if (op.read) {
        ++stats.gets;
        bool hit;
        if constexpr (Sample) {
            const auto before = Clock::now();
            hit = cache.get(op.key, stats.out);
            const auto after = Clock::now();
            stats.samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
        } else {
            hit = cache.get(op.key, stats.out);
        }
        if (hit) {
            ++stats.hits;
            stats.checksum += static_cast<std::uint64_t>(op.key) + stats.out.size() + 1;
            if (!stats.out.empty())
                stats.checksum += static_cast<unsigned char>(stats.out.front()) +
                                  static_cast<unsigned char>(stats.out.back());
        } else if (readThrough) {
            cache.put(op.key, value);
        }
    } else {
        if constexpr (Sample) {
            const auto before = Clock::now();
            cache.put(op.key, value);
            const auto after = Clock::now();
            stats.samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
        } else {
            cache.put(op.key, value);
        }
        stats.checksum += static_cast<std::uint64_t>(op.key) + value.size() + 1;
    }
    ++stats.operations;
}

template <class Cache>
Result run(Cache& cache, const Options& o, const Trace& trace) {
    const std::string value(o.valueSize, 'x');
    const bool readThrough = o.mode == "hit-rate";
    for (std::size_t key = 0; key < std::min(o.capacity, o.keyspace); ++key)
        cache.put(static_cast<int>(key), value);
    WorkerStats warmup;
    warmup.out.reserve(o.valueSize);
    for (const auto& op : trace.warmup) execute(cache, op, value, readThrough, warmup);

    std::vector<WorkerStats> stats(o.threads);
    const auto block = o.operations / o.threads;
    const auto remainder = o.operations % o.threads;
    auto beginFor = [&](std::size_t tid) {
        return tid * block + std::min(tid, remainder);
    };
    for (std::size_t tid = 0; tid < o.threads; ++tid) {
        stats[tid].out.reserve(o.valueSize);
        if (o.mode == "latency") {
            const auto count = block + (tid < remainder ? 1 : 0);
            // A worker's contiguous slice can overlap two partial sample blocks.
            stats[tid].samples.reserve(count / o.sampleEvery + 2);
        }
    }

    Gate gate;
    std::vector<std::thread> workers;
    workers.reserve(o.threads);
    auto worker = [&](std::size_t tid) {
        auto& local = stats[tid];
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            ++gate.ready;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&] { return gate.start || gate.abort; });
            if (gate.abort) return;
        }
        const auto begin = beginFor(tid);
        const auto end = begin + block + (tid < remainder ? 1 : 0);
        try {
            if (o.mode == "latency") {
                for (auto i = begin; i < end; ++i) {
                    if (trace.sampleMask[i]) {
                        execute<true>(cache, trace.measured[i], value, readThrough, local);
                    } else {
                        execute(cache, trace.measured[i], value, readThrough, local);
                    }
                }
            } else {
                for (auto i = begin; i < end; ++i)
                    execute(cache, trace.measured[i], value, readThrough, local);
            }
        } catch (...) {
            local.error = std::current_exception();
        }
        local.end = Clock::now();
    };

    try {
        for (std::size_t tid = 0; tid < o.threads; ++tid) workers.emplace_back(worker, tid);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.abort = true;
        }
        gate.cv.notify_all();
        for (auto& thread : workers) thread.join();
        throw;
    }

    Clock::time_point start;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.cv.wait(lock, [&] { return gate.ready == o.threads; });
        start = Clock::now();
        gate.start = true;
    }
    gate.cv.notify_all();
    for (auto& thread : workers) thread.join();

    Result result;
    auto end = start;
    for (auto& local : stats) {
        if (local.error) std::rethrow_exception(local.error);
        end = std::max(end, local.end);
        result.operations += local.operations;
        result.gets += local.gets;
        result.hits += local.hits;
        result.checksum += local.checksum;
        result.samples.insert(result.samples.end(), local.samples.begin(), local.samples.end());
    }
    result.seconds = std::chrono::duration<double>(end - start).count();
    if (result.operations != o.operations || result.seconds <= 0 || !std::isfinite(result.seconds))
        throw std::runtime_error("invalid measurement: operation count or elapsed time");
    std::sort(result.samples.begin(), result.samples.end());
    return result;
}

Result runAlgorithm(const std::string& name, const Options& o, const Trace& trace) {
    const auto capacity = static_cast<int>(o.capacity);
    const auto shards = static_cast<int>(o.shards);
    if (name == "lru") {
        vcache::LruCache<int, std::string> cache(capacity);
        return run(cache, o, trace);
    }
    if (name == "lfu") {
        vcache::LfuCache<int, std::string> cache(capacity, 1000000);
        return run(cache, o, trace);
    }
    if (name == "sharded-lru") {
        vcache::ShardedLruCache<int, std::string> cache(o.capacity, shards);
        return run(cache, o, trace);
    }
    if (name == "sharded-lfu") {
        vcache::ShardedLfuCache<int, std::string> cache(o.capacity, shards, 1000000);
        return run(cache, o, trace);
    }
    if (name == "baseline-lru") {
        BaselineLru cache(o.capacity);
        return run(cache, o, trace);
    }
    throw std::invalid_argument("unknown algorithm: " + name);
}

std::string csv(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string result = "\"";
    for (char c : value) {
        if (c == '\"') result += '\"';
        result += c;
    }
    return result + '\"';
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::size_t pct) {
    // Nearest rank: ceil(pct * N / 100) - 1, without overflowing pct * N.
    const auto rank = (sorted.size() / 100) * pct +
                      ((sorted.size() % 100) * pct + 99) / 100;
    return sorted.at(rank - 1);
}

void writeResult(std::ostream& out, const std::string& name, const Options& o,
                 std::size_t repetition, std::uint64_t seed, std::uint64_t traceHash,
                 const Result& r) {
    const auto shards = name.compare(0, 8, "sharded-") == 0 ? o.shards : 1;
    out << std::setprecision(17)
        << name << ',' << o.mode << ',' << o.workload << ',' << o.threads << ','
        << o.capacity << ',' << shards << ',' << o.keyspace << ',' << o.readPct << ','
        << o.valueSize << ',' << r.operations << ',' << o.warmup << ',' << repetition << ','
        << seed << ',' << r.seconds << ',' << r.operations / r.seconds << ','
        << r.gets << ',' << r.hits << ',';
    if (r.gets) out << static_cast<double>(r.hits) / r.gets;
    out << ',' << r.checksum << ',' << r.samples.size() << ',';
    if (!r.samples.empty()) out << percentile(r.samples, 50);
    out << ',';
    if (!r.samples.empty()) out << percentile(r.samples, 95);
    out << ',';
    if (!r.samples.empty()) out << percentile(r.samples, 99);
    out << ',' << o.sampleEvery << ','
        << (o.mode == "hit-rate" ? "read-through" : "explicit-write") << ','
        << csv(VCACHE_COMPILER) << ',' << csv(VCACHE_BUILD_TYPE) << ','
        << csv(VCACHE_REVISION) << ',' << traceHash << ',' << (r.operations - r.gets)
        << ',' << std::min(o.capacity, o.keyspace) << '\n';
    out.flush();
    if (!out) throw std::runtime_error("failed to write benchmark CSV");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        std::ofstream file;
        std::ostream* output = &std::cout;
        if (!options.output.empty()) {
            const auto parent = std::filesystem::path(options.output).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            file.open(options.output, std::ios::out | std::ios::trunc);
            if (!file) throw std::runtime_error("cannot open output: " + options.output);
            output = &file;
        }
        *output << "algorithm,mode,workload,threads,capacity,shards,keyspace,read_pct,"
                   "value_size,operations,warmup,repetition,seed,elapsed_seconds,"
                   "ops_per_second,gets,hits,hit_rate,checksum,samples,p50_ns,p95_ns,"
                   "p99_ns,sample_every,semantics,compiler,build_type,revision,"
                   "trace_hash,puts,prefill\n";
        output->flush();
        if (!*output) throw std::runtime_error("failed to write benchmark CSV header");
        const auto selected = options.algorithm == "all" ? algorithms :
                              std::vector<std::string>{options.algorithm};
        for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
            const auto seed = options.seed + repetition;
            const auto trace = makeTrace(options, seed);
            // Rotate order to distribute first/last-run effects over repetitions.
            for (std::size_t index = 0; index < selected.size(); ++index) {
                const auto& name = selected[(index + repetition) % selected.size()];
                const auto result = runAlgorithm(name, options, trace);
                writeResult(*output, name, options, repetition, seed, trace.hash, result);
                std::cerr << name << " mode=" << options.mode << " repetition=" << repetition
                          << " operations=" << result.operations << '\n';
            }
        }
        if (file.is_open()) {
            file.close();
            if (!file) throw std::runtime_error("failed to close output: " + options.output);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vcache_bench: " << error.what() << '\n';
        return 1;
    }
}
