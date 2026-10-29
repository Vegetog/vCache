#pragma once

namespace vcache
{

template <typename Key, typename Value>
class CachePolicy
{
public:
    virtual ~CachePolicy() {};

    // Insert or update a cached value.
    virtual void put(Key key, Value value) = 0;

    // Return true on a hit and copy the cached value into the output parameter.
    virtual bool get(Key key, Value& value) = 0;
    // Return the cached value on a hit, or a default value on a miss.
    virtual Value get(Key key) = 0;

};

} // namespace vcache
