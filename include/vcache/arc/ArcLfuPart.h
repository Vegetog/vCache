#pragma once

#include "ArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <list>
#include <mutex>

namespace vcache
{

template<typename Key, typename Value>
class ArcLfuPart
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqMap = std::map<size_t, std::list<NodePtr>>;

    explicit ArcLfuPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
        , minFreq_(0)
    {
        initializeLists();
    }

    ~ArcLfuPart()
    {
        auto node = std::move(ghostHead_);
        while (node)
        {
            auto next = std::move(node->next_);
            node->prev_.reset();
            node = std::move(next);
        }
    }

    bool put(Key key, Value value)
    {
        if (capacity_ == 0)
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    bool get(Key key, Value& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
        {
            updateNodeFrequency(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    bool contain(Key key)
    {
        return mainCache_.find(key) != mainCache_.end();
    }

    bool checkGhost(Key key)
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end())
        {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    void increaseCapacity() { ++capacity_; }

    bool decreaseCapacity()
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_)
        {
            evictLeastFrequent();
        }
        --capacity_;
        return true;
    }

private:
    void initializeLists()
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value)
    {
        node->setValue(value);
        updateNodeFrequency(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value)
    {
        if (mainCache_.size() >= capacity_)
        {
            evictLeastFrequent();
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;

        // Add the new node to the frequency-1 list.
        if (freqMap_.find(1) == freqMap_.end())
        {
            freqMap_[1] = std::list<NodePtr>();
        }
        freqMap_[1].push_back(newNode);
        minFreq_ = 1;

        return true;
    }

    void updateNodeFrequency(NodePtr node)
    {
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount();
        size_t newFreq = node->getAccessCount();

        // Remove the node from its previous frequency list.
        auto& oldList = freqMap_[oldFreq];
        oldList.remove(node);
        if (oldList.empty())
        {
            freqMap_.erase(oldFreq);
            if (oldFreq == minFreq_)
            {
                minFreq_ = newFreq;
            }
        }

        // Add the node to its new frequency list.
        if (freqMap_.find(newFreq) == freqMap_.end())
        {
            freqMap_[newFreq] = std::list<NodePtr>();
        }
        freqMap_[newFreq].push_back(node);
    }

    void evictLeastFrequent()
    {
        if (freqMap_.empty())
            return;

        // Find the minimum-frequency list.
        auto& minFreqList = freqMap_[minFreq_];
        if (minFreqList.empty())
            return;

        // Remove the least frequently used node.
        NodePtr leastNode = minFreqList.front();
        minFreqList.pop_front();

        // Remove the frequency bucket when its list is empty.
        if (minFreqList.empty())
        {
            freqMap_.erase(minFreq_);
            // Update the minimum frequency.
            if (!freqMap_.empty())
            {
                minFreq_ = freqMap_.begin()->first;
            }
        }

        // Move the node to the ghost cache.
        if (ghostCache_.size() >= ghostCapacity_)
        {
            removeOldestGhost();
        }
        addToGhost(leastNode);

        // Remove the node from the main cache.
        mainCache_.erase(leastNode->getKey());
    }

    void removeFromGhost(NodePtr node)
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
        node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // Clear the link after detaching the node.
        }
    }

    void addToGhost(NodePtr node)
    {
        node->next_ = ghostTail_;
        node->prev_ = ghostTail_->prev_;
        if (!ghostTail_->prev_.expired()) {
            ghostTail_->prev_.lock()->next_ = node;
        }
        ghostTail_->prev_ = node;
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost()
    {
        NodePtr oldestGhost = ghostHead_->next_;
        if (oldestGhost != ghostTail_)
        {
            removeFromGhost(oldestGhost);
            ghostCache_.erase(oldestGhost->getKey());
        }
    }

private:
    size_t capacity_;
    size_t ghostCapacity_;
    size_t transformThreshold_;
    size_t minFreq_;
    std::mutex mutex_;

    NodeMap mainCache_;
    NodeMap ghostCache_;
    FreqMap freqMap_;

    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace vcache
