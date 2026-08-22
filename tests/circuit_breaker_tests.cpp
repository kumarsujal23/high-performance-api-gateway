#include <gtest/gtest.h>
#include "resilience/CircuitBreaker.hpp"
#include <thread>
#include <chrono>

using namespace gw::resilience;

TEST(CircuitBreakerTest, StartsClosed) {
    CircuitBreaker cb(3, std::chrono::milliseconds(100));
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_TRUE(cb.allowRequest());
}

TEST(CircuitBreakerTest, OpensAfterThresholdFailures) {
    CircuitBreaker cb(3, std::chrono::milliseconds(1000));
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(cb.allowRequest());
        cb.onFailure();
    }
    EXPECT_EQ(cb.state(), CircuitState::Open);
    EXPECT_FALSE(cb.allowRequest());
}

TEST(CircuitBreakerTest, SuccessResetsFailureCount) {
    CircuitBreaker cb(3, std::chrono::milliseconds(1000));
    cb.onFailure();
    cb.onFailure();
    cb.onSuccess(); // resets consecutive failure count
    cb.onFailure();
    cb.onFailure();
    EXPECT_EQ(cb.state(), CircuitState::Closed); // only 2 consecutive since reset
}

TEST(CircuitBreakerTest, TransitionsToHalfOpenAfterTimeout) {
    CircuitBreaker cb(1, std::chrono::milliseconds(30));
    cb.onFailure(); // trips open
    EXPECT_EQ(cb.state(), CircuitState::Open);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(cb.allowRequest()); // should transition to half-open and allow trial
    EXPECT_EQ(cb.state(), CircuitState::HalfOpen);
}

TEST(CircuitBreakerTest, HalfOpenSuccessCloses) {
    CircuitBreaker cb(1, std::chrono::milliseconds(20), 1);
    cb.onFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(cb.allowRequest());
    cb.onSuccess();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
}

TEST(CircuitBreakerTest, HalfOpenFailureReopens) {
    CircuitBreaker cb(1, std::chrono::milliseconds(20), 1);
    cb.onFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(cb.allowRequest());
    cb.onFailure();
    EXPECT_EQ(cb.state(), CircuitState::Open);
}

TEST(CircuitBreakerTest, HalfOpenLimitsTrialRequests) {
    CircuitBreaker cb(1, std::chrono::milliseconds(20), /*halfOpenTrialRequests=*/1);
    cb.onFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(cb.allowRequest());  // consumes the single trial slot
    EXPECT_FALSE(cb.allowRequest()); // no more trial slots until resolved
}
