#pragma once
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <string>

namespace gw::resilience {

// Per-key token bucket rate limiter. One bucket per client key (e.g. source
// IP). Token bucket (vs. fixed window) is chosen because it allows short
// bursts up to `capacity` while enforcing a steady-state `refillRate` — the
// standard trade-off discussed in interviews vs. sliding-window counters.
class TokenBucketRateLimiter {
public:
    TokenBucketRateLimiter(double capacity, double refillPerSecond)
        : capacity_(capacity), refillPerSecond_(refillPerSecond) {}

    // Returns true if the request is allowed (a token was consumed).
    bool allow(const std::string& key, double cost = 1.0) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        auto& bucket = buckets_[key];
        if (bucket.lastRefill.time_since_epoch().count() == 0) {
            bucket.tokens = capacity_;
            bucket.lastRefill = now;
        } else {
            double elapsed = std::chrono::duration<double>(now - bucket.lastRefill).count();
            bucket.tokens = std::min(capacity_, bucket.tokens + elapsed * refillPerSecond_);
            bucket.lastRefill = now;
        }
        if (bucket.tokens >= cost) {
            bucket.tokens -= cost;
            return true;
        }
        return false;
    }

    size_t trackedKeys() const {
        std::lock_guard<std::mutex> lock(mu_);
        return buckets_.size();
    }

private:
    struct Bucket {
        double tokens = 0;
        std::chrono::steady_clock::time_point lastRefill{};
    };

    double capacity_;
    double refillPerSecond_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace gw::resilience
