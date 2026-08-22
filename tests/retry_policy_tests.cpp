#include <gtest/gtest.h>
#include "resilience/RetryPolicy.hpp"

using namespace gw::resilience;

TEST(RetryPolicyTest, DelayNeverExceedsMax) {
    RetryPolicy policy(5, std::chrono::milliseconds(10), std::chrono::milliseconds(200));
    for (int attempt = 0; attempt < 10; ++attempt) {
        auto d = policy.nextDelay(attempt);
        EXPECT_LE(d.count(), 200);
        EXPECT_GE(d.count(), 0);
    }
}

TEST(RetryPolicyTest, DelayGrowsExponentiallyOnAverage) {
    RetryPolicy policy(5, std::chrono::milliseconds(10), std::chrono::milliseconds(100000));
    // Full jitter means individual samples are noisy, so compare an average
    // over many samples instead of single draws.
    auto avgAt = [&](int attempt) {
        long long sum = 0;
        constexpr int kSamples = 200;
        for (int i = 0; i < kSamples; ++i) sum += policy.nextDelay(attempt).count();
        return static_cast<double>(sum) / kSamples;
    };
    double avg0 = avgAt(0);
    double avg4 = avgAt(4);
    EXPECT_GT(avg4, avg0 * 2); // attempt 4 backoff (base*16) should clearly exceed attempt 0 (base*1)
}

TEST(RetryPolicyTest, IsRetryableForServerErrorsAndConnectionFailures) {
    EXPECT_TRUE(RetryPolicy::isRetryable(-1));  // connection error sentinel
    EXPECT_TRUE(RetryPolicy::isRetryable(502));
    EXPECT_TRUE(RetryPolicy::isRetryable(503));
    EXPECT_TRUE(RetryPolicy::isRetryable(504));
    EXPECT_FALSE(RetryPolicy::isRetryable(200));
    EXPECT_FALSE(RetryPolicy::isRetryable(404));
    EXPECT_FALSE(RetryPolicy::isRetryable(400));
}
