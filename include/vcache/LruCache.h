#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "CachePolicy.h"

namespace vcache
{

// Forward declaration.
template<typename Key, typename Value> class LruCache;

template<typename Key, typename Value>
class LruNode
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;  // Access count.
    std::weak_ptr<LruNode<Key, Value>> prev_;  // Weak backward link avoids ownership cycles.
    std::shared_ptr<LruNode<Key, Value>> next_;

public:
    LruNode(Key key, Value value)
        : key_(key)
        , value_(value)
        , accessCount_(1)
    {}

    // Node accessors.
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class LruCache<Key, Value>;
};


template<typename Key, typename Value>
class LruCache : public CachePolicy<Key, Value>
{
public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    LruCache(int capacity)
        : capacity_(capacity)
    {
        if (capacity < 0)
            throw std::invalid_argument("cache capacity must be non-negative");
        initializeList();
    }

    ~LruCache() override
    {
        // Unlink nodes iteratively to avoid recursive destruction exhausting the stack.
        auto node = std::move(dummyHead_);
        while (node)
        {
            auto next = std::move(node->next_);
            node->prev_.reset();
            node = std::move(next);
        }
    }

    // Insert a cached value.
    void put(Key key, Value value) override
    {
        if (capacity_ <= 0)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // Update the value and mark the resident entry as most recently used.
            updateExistingNode(it->second, value);
            return ;
        }

        addNewNode(key, value);
    }

    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    Value get(Key key) override
    {
        Value value{};
        // Value initialization also supports nontrivial types such as std::string.
        get(key, value);
        return value;
    }

    // Remove the specified entry.
    void remove(Key key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

private:
    void initializeList()
    {
        // Create the head and tail sentinels.
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    void updateExistingNode(NodePtr node, const Value& value)
    {
        node->setValue(value);
        moveToMostRecent(node);
    }

    void addNewNode(const Key& key, const Value& value)
    {
       if (nodeMap_.size() >= static_cast<size_t>(capacity_))
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value);
       insertNode(newNode);
       nodeMap_[key] = newNode;
    }

    // Move the node to the most recently used position.
    void moveToMostRecent(NodePtr node)
    {
        removeNode(node);
        insertNode(node);
    }

    void removeNode(NodePtr node)
    {
        if(!node->prev_.expired() && node->next_)
        {
            auto prev = node->prev_.lock(); // Lock the weak pointer before accessing the node.
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr; // Clear the forward link to fully detach the node.
        }
    }

    // Insert immediately before the tail sentinel.
    void insertNode(NodePtr node)
    {
        node->next_ = dummyTail_;
        node->prev_ = dummyTail_->prev_;
        dummyTail_->prev_.lock()->next_ = node; // Lock the weak pointer before accessing the node.
        dummyTail_->prev_ = node;
    }

    // Evict the least recently used node.
    void evictLeastRecent()
    {
        NodePtr leastRecent = dummyHead_->next_;
        removeNode(leastRecent);
        nodeMap_.erase(leastRecent->getKey());
    }

private:
    int           capacity_; // Cache capacity.
    NodeMap       nodeMap_; // key -> Node
    std::mutex    mutex_;
    NodePtr       dummyHead_; // Head sentinel.
    NodePtr       dummyTail_;
};

// Experimental access-count admission policy.
// Historical values are not evicted with the history queue; compound operations are not synchronized.
template<typename Key, typename Value>
class AdmissionLruCache : public LruCache<Key, Value>
{
public:
    AdmissionLruCache(int capacity, int historyCapacity, int k)
        : LruCache<Key, Value>(capacity) // Initialize the resident cache.
        , historyList_(std::make_unique<LruCache<Key, size_t>>(historyCapacity))
        , k_(k)
    {
        if (k <= 0)
            throw std::invalid_argument("admission threshold must be positive");
    }

    Value get(Key key)
    {
        // Try the resident cache first.
        Value value{};
        bool inMainCache = LruCache<Key, Value>::get(key, value);

        // Read and increment the historical access count.
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);

        // Return immediately on a resident-cache hit.
        if (inMainCache)
        {
            return value;
        }

        // Check for admission after the access threshold is reached.
        if (historyCount >= k_)
        {
            // Look for a retained historical value.
            auto it = historyValueMap_.find(key);
            if (it != historyValueMap_.end())
            {
                // Promote the retained value to the resident cache.
                Value storedValue = it->second;

                // Remove the historical record.
                historyList_->remove(key);
                historyValueMap_.erase(it);

                // Insert into the resident cache.
                LruCache<Key, Value>::put(key, storedValue);

                return storedValue;
            }
            // Without a historical value, return the default value.
        }

        // Return the default value when admission is not possible.
        return value;
    }

    void put(Key key, Value value)
    {
        // Check the resident cache.
        Value existingValue{};
        bool inMainCache = LruCache<Key, Value>::get(key, existingValue);

        if (inMainCache)
        {
            // Update the resident value.
            LruCache<Key, Value>::put(key, value);
            return;
        }

        // Read and update the access history.
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);

        // Retain the value for possible admission on a later access.
        historyValueMap_[key] = value;

        // Check whether the admission threshold has been reached.
        if (historyCount >= k_)
        {
            // Promote the value when the threshold is reached.
            historyList_->remove(key);
            historyValueMap_.erase(key);
            LruCache<Key, Value>::put(key, value);
        }
    }

private:
    int                                     k_; // Admission access threshold.
    std::unique_ptr<LruCache<Key, size_t>> historyList_; // Historical access counts.
    std::unordered_map<Key, Value>          historyValueMap_; // Values awaiting admission.
};

// Partition LRU entries across independently locked shards.
template<typename Key, typename Value>
class ShardedLruCache
{
public:
    ShardedLruCache(size_t capacity, int sliceNum)
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
            // Distribute the remainder so shard capacities sum to the requested capacity.
            const size_t sliceSize = base + (static_cast<size_t>(i) < remainder ? 1 : 0);
            lruSliceCaches_.emplace_back(new LruCache<Key, Value>(static_cast<int>(sliceSize)));
        }
    }

    void put(Key key, Value value)
    {
        // Select the shard for this key.
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // Select the shard for this key.
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value{};
        get(key, value);
        return value;
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
    size_t                                              capacity_;  // Total cache capacity.
    int                                                 sliceNum_;  // Number of shards.
    std::vector<std::unique_ptr<LruCache<Key, Value>>> lruSliceCaches_; // LRU shards.
};

} // namespace vcache
