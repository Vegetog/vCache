#pragma once

#include "ArcCacheNode.h"
#include <unordered_map>
#include <mutex>

namespace vcache
{

template<typename Key, typename Value>
class ArcLruPart
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    explicit ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
    {
        initializeLists();
    }

    ~ArcLruPart()
    {
        unlinkList(mainHead_);
        unlinkList(ghostHead_);
    }

    bool put(Key key, Value value)
    {
        if (capacity_ == 0) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    bool get(Key key, Value& value, bool& shouldTransform)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
        {
            shouldTransform = updateNodeAccess(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    bool checkGhost(Key key)
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) {
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
        if (mainCache_.size() == capacity_) {
            evictLeastRecent();
        }
        --capacity_;
        return true;
    }

private:
    static void unlinkList(NodePtr& head)
    {
        auto node = std::move(head);
        while (node)
        {
            auto next = std::move(node->next_);
            node->prev_.reset();
            node = std::move(next);
        }
    }

    void initializeLists()
    {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value)
    {
        node->setValue(value);
        moveToFront(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value)
    {
        if (mainCache_.size() >= capacity_)
        {
            evictLeastRecent(); // Evict the least recently used node.
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        addToFront(newNode);
        return true;
    }

    bool updateNodeAccess(NodePtr node)
    {
        moveToFront(node);
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;
    }

    void moveToFront(NodePtr node)
    {
        // Detach the node from its current position.
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // Clear the link after detaching the node.
        }

        // Insert at the front.
        addToFront(node);
    }

    void addToFront(NodePtr node)
    {
        node->next_ = mainHead_->next_;
        node->prev_ = mainHead_;
        mainHead_->next_->prev_ = node;
        mainHead_->next_ = node;
    }

    void evictLeastRecent()
    {
        NodePtr leastRecent = mainTail_->prev_.lock();
        if (!leastRecent || leastRecent == mainHead_)
            return;

        // Detach from the main list.
        removeFromMain(leastRecent);

        // Add the node to the ghost cache.
        if (ghostCache_.size() >= ghostCapacity_)
        {
            removeOldestGhost();
        }
        addToGhost(leastRecent);

        // Remove the node from the main-cache index.
        mainCache_.erase(leastRecent->getKey());
    }

    void removeFromMain(NodePtr node)
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // Clear the link after detaching the node.
        }
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
        // Reset the node access count.
        node->accessCount_ = 1;

        // Insert at the front of the ghost list.
        node->next_ = ghostHead_->next_;
        node->prev_ = ghostHead_;
        ghostHead_->next_->prev_ = node;
        ghostHead_->next_ = node;

        // Add the node to the ghost-cache index.
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost()
    {
        // Lock the weak pointer and check the result.
        NodePtr oldestGhost = ghostTail_->prev_.lock();
        if (!oldestGhost || oldestGhost == ghostHead_)
            return;

        removeFromGhost(oldestGhost);
        ghostCache_.erase(oldestGhost->getKey());
    }


private:
    size_t capacity_;
    size_t ghostCapacity_;
    size_t transformThreshold_; // Promotion threshold.
    std::mutex mutex_;

    NodeMap mainCache_; // key -> ArcNode
    NodeMap ghostCache_;

    // Main list.
    NodePtr mainHead_;
    NodePtr mainTail_;
    // Ghost list.
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace vcache
