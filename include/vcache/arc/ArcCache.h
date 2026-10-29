#pragma once

#include "../CachePolicy.h"
#include "ArcLruPart.h"
#include "ArcLfuPart.h"
#include <memory>

namespace vcache
{

// Experimental: each part has its own capacity; global capacity and cross-part synchronization are not enforced.
// Excluded from standard ARC and equal-capacity benchmark comparisons.
template<typename Key, typename Value>
class ArcCache : public CachePolicy<Key, Value>
{
public:
    explicit ArcCache(size_t capacity = 10, size_t transformThreshold = 2)
        : capacity_(capacity)
        , transformThreshold_(transformThreshold)
        , lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold))
        , lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity, transformThreshold))
    {}

    ~ArcCache() override = default;

    void put(Key key, Value value) override
    {
        checkGhostCaches(key);

        // Check whether the key is present in the LFU part.
        bool inLfu = lfuPart_->contain(key);
        // Update the LRU part.
        lruPart_->put(key, value);
        // Update the LFU part when it already contains the key.
        if (inLfu)
        {
            lfuPart_->put(key, value);
        }
    }

    bool get(Key key, Value& value) override
    {
        checkGhostCaches(key);

        bool shouldTransform = false;
        if (lruPart_->get(key, value, shouldTransform))
        {
            if (shouldTransform)
            {
                lfuPart_->put(key, value);
            }
            return true;
        }
        return lfuPart_->get(key, value);
    }

    Value get(Key key) override
    {
        Value value{};
        get(key, value);
        return value;
    }

private:
    bool checkGhostCaches(Key key)
    {
        bool inGhost = false;
        if (lruPart_->checkGhost(key))
        {
            if (lfuPart_->decreaseCapacity())
            {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        }
        else if (lfuPart_->checkGhost(key))
        {
            if (lruPart_->decreaseCapacity())
            {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }
        return inGhost;
    }

private:
    size_t capacity_;
    size_t transformThreshold_;
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;
};

} // namespace vcache
