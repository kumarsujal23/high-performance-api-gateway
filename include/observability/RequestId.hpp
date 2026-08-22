#pragma once
#include <string>
#include <atomic>
#include <random>
#include <sstream>
#include <iomanip>

namespace gw::observability {

// Generates short, unique-enough request IDs for tracing a request through
// logs. Combines a per-process monotonic counter with a random suffix so IDs
// are unique across worker threads without needing a mutex-protected UUID lib.
class RequestIdGenerator {
public:
    static std::string next() {
        static std::atomic<uint64_t> counter{0};
        thread_local std::mt19937 rng{std::random_device{}()};
        thread_local std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFF);

        uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream oss;
        oss << "req-" << std::hex << seq << "-" << dist(rng);
        return oss.str();
    }
};

} // namespace gw::observability
