#include <vcache/LruCache.h>
#include <vcache/LfuCache.h>
#include <vcache/arc/ArcCache.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void requireInvalid(Function fn) {
    bool rejected = false;
    try { fn(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid parameter must be rejected");
}

void lruPolicy() {
    vcache::LruCache<int, std::string> cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    require(cache.get(1) == "one", "LRU read");
    cache.put(3, "three");
    std::string value = "unchanged";
    require(!cache.get(2, value), "LRU must evict least recently read key");
    require(value == "unchanged", "miss must not overwrite output");
    cache.put(1, "updated");
    cache.put(4, "four");
    require(cache.get(1) == "updated", "LRU update promotes existing key");
    require(!cache.get(3, value), "LRU update must not grow capacity");
    cache.remove(1);
    require(cache.get(1).empty(), "LRU removed string key must return default");

    vcache::LruCache<int, std::string> zero(0);
    zero.put(1, "ignored");
    require(zero.get(1).empty(), "zero capacity LRU must stay empty");
    requireInvalid([] { vcache::LruCache<int, int> invalid(-1); });
}

void lfuPolicy() {
    vcache::LfuCache<int, std::string> cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    require(cache.get(1) == "one", "LFU read");
    cache.put(3, "three");
    std::string value = "unchanged";
    require(!cache.get(2, value), "LFU must evict least frequently used key");
    require(value == "unchanged", "LFU miss must not overwrite output");
    cache.put(1, "updated");
    cache.put(4, "four");
    require(cache.get(1) == "updated", "LFU update retains frequently used key");
    require(cache.get(3).empty(), "LFU update must not grow capacity");

    vcache::LfuCache<int, int> tie(2);
    tie.put(1, 1);
    tie.put(2, 2);
    tie.put(3, 3);
    require(tie.get(1) == 0 && tie.get(2) == 2, "LFU ties must evict oldest key");

    // Exercise aging with every resident key at a frequency greater than 127.
    vcache::LfuCache<int, int> aging(2, 300);
    aging.put(1, 1);
    aging.put(2, 2);
    for (int i = 0; i < 700; ++i) {
        require(aging.get(1) == 1, "LFU aging must preserve values");
        require(aging.get(2) == 2, "LFU aging must preserve values");
    }
    aging.put(3, 3);
    require(aging.get(3) == 3, "LFU must admit key after aging");
    int retained = 0;
    for (int key = 1; key <= 3; ++key) {
        int out = 0;
        retained += aging.get(key, out) ? 1 : 0;
    }
    require(retained == 2, "LFU aging must retain exact capacity");
    aging.purge();
    require(aging.get(3) == 0, "LFU purge clears keys");
    aging.put(5, 5);
    aging.put(6, 6);
    aging.put(7, 7);
    require(aging.get(7) == 7, "LFU remains usable after purge");

    vcache::LfuCache<int, std::string> zero(0);
    zero.put(1, "ignored");
    require(zero.get(1).empty(), "zero capacity LFU must stay empty");
    requireInvalid([] { vcache::LfuCache<int, int> invalid(-1); });
    requireInvalid([] { vcache::LfuCache<int, int> invalid(1, 1); });
}

template <typename Cache>
void shardedCapacity(size_t capacity, int shards) {
    Cache cache(capacity, shards);
    // Supply enough keys to every shard without assuming how std::hash<int> behaves.
    std::vector<int> keys;
    std::vector<size_t> provided(static_cast<size_t>(shards), 0);
    for (int key = 1; keys.size() < static_cast<size_t>(shards) * (capacity + 2); ++key) {
        const auto shard = std::hash<int>{}(key) % static_cast<size_t>(shards);
        if (provided[shard] < capacity + 2) {
            ++provided[shard];
            keys.push_back(key);
            cache.put(key, std::to_string(key));
        }
    }
    size_t retained = 0;
    for (const int key : keys) {
        std::string value;
        if (cache.get(key, value)) {
            ++retained;
            require(value == std::to_string(key), "sharded cache must preserve values");
        }
    }
    require(retained == capacity, "sum of shard capacities must equal requested capacity");
    require(cache.get(-1).empty(), "sharded string miss must return default value");
}

void shardedPolicies() {
    for (const auto capacity : {size_t{0}, size_t{3}, size_t{7}, size_t{12}}) {
        shardedCapacity<vcache::ShardedLruCache<int, std::string>>(capacity, 4);
        shardedCapacity<vcache::ShardedLfuCache<int, std::string>>(capacity, 4);
    }
    vcache::ShardedLruCache<int, int> autoLru(100, 0);
    vcache::ShardedLfuCache<int, int> autoLfu(100, 0);
    autoLru.put(1, 1);
    autoLfu.put(1, 1);
    requireInvalid([] { vcache::ShardedLruCache<int, int> invalid(1, -1); });
    requireInvalid([] { vcache::ShardedLfuCache<int, int> invalid(1, -1); });
}

template <typename Cache>
void concurrentAccess(Cache& cache) {
    constexpr int threads = 4;
    constexpr int keysPerThread = 64;
    std::atomic<bool> correct{true};
    std::vector<std::thread> workers;
    for (int tid = 0; tid < threads; ++tid) {
        workers.emplace_back([&, tid] {
            for (int repeat = 0; repeat < 20; ++repeat) {
                for (int i = 0; i < keysPerThread; ++i) {
                    const int key = tid * keysPerThread + i;
                    const std::string expected = std::to_string(key);
                    cache.put(key, expected);
                    std::string actual;
                    if (!cache.get(key, actual) || actual != expected) correct.store(false);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(correct.load(), "parallel access to distinct resident keys must preserve values");
}

void concurrentPolicies() {
    vcache::LruCache<int, std::string> lru(512);
    vcache::LfuCache<int, std::string> lfu(512);
    vcache::ShardedLruCache<int, std::string> shardedLru(512, 4);
    vcache::ShardedLfuCache<int, std::string> shardedLfu(512, 4);
    concurrentAccess(lru);
    concurrentAccess(lfu);
    concurrentAccess(shardedLru);
    concurrentAccess(shardedLfu);
}

void resourceLifetime() {
    std::weak_ptr<int> lifetime;
    {
        vcache::LfuCache<int, std::shared_ptr<int>> cache(2);
        auto value = std::make_shared<int>(42);
        lifetime = value;
        cache.put(1, value);
        value.reset();
        cache.purge();
        require(lifetime.expired(), "LFU purge must release cached values");
        cache.put(2, std::make_shared<int>(43));
        lifetime = cache.get(2);
    }
    require(lifetime.expired(), "LFU destruction must release cached values and lists");

    // Large lists must be destroyed without recursive shared_ptr release exhausting the stack.
    {
        vcache::LruCache<int, int> cache(100000);
        for (int key = 0; key < 100000; ++key) cache.put(key, key);
    }
    {
        vcache::LfuCache<int, int> cache(100000);
        for (int key = 0; key < 100000; ++key) cache.put(key, key);
    }
}
} // namespace

int main() {
    try {
        lruPolicy();
        lfuPolicy();
        shardedPolicies();
        concurrentPolicies();
        resourceLifetime();
        std::cout << "cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "cache test failed: " << error.what() << '\n';
        return 1;
    }
}
