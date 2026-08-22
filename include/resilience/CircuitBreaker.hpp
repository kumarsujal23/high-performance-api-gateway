#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace gw::resilience {

// Classic three-state circuit breaker (Closed -> Open -> Half-Open -> Closed)
// per backend. Protects a struggling backend from being hammered with
// requests it will just time out on, and gives it a recovery window.
//
//  Closed:     requests pass through; failures are counted in a rolling
//              window. Too many failures -> Open.
//  Open:       requests are rejected immediately (fail fast) until the
//              reset timeout elapses -> Half-Open.
//  Half-Open:  a limited number of trial requests are allowed through. If
//              they succeed the breaker closes; if any fails it reopens.
enum class CircuitState { Closed, Open, HalfOpen };

class CircuitBreaker {
public:
    CircuitBreaker(int failureThreshold,
                    std::chrono::milliseconds resetTimeout,
                    int halfOpenTrialRequests = 1)
        : failureThreshold_(failureThreshold),
          resetTimeout_(resetTimeout),
          halfOpenTrialRequests_(halfOpenTrialRequests) {}

    // Call before dispatching a request. Returns false if the request should
    // be rejected (breaker open).
    bool allowRequest() {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ == CircuitState::Open) {
            auto now = std::chrono::steady_clock::now();
            if (now - openedAt_ >= resetTimeout_) {
                state_ = CircuitState::HalfOpen;
                halfOpenInFlight_ = 0;
            } else {
                return false;
            }
        }
        if (state_ == CircuitState::HalfOpen) {
            if (halfOpenInFlight_ >= halfOpenTrialRequests_) return false;
            ++halfOpenInFlight_;
        }
        return true;
    }

    void onSuccess() {
        std::lock_guard<std::mutex> lock(mu_);
        consecutiveFailures_ = 0;
        if (state_ == CircuitState::HalfOpen) {
            state_ = CircuitState::Closed;
            halfOpenInFlight_ = 0;
        }
    }

    void onFailure() {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ == CircuitState::HalfOpen) {
            // Trial failed: reopen immediately and restart the timer.
            trip();
            return;
        }
        ++consecutiveFailures_;
        if (consecutiveFailures_ >= failureThreshold_) {
            trip();
        }
    }

    CircuitState state() const {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

private:
    void trip() {
        state_ = CircuitState::Open;
        openedAt_ = std::chrono::steady_clock::now();
        consecutiveFailures_ = 0;
        halfOpenInFlight_ = 0;
    }

    int failureThreshold_;
    std::chrono::milliseconds resetTimeout_;
    int halfOpenTrialRequests_;

    mutable std::mutex mu_;
    CircuitState state_ = CircuitState::Closed;
    int consecutiveFailures_ = 0;
    int halfOpenInFlight_ = 0;
    std::chrono::steady_clock::time_point openedAt_{};
};

} // namespace gw::resilience
