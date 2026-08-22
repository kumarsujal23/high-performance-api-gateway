#pragma once
#include <chrono>
#include <random>
#include <algorithm>
#include <cmath>

namespace gw::resilience {

// Bounded retry with exponential backoff + full jitter, following the
// well-known AWS architecture blog formula:
//   backoff = min(cap, base * 2^attempt)
//   sleep   = random(0, backoff)
// Full jitter (rather than fixed or equal jitter) avoids retry storms where
// many clients back off in lockstep and then all retry at the same instant.
class RetryPolicy {
public:
    RetryPolicy(int maxRetries,
                std::chrono::milliseconds baseDelay,
                std::chrono::milliseconds maxDelay)
        : maxRetries_(maxRetries), baseDelay_(baseDelay), maxDelay_(maxDelay) {}

    int maxRetries() const { return maxRetries_; }

    // attempt: 0 for the first retry, 1 for the second, etc.
    std::chrono::milliseconds nextDelay(int attempt) const {
        double base = static_cast<double>(baseDelay_.count());
        double cap = static_cast<double>(maxDelay_.count());
        double backoff = std::min(cap, base * std::pow(2.0, attempt));

        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<double> dist(0.0, backoff);
        return std::chrono::milliseconds(static_cast<long long>(dist(rng)));
    }

    static bool isRetryable(int httpStatusOrErrno) {
        // Retry on connection failures (negative sentinel) and 502/503/504.
        if (httpStatusOrErrno < 0) return true;
        return httpStatusOrErrno == 502 || httpStatusOrErrno == 503 || httpStatusOrErrno == 504;
    }

private:
    int maxRetries_;
    std::chrono::milliseconds baseDelay_;
    std::chrono::milliseconds maxDelay_;
};

} // namespace gw::resilience
