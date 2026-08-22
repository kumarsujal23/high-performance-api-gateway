#pragma once
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <chrono>

namespace gw::network {

// Pools persistent TCP connections to backend services, keyed by
// "host:port". Reusing connections avoids a TCP handshake (and TIME_WAIT
// buildup) on every proxied request, which matters a lot under load -- this
// is exactly what the benchmark's "connection reuse vs no reuse" comparison
// is meant to demonstrate.
//
// Design note: acquire() blocks the calling worker thread for up to
// `connectTimeout` while establishing a new connection if the pool is empty.
// This is a deliberate simplification (see README "Design Tradeoffs") that
// keeps the request-handling code linear and easy to explain; a fully async
// version would register the connecting fd with the event loop instead.
class ConnectionPool {
public:
    ConnectionPool(size_t maxIdlePerBackend, std::chrono::milliseconds connectTimeout)
        : maxIdlePerBackend_(maxIdlePerBackend), connectTimeout_(connectTimeout) {}

    // Returns a connected, non-blocking fd, or -1 on failure. Caller owns the
    // fd until release()/discard() is called.
    int acquire(const std::string& host, int port);

    // Return a still-healthy fd to the pool for reuse.
    void release(const std::string& host, int port, int fd);

    // Discard a broken fd (closes it, does not return to pool).
    void discard(int fd);

    size_t idleCount(const std::string& host, int port) const;

    void closeAll();

private:
    struct Key {
        std::string host;
        int port;
    };
    std::string key(const std::string& host, int port) const { return host + ":" + std::to_string(port); }

    int dialNew(const std::string& host, int port) const;

    size_t maxIdlePerBackend_;
    std::chrono::milliseconds connectTimeout_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::deque<int>> idle_;
};

} // namespace gw::network
