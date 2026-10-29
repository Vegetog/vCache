#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "CachePolicy.h"

namespace vcache
{

template<typename Key, typename Value> class LfuCache;

template<typename Key, typename Value>
class FreqList
{
private:
    struct Node
    {
        std::int64_t freq; // Access frequency.
        Key key;
        Value value;
        std::weak_ptr<Node> pre; // Weak backward link avoids ownership cycles.
        std::shared_ptr<Node> next;

        Node()
        : freq(1), key{}, value{}, next(nullptr) {}
        Node(Key key, Value value)
        : freq(1), key(key), value(value), next(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    std::int64_t freq_; // Access frequency.
    NodePtr head_; // Head sentinel.
    NodePtr tail_; // Tail sentinel.

public:
    explicit FreqList(std::int64_t n)
     : freq_(n)
    {
      head_ = std::make_shared<Node>();
      tail_ = std::make_shared<Node>();
      head_->next = tail_;
      tail_->pre = head_;
    }

    ~FreqList()
    {
      auto node = std::move(head_);
      while (node)
      {
        auto next = std::move(node->next);
        node->pre.reset();
        node = std::move(next);
      }
    }

    bool isEmpty() const
    {
      return head_->next == tail_;
    }

    // Node management.
    void addNode(NodePtr node)
    {
        if (!node || !head_ || !tail_)
            return;

        node->pre = tail_->pre;
        node->next = tail_;
        tail_->pre.lock()->next = node; // Lock the weak pointer before accessing the node.
        tail_->pre = node;
    }

    void removeNode(NodePtr node)
    {
        if (!node || !head_ || !tail_)
            return;
        if (node->pre.expired() || !node->next)
            return;

        auto pre = node->pre.lock(); // Lock the weak pointer before accessing the node.
        pre->next = node->next;
        node->next->pre = pre;
        node->next = nullptr; // Clear the forward link to fully detach the node.
    }

    NodePtr getFirstNode() const { return head_->next; }

    friend class LfuCache<Key, Value>;
};

template <typename Key, typename Value>
class LfuCache : public CachePolicy<Key, Value>
{
public:
    using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    LfuCache(int capacity, int maxAverageNum = 1000000)
    : capacity_(capacity), minFreq_(1), maxAverageNum_(maxAverageNum),
      curAverageNum_(0), curTotalNum_(0)
    {
      if (capacity < 0)
          throw std::invalid_argument("cache capacity must be non-negative");
      if (maxAverageNum < 2)
          throw std::invalid_argument("LFU aging threshold must be at least 2");
    }

    ~LfuCache() override = default;

    void put(Key key, Value value) override
    {
        if (capacity_ == 0)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // Update the cached value.
            it->second->value = value;
            // Update the frequency using the node already found.
            getInternal(it->second, value);
            return;
        }

        putInternal(key, value);
    }

    // Copy the cached value into the output parameter on a hit.
    bool get(Key key, Value& value) override
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
          getInternal(it->second, value);
          return true;
      }

      return false;
    }

    Value get(Key key) override
    {
      Value value{};
      get(key, value);
      return value;
    }

      // Clear the cache and release its resources.
    void purge()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      freqToFreqList_.clear();
      nodeMap_.clear();
      minFreq_ = 1;
      curAverageNum_ = 0;
      curTotalNum_ = 0;
    }

private:
    void putInternal(Key key, Value value); // Insert a cached value.
    void getInternal(NodePtr node, Value& value); // Read a cached value.

    void kickOut(); // Evict the least frequently used entry.

    void removeFromFreqList(NodePtr node); // Remove a node from its frequency list.
    void addToFreqList(NodePtr node); // Add a node to its frequency list.

    void addFreqNum(); // Increment total and average access frequencies.
    void decreaseFreqNum(std::int64_t num); // Decrease total and average access frequencies.
    void handleOverMaxAverageNum(); // Age frequencies when the average exceeds the threshold.
    void updateMinFreq();

private:
    int                                            capacity_; // Cache capacity.
    std::int64_t                                   minFreq_; // Minimum access frequency.
    int                                            maxAverageNum_; // Average-frequency aging threshold.
    std::int64_t                                   curAverageNum_; // Current average access frequency.
    std::int64_t                                   curTotalNum_; // Total access frequency across resident entries.
    std::mutex                                     mutex_; // Cache mutex.
    NodeMap                                        nodeMap_; // Key-to-node index.
    std::unordered_map<std::int64_t, std::unique_ptr<FreqList<Key, Value>>> freqToFreqList_;
};

