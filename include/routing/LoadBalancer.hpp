#pragma once
#include "resilience/CircuitBreaker.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>

namespace gw::routing {

struct Backend {
    std::string host;
    int port;
    std::atomic<bool> healthy{true};
    std::atomic<int> activeConnections{0};
    // Each backend gets its own circuit breaker so one bad backend in a
    // group doesn't trip requests destined for its healthy siblings.
    std::shared_ptr<gw::resilience::CircuitBreaker> circuitBreaker;

    std::string address() const { return host + ":" + std::to_string(port); }
};

enum class LbStrategy { RoundRobin, LeastConnections };

// Chooses a healthy backend from a fixed pool. Two strategies are
// implemented because they represent the two dominant real-world approaches
// and are cheap to compare: Round Robin (O(1), no runtime signal, assumes
// uniform request cost) vs Least Connections (tracks in-flight load, better
// under non-uniform request costs but requires bookkeeping on every
// request start/finish).
class LoadBalancer {
public:
    explicit LoadBalancer(std::vector<std::shared_ptr<Backend>> backends,
                           LbStrategy strategy = LbStrategy::RoundRobin)
        : backends_(std::move(backends)), strategy_(strategy) {}

    std::shared_ptr<Backend> pick() {
        if (backends_.empty()) return nullptr;
        switch (strategy_) {
            case LbStrategy::RoundRobin: return pickRoundRobin();
            case LbStrategy::LeastConnections: return pickLeastConnections();
        }
        return nullptr;
    }

    const std::vector<std::shared_ptr<Backend>>& backends() const { return backends_; }
    LbStrategy strategy() const { return strategy_; }

private:
    std::shared_ptr<Backend> pickRoundRobin() {
        size_t n = backends_.size();
        for (size_t i = 0; i < n; ++i) {
            size_t idx = rrCounter_.fetch_add(1, std::memory_order_relaxed) % n;
            if (backends_[idx]->healthy.load(std::memory_order_relaxed)) return backends_[idx];
        }
        return nullptr; // all unhealthy
    }

    std::shared_ptr<Backend> pickLeastConnections() {
        std::shared_ptr<Backend> best = nullptr;
        int bestConns = -1;
        for (auto& b : backends_) {
            if (!b->healthy.load(std::memory_order_relaxed)) continue;
            int conns = b->activeConnections.load(std::memory_order_relaxed);
            if (best == nullptr || conns < bestConns) {
                best = b;
                bestConns = conns;
            }
        }
        return best;
    }

    std::vector<std::shared_ptr<Backend>> backends_;
    LbStrategy strategy_;
    std::atomic<size_t> rrCounter_{0};
};

} // namespace gw::routing
