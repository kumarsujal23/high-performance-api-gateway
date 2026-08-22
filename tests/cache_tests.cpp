#include <gtest/gtest.h>
#include "cache/LRUCache.hpp"
#include <thread>
#include <chrono>

using namespace gw::cache;

TEST(LRUCacheTest, PutThenGetReturnsValue) {
    LRUCache<std::string> cache(2);
    cache.put("a", "1");
    auto v = cache.get("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "1");
}

TEST(LRUCacheTest, MissingKeyReturnsNullopt) {
    LRUCache<std::string> cache(2);
    EXPECT_FALSE(cache.get("missing").has_value());
}

TEST(LRUCacheTest, EvictsLeastRecentlyUsedWhenOverCapacity) {
    LRUCache<std::string> cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3"); // should evict "a" (least recently used)
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
    EXPECT_TRUE(cache.get("c").has_value());
}

TEST(LRUCacheTest, GetRefreshesRecency) {
    LRUCache<std::string> cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.get("a");        // "a" is now most recently used
    cache.put("c", "3");   // should evict "b", not "a"
    EXPECT_TRUE(cache.get("a").has_value());
    EXPECT_FALSE(cache.get("b").has_value());
}

TEST(LRUCacheTest, EntriesExpireAfterTtl) {
    LRUCache<std::string> cache(10, std::chrono::milliseconds(20));
    cache.put("a", "1");
    EXPECT_TRUE(cache.get("a").has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(cache.get("a").has_value());
}

TEST(LRUCacheTest, InvalidateRemovesEntry) {
    LRUCache<std::string> cache(10);
    cache.put("a", "1");
    cache.invalidate("a");
    EXPECT_FALSE(cache.get("a").has_value());
}

TEST(LRUCacheTest, PutOverwritesExistingKey) {
    LRUCache<std::string> cache(10);
    cache.put("a", "1");
    cache.put("a", "2");
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(*cache.get("a"), "2");
}