template<typename Key, typename Value>
void LfuCache<Key, Value>::getInternal(NodePtr node, Value& value)
{
    // Move the node to the next frequency list
    // and return its cached value.
    value = node->value;
    // Detach from the current frequency list.
    removeFromFreqList(node);
    node->freq++;
    addToFreqList(node);
    // If the minimum-frequency bucket became empty,
    // advance the minimum frequency to the next bucket.
    if (node->freq - 1 == minFreq_ && freqToFreqList_.find(node->freq - 1) == freqToFreqList_.end())
        minFreq_++;

    // Update total and average access frequencies.
    addFreqNum();
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::putInternal(Key key, Value value)
{
    // Check capacity before inserting a new key.
    if (nodeMap_.size() == static_cast<size_t>(capacity_))
    {
        // Evict an entry and update frequency totals when the cache is full.
        kickOut();
    }

    // Insert a new node and update the minimum frequency.
    NodePtr node = std::make_shared<Node>(key, value);
    nodeMap_[key] = node;
    addToFreqList(node);
    addFreqNum();
    minFreq_ = 1;
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::kickOut()
{
    NodePtr node = freqToFreqList_[minFreq_]->getFirstNode();
    removeFromFreqList(node);
    nodeMap_.erase(node->key);
    decreaseFreqNum(node->freq);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::removeFromFreqList(NodePtr node)
{
    // Ignore null nodes.
    if (!node)
        return;

    auto freq = node->freq;
    auto it = freqToFreqList_.find(freq);
    it->second->removeNode(node);
    // Destroy empty frequency buckets so their number remains bounded by resident entries.
    if (it->second->isEmpty())
        freqToFreqList_.erase(it);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::addToFreqList(NodePtr node)
{
    // Ignore null nodes.
    if (!node)
        return;

    // Create the target frequency bucket if necessary.
    auto freq = node->freq;
    if (freqToFreqList_.find(node->freq) == freqToFreqList_.end())
    {
        // Create the missing bucket.
        freqToFreqList_[node->freq] = std::make_unique<FreqList<Key, Value>>(node->freq);
    }

    freqToFreqList_[freq]->addNode(node);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::addFreqNum()
{
    curTotalNum_++;
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();

    if (curAverageNum_ > maxAverageNum_)
    {
       handleOverMaxAverageNum();
    }
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::decreaseFreqNum(std::int64_t num)
{
    // Update total and average frequencies after eviction.
    curTotalNum_ -= num;
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::handleOverMaxAverageNum()
{
    if (nodeMap_.empty())
        return;

    // Age every resident frequency by half the configured threshold.
    for (auto it = nodeMap_.begin(); it != nodeMap_.end(); ++it)
    {
        // Ignore null nodes.
        if (!it->second)
            continue;

        NodePtr node = it->second;

        // Detach from the current frequency list.
        removeFromFreqList(node);

        // Decrease the frequency.
        const std::int64_t oldFreq = node->freq;

        int decay = maxAverageNum_ / 2;
        node->freq -= decay;

        if (node->freq < 1)
            node->freq = 1;

        const std::int64_t delta = node->freq - oldFreq;
        curTotalNum_ += delta;

        // Add the node to its new frequency list.
        addToFreqList(node);
    }

    // Update the minimum frequency.
    updateMinFreq();
    curAverageNum_ = curTotalNum_ / static_cast<std::int64_t>(nodeMap_.size());
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::updateMinFreq()
{
    minFreq_ = std::numeric_limits<std::int64_t>::max();
    for (const auto& pair : freqToFreqList_)
    {
        if (pair.second && !pair.second->isEmpty())
        {
            minFreq_ = std::min(minFreq_, pair.first);
        }
    }
    if (minFreq_ == std::numeric_limits<std::int64_t>::max())
        minFreq_ = 1;
}

// Partition the configured cache capacity across independent shards.
template<typename Key, typename Value>
class ShardedLfuCache
{
public:
    ShardedLfuCache(size_t capacity, int sliceNum, int maxAverageNum = 1000000)
        : capacity_(capacity)
        , sliceNum_(resolveShardCount(sliceNum))
    {
        const size_t base = capacity / static_cast<size_t>(sliceNum_);
        const size_t remainder = capacity % static_cast<size_t>(sliceNum_);
        if (base > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            (base == static_cast<size_t>(std::numeric_limits<int>::max()) && remainder))
            throw std::length_error("per-shard capacity exceeds supported range");
        for (int i = 0; i < sliceNum_; ++i)
        {
            const size_t sliceSize = base + (static_cast<size_t>(i) < remainder ? 1 : 0);
            lfuSliceCaches_.emplace_back(new LfuCache<Key, Value>(static_cast<int>(sliceSize), maxAverageNum));
        }
    }

    void put(Key key, Value value)
    {
        // Select the LFU shard for this key.
        size_t sliceIndex = Hash(key) % sliceNum_;
        lfuSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // Select the LFU shard for this key.
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value{};
        get(key, value);
        return value;
    }

    // Clear every shard.
    void purge()
    {
        for (auto& lfuSliceCache : lfuSliceCaches_)
        {
            lfuSliceCache->purge();
        }
    }

private:
    static int resolveShardCount(int count)
    {
        if (count < 0)
            throw std::invalid_argument("shard count must be non-negative (0 means automatic)");
        if (count > 0)
            return count;
        const unsigned detected = std::thread::hardware_concurrency();
        return static_cast<int>(std::max(1u, std::min(detected, static_cast<unsigned>(std::numeric_limits<int>::max()))));
    }

    // Hash the key.
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t capacity_; // Total cache capacity.
    int sliceNum_; // Number of shards.
    std::vector<std::unique_ptr<LfuCache<Key, Value>>> lfuSliceCaches_; // LFU shards.
};

} // namespace vcache
