#include <gtest/gtest.h>
#include "resilience/RateLimiter.hpp"
#include <thread>
#include <chrono>

using namespace gw::resilience;

TEST(RateLimiterTest, AllowsUpToCapacityThenBlocks) {
    TokenBucketRateLimiter limiter(/*capacity=*/3, /*refillPerSecond=*/0.0);
    EXPECT_TRUE(limiter.allow("client1"));
    EXPECT_TRUE(limiter.allow("client1"));
    EXPECT_TRUE(limiter.allow("client1"));
    EXPECT_FALSE(limiter.allow("client1")); // bucket exhausted, no refill
}

TEST(RateLimiterTest, TracksSeparateBucketsPerKey) {
    TokenBucketRateLimiter limiter(1, 0.0);
    EXPECT_TRUE(limiter.allow("clientA"));
    EXPECT_TRUE(limiter.allow("clientB")); // separate bucket, unaffected
    EXPECT_FALSE(limiter.allow("clientA"));
}

TEST(RateLimiterTest, RefillsOverTime) {
    TokenBucketRateLimiter limiter(1, /*refillPerSecond=*/1000.0); // fast refill for test speed
    EXPECT_TRUE(limiter.allow("client1"));
    EXPECT_FALSE(limiter.allow("client1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(limiter.allow("client1"));
}

TEST(RateLimiterTest, RefillDoesNotExceedCapacity) {
    TokenBucketRateLimiter limiter(2, 1000.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // way more than enough to overfill
    EXPECT_TRUE(limiter.allow("client1"));
    EXPECT_TRUE(limiter.allow("client1"));
    EXPECT_FALSE(limiter.allow("client1")); // capped at capacity=2, third should fail
}
