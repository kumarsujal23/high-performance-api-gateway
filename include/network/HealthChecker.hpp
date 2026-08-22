#pragma once
#include "routing/LoadBalancer.hpp"
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace gw::network {

// Runs a background thread that periodically performs a simple HTTP GET
// against each backend's health endpoint (default "/health") and flips
// Backend::healthy accordingly. Kept on its own thread (rather than folded
// into the epoll loop) because it is low-frequency and independent of
// request traffic -- simplest possible design that is still correct.
class HealthChecker {
public:
    HealthChecker(std::vector<std::shared_ptr<gw::routing::Backend>> backends,
                   std::chrono::milliseconds interval,
                   std::chrono::milliseconds timeout,
                   std::string path = "/health")
        : backends_(std::move(backends)), interval_(interval), timeout_(timeout), path_(std::move(path)) {}

    ~HealthChecker() { stop(); }

    void start();
    void stop();

private:
    void run();
    bool checkOne(const gw::routing::Backend& b) const;

    std::vector<std::shared_ptr<gw::routing::Backend>> backends_;
    std::chrono::milliseconds interval_;
    std::chrono::milliseconds timeout_;
    std::string path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    // A condition variable (rather than plain sleep_for) lets stop() wake the
    // health-check thread immediately instead of waiting out the full
    // interval, which matters for fast, deterministic shutdown/tests.
    std::mutex cvMu_;
    std::condition_variable cv_;
};

} // namespace gw::network
