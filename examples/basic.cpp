#include <iostream>
#include <string>

#include <vcache/LruCache.h>

int main()
{
    vcache::LruCache<int, std::string> cache(2);
    cache.put(1, "hello");
    cache.put(2, "vCache");

    std::string value;
    if (cache.get(1, value))
        std::cout << value << '\n';

    cache.put(3, "cache"); // Retain recently accessed key 1 and evict key 2.
    return cache.get(2, value) ? 1 : 0;
}
