#pragma once
#include "config/Config.hpp"
#include "core/EventLoop.hpp"
#include "core/Connection.hpp"
#include "http/HttpRequest.hpp"
#include "http/HttpResponse.hpp"
#include "routing/Router.hpp"
#include "resilience/RateLimiter.hpp"
#include "resilience/RetryPolicy.hpp"
#include "cache/LRUCache.hpp"
#include "network/ConnectionPool.hpp"
#include "network/TcpServer.hpp"
#include "network/HealthChecker.hpp"

#include <thread>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace gw::core {

// A cached response is the same shape as an HttpResponse but decoupled from
// it so cache entries don't accidentally carry connection-specific headers.
struct CachedResponse {
    int statusCode;
    std::string statusText;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// Gateway owns every shared subsystem (router, rate limiter, cache,
// connection pool, health checker) and spawns `workerThreads` independent
// Worker instances. Each Worker has its own epoll loop and its own listening
// socket bound with SO_REUSEPORT on the same port -- the kernel spreads
// incoming connections across workers, so there is no shared accept lock and
// no cross-thread hand-off of fds. Shared subsystems below are internally
// thread-safe (mutexes / atomics), which is the only cross-thread
// synchronization this design needs.
class Gateway {
public:
    explicit Gateway(gw::config::GatewayConfig cfg);
    ~Gateway();

    void run();   // blocks until stop() is called from another thread/signal
    void stop();

private:
    class Worker;
    friend class Worker;

    void buildRouter();
    gw::http::HttpResponse forwardWithResilience(const gw::routing::Route& route, gw::http::HttpRequest& req);

    gw::config::GatewayConfig cfg_;
    gw::routing::Router router_;
    std::vector<std::shared_ptr<gw::routing::Backend>> allBackends_;
    std::unordered_map<std::string, std::shared_ptr<gw::routing::LoadBalancer>> loadBalancers_;

    gw::resilience::TokenBucketRateLimiter rateLimiter_;
    gw::resilience::RetryPolicy retryPolicy_;
    gw::cache::LRUCache<CachedResponse> cache_;
    gw::network::ConnectionPool pool_;
    std::unique_ptr<gw::network::HealthChecker> healthChecker_;

    std::vector<std::thread> workerThreads_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point startTime_;
};

} // namespace gw::core
