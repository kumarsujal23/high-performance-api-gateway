#pragma once
#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstdint>
#include <string>
#include <sstream>
#include <array>

namespace gw::observability {

// A minimal fixed-bucket histogram for latency percentiles (p50/p95/p99).
// We avoid a full HDR-histogram implementation for explainability: bucket
// boundaries are explicit and the percentile math is easy to walk through
// on a whiteboard.
class LatencyHistogram {
public:
    // Bucket upper bounds in microseconds.
    LatencyHistogram()
        : bounds_{100, 250, 500, 1000, 2500, 5000, 10000, 25000,
                  50000, 100000, 250000, 500000, 1000000,
                  UINT64_MAX},
          counts_(bounds_.size()) {} // std::atomic default-constructs to 0 (C++20)

    void record(uint64_t micros) {
        auto it = std::lower_bound(bounds_.begin(), bounds_.end(), micros);
        size_t idx = static_cast<size_t>(std::distance(bounds_.begin(), it));
        if (idx >= counts_.size()) idx = counts_.size() - 1;
        counts_[idx].fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(1, std::memory_order_relaxed);
    }

    // Returns approximate percentile in microseconds (upper bound of bucket).
    uint64_t percentile(double p) const {
        uint64_t total = total_.load(std::memory_order_relaxed);
        if (total == 0) return 0;
        uint64_t target = static_cast<uint64_t>(p * static_cast<double>(total));
        uint64_t cumulative = 0;
        for (size_t i = 0; i < counts_.size(); ++i) {
            cumulative += counts_[i].load(std::memory_order_relaxed);
            if (cumulative >= target) {
                return bounds_[i] == UINT64_MAX ? bounds_[i - 1] : bounds_[i];
            }
        }
        return bounds_.back();
    }

    uint64_t p50() const { return percentile(0.50); }
    uint64_t p95() const { return percentile(0.95); }
    uint64_t p99() const { return percentile(0.99); }
    uint64_t count() const { return total_.load(std::memory_order_relaxed); }

private:
    std::vector<uint64_t> bounds_;
    std::vector<std::atomic<uint64_t>> counts_;
    std::atomic<uint64_t> total_{0};
};

// Central metrics registry. Counters use relaxed atomics because we only
// need eventual consistency for reporting, never ordering guarantees with
// other memory operations.
class Metrics {
public:
    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    void incRequests() { requests_.fetch_add(1, std::memory_order_relaxed); }
    void incErrors() { errors_.fetch_add(1, std::memory_order_relaxed); }
    void incCacheHit() { cacheHits_.fetch_add(1, std::memory_order_relaxed); }
    void incCacheMiss() { cacheMisses_.fetch_add(1, std::memory_order_relaxed); }
    void incRateLimited() { rateLimited_.fetch_add(1, std::memory_order_relaxed); }
    void incRetries() { retries_.fetch_add(1, std::memory_order_relaxed); }
    void incCircuitOpen() { circuitOpenRejections_.fetch_add(1, std::memory_order_relaxed); }
    void recordResponse(int statusCode) {
        if (statusCode >= 100 && statusCode < 600) {
            responsesByClass_[static_cast<size_t>(statusCode / 100)].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void connectionOpened() { activeConnections_.fetch_add(1, std::memory_order_relaxed); }
    void connectionClosed() { activeConnections_.fetch_sub(1, std::memory_order_relaxed); }

    void recordLatencyMicros(uint64_t micros) { latency_.record(micros); }

    uint64_t requests() const { return requests_.load(std::memory_order_relaxed); }
    uint64_t errors() const { return errors_.load(std::memory_order_relaxed); }
    int64_t activeConnections() const { return activeConnections_.load(std::memory_order_relaxed); }

    // Rate of requests per second since process start (simple, explainable).
    double requestsPerSecond(double elapsedSeconds) const {
        if (elapsedSeconds <= 0) return 0.0;
        return static_cast<double>(requests()) / elapsedSeconds;
    }

    std::string toPrometheusText(double uptimeSeconds) const {
        std::ostringstream oss;
        oss << "# HELP gateway_requests_total Total requests handled\n";
        oss << "# TYPE gateway_requests_total counter\n";
        oss << "gateway_requests_total " << requests() << "\n";
        oss << "# TYPE gateway_errors_total counter\n";
        oss << "gateway_errors_total " << errors() << "\n";
        oss << "# TYPE gateway_responses_total counter\n";
        for (size_t klass = 1; klass <= 5; ++klass) {
            oss << "gateway_responses_total{status_class=\"" << klass << "xx\"} "
                << responsesByClass_[klass].load(std::memory_order_relaxed) << "\n";
        }
        oss << "gateway_cache_hits_total " << cacheHits_.load() << "\n";
        oss << "gateway_cache_misses_total " << cacheMisses_.load() << "\n";
        oss << "gateway_rate_limited_total " << rateLimited_.load() << "\n";
        oss << "gateway_retries_total " << retries_.load() << "\n";
        oss << "gateway_circuit_open_rejections_total " << circuitOpenRejections_.load() << "\n";
        oss << "gateway_active_connections " << activeConnections() << "\n";
        oss << "gateway_requests_per_second " << requestsPerSecond(uptimeSeconds) << "\n";
        oss << "gateway_latency_p50_micros " << latency_.p50() << "\n";
        oss << "gateway_latency_p95_micros " << latency_.p95() << "\n";
        oss << "gateway_latency_p99_micros " << latency_.p99() << "\n";
        return oss.str();
    }

    const LatencyHistogram& latencyHistogram() const { return latency_; }

private:
    Metrics() = default;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint64_t> cacheHits_{0};
    std::atomic<uint64_t> cacheMisses_{0};
    std::atomic<uint64_t> rateLimited_{0};
    std::atomic<uint64_t> retries_{0};
    std::atomic<uint64_t> circuitOpenRejections_{0};
    std::atomic<int64_t> activeConnections_{0};
    std::array<std::atomic<uint64_t>, 6> responsesByClass_{};
    LatencyHistogram latency_;
};

} // namespace gw::observability
