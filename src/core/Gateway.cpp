#include "core/Gateway.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "observability/RequestId.hpp"
#include "network/TcpServer.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iostream>

using namespace gw::observability;
using namespace gw::http;
using namespace gw::routing;
using namespace gw::resilience;
using namespace gw::network;
using namespace gw::config;

namespace gw::core {

// ---------------------------------------------------------------------------
// Worker: one epoll loop + one listening socket (SO_REUSEPORT) per thread.
// Handles the full lifecycle of client connections: accept, incremental
// parse, pipeline through rate-limit -> cache -> route -> load-balance ->
// forward-to-backend -> respond.
// ---------------------------------------------------------------------------
class Gateway::Worker {
public:
    Worker(Gateway& gw, int id) : gw_(gw), id_(id) {}

    void run() {
        TcpServer server(gw_.cfg_.listenHost, gw_.cfg_.listenPort);
        loop_.add(server.fd(), EPOLLIN, [this, &server](uint32_t) {
            server.acceptAll([this](int fd, const std::string& ip) { onAccept(fd, ip); });
        });
        loop_.setTickCallback([this] { sweepIdleConnections(); });

        Logger::instance().info("worker started",
            "\"worker_id\":" + std::to_string(id_) + ",\"port\":" + std::to_string(gw_.cfg_.listenPort));

        while (gw_.running_.load(std::memory_order_relaxed)) {
            loop_.poll(200 /*ms*/);
        }

        std::vector<int> openFds;
        openFds.reserve(conns_.size());
        for (const auto& [fd, conn] : conns_) openFds.push_back(fd);
        for (int fd : openFds) closeConnection(fd);
    }

private:
    Gateway& gw_;
    int id_;
    EventLoop loop_;
    std::unordered_map<int, ClientConnection> conns_;

    void onAccept(int fd, const std::string& ip) {
        Metrics::instance().connectionOpened();
        ClientConnection conn;
        conn.fd = fd;
        conn.clientIp = ip;
        conn.lastActivity = std::chrono::steady_clock::now();
        conns_[fd] = std::move(conn);
        loop_.add(fd, EPOLLIN, [this, fd](uint32_t events) { onEvent(fd, events); });
    }

    void onEvent(int fd, uint32_t events) {
        if (events & (EPOLLHUP | EPOLLERR)) { closeConnection(fd); return; }
        if (events & EPOLLIN) onReadable(fd);
        auto it = conns_.find(fd);
        if (it == conns_.end()) return; // connection was closed while handling read
        if (events & EPOLLOUT) onWritable(fd);
    }

    void onReadable(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        ClientConnection& conn = it->second;

        char buf[16384];
        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                conn.parser.append(buf, static_cast<size_t>(n));
                conn.lastActivity = std::chrono::steady_clock::now();
                if (static_cast<size_t>(n) < sizeof(buf)) break; // drained socket buffer
                continue;
            }
            if (n == 0) { closeConnection(fd); return; } // peer closed
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            closeConnection(fd);
            return;
        }

        // Drain as many complete pipelined requests as are buffered.
        while (true) {
            HttpRequest req;
            ParseStatus status = conn.parser.parse(req);
            if (status == ParseStatus::Incomplete) break;
            if (status == ParseStatus::Error) {
                queueResponse(conn, HttpResponse::make(400, "Bad Request", "malformed request"), true);
                break;
            }
            req.requestId = RequestIdGenerator::next();
            handleRequest(conn, req);
            if (conn.closeAfterWrite) break;
        }

        flushIfPossible(fd);
    }

    void handleRequest(ClientConnection& conn, HttpRequest& req) {
        auto startTs = std::chrono::steady_clock::now();
        Metrics::instance().incRequests();

        Logger::instance().info("request received",
            "\"request_id\":\"" + req.requestId + "\",\"method\":\"" + req.method +
            "\",\"path\":\"" + req.path + "\",\"client_ip\":\"" + conn.clientIp + "\"");

        // Built-in observability endpoint, served directly by the gateway
        // (never proxied) so metrics remain available even if every backend
        // is down.
        if (req.path == "/metrics") {
            double uptime = std::chrono::duration<double>(std::chrono::steady_clock::now() - gw_.startTime_).count();
            HttpResponse resp = HttpResponse::make(200, "OK", Metrics::instance().toPrometheusText(uptime));
            resp.setHeader("Content-Type", "text/plain; version=0.0.4");
            recordLatency(startTs);
            queueResponse(conn, resp, !req.keepAlive);
            return;
        }
        if (req.path == "/gateway/health") {
            recordLatency(startTs);
            queueResponse(conn, HttpResponse::make(200, "OK", "{\"status\":\"ok\"}"), !req.keepAlive);
            return;
        }

        if (!gw_.rateLimiter_.allow(conn.clientIp)) {
            Metrics::instance().incRateLimited();
            recordLatency(startTs);
            queueResponse(conn, HttpResponse::make(429, "Too Many Requests", "rate limit exceeded"), !req.keepAlive);
            return;
        }

        const routing::Route* route = gw_.router_.match(req.path);
        if (!route) {
            Metrics::instance().incErrors();
            recordLatency(startTs);
            queueResponse(conn, HttpResponse::make(404, "Not Found", "no matching route"), !req.keepAlive);
            return;
        }

        bool cacheableRequest = route->cacheable && (req.method == "GET" || req.method == "HEAD");
        std::string cacheKey = req.cacheKey();
        if (cacheableRequest) {
            auto cached = gw_.cache_.get(cacheKey);
            if (cached.has_value()) {
                Metrics::instance().incCacheHit();
                HttpResponse resp = HttpResponse::make(cached->statusCode, cached->statusText, cached->body);
                resp.headers = cached->headers;
                resp.setHeader("X-Cache", "HIT");
                resp.setHeader("X-Request-Id", req.requestId);
                recordLatency(startTs);
                queueResponse(conn, resp, !req.keepAlive);
                return;
            }
            Metrics::instance().incCacheMiss();
        }

        HttpResponse resp = gw_.forwardWithResilience(*route, req);
        resp.setHeader("X-Request-Id", req.requestId);
        if (cacheableRequest && resp.statusCode == 200) {
            resp.setHeader("X-Cache", "MISS");
            gw_.cache_.put(cacheKey, CachedResponse{resp.statusCode, resp.statusText, resp.headers, resp.body});
        }
        if (resp.statusCode >= 500) Metrics::instance().incErrors();

        recordLatency(startTs);
        queueResponse(conn, resp, !req.keepAlive);
    }

    static void recordLatency(std::chrono::steady_clock::time_point start) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        Metrics::instance().recordLatencyMicros(static_cast<uint64_t>(us));
    }

    void queueResponse(ClientConnection& conn, const HttpResponse& resp, bool closeAfter) {
        Metrics::instance().recordResponse(resp.statusCode);
        conn.pendingWrite += resp.serialize(!closeAfter);
        conn.closeAfterWrite = closeAfter;
    }

    void flushIfPossible(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        ClientConnection& conn = it->second;
        if (!conn.hasPendingWrite()) return;

        while (conn.hasPendingWrite()) {
            const char* data = conn.pendingWrite.data() + conn.writeOffset;
            size_t remaining = conn.pendingWrite.size() - conn.writeOffset;
            ssize_t n = ::send(fd, data, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                conn.writeOffset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                loop_.modify(fd, EPOLLIN | EPOLLOUT); // wait for socket to drain
                return;
            }
            closeConnection(fd);
            return;
        }

        // Fully flushed.
        conn.pendingWrite.clear();
        conn.writeOffset = 0;
        if (conn.closeAfterWrite) {
            closeConnection(fd);
        } else {
            loop_.modify(fd, EPOLLIN);
        }
    }

    void onWritable(int fd) { flushIfPossible(fd); }

    void closeConnection(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        loop_.remove(fd);
        ::close(fd);
        conns_.erase(it);
        Metrics::instance().connectionClosed();
    }

    // Closes idle keep-alive connections that have sent no bytes recently, to
    // bound memory/fd usage under slow-client or dead-peer conditions.
    void sweepIdleConnections() {
        static constexpr auto kIdleTimeout = std::chrono::seconds(60);
        auto now = std::chrono::steady_clock::now();
        std::vector<int> toClose;
        for (auto& [fd, conn] : conns_) {
            if (!conn.hasPendingWrite() && (now - conn.lastActivity) > kIdleTimeout) {
                toClose.push_back(fd);
            }
        }
        for (int fd : toClose) closeConnection(fd);
    }
};

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

Gateway::Gateway(GatewayConfig cfg)
    : cfg_(std::move(cfg)),
      rateLimiter_(cfg_.rateLimitCapacity, cfg_.rateLimitRefillPerSecond),
      retryPolicy_(cfg_.retryMax, std::chrono::milliseconds(cfg_.retryBaseDelayMs), std::chrono::milliseconds(cfg_.retryMaxDelayMs)),
      cache_(cfg_.cacheCapacity, std::chrono::milliseconds(cfg_.cacheTtlMs)),
      pool_(cfg_.maxIdleConnsPerBackend, std::chrono::milliseconds(cfg_.connectTimeoutMs)) {
    buildRouter();
    healthChecker_ = std::make_unique<HealthChecker>(
        allBackends_,
        std::chrono::milliseconds(cfg_.healthCheckIntervalMs),
        std::chrono::milliseconds(cfg_.healthCheckTimeoutMs),
        cfg_.healthCheckPath);
}

Gateway::~Gateway() { stop(); }

void Gateway::runMetricsServer() {
    try {
        TcpServer server(cfg_.listenHost, cfg_.metricsPort);
        Logger::instance().info("metrics server started", "\"port\":" + std::to_string(cfg_.metricsPort));
        while (running_.load(std::memory_order_relaxed)) {
            pollfd pfd{server.fd(), POLLIN, 0};
            int ready = ::poll(&pfd, 1, 200);
            if (ready <= 0) continue;
            server.acceptAll([this](int fd, const std::string&) {
                double uptime = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_).count();
                HttpResponse response = HttpResponse::make(200, "OK", Metrics::instance().toPrometheusText(uptime));
                response.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
                response.setHeader("Connection", "close");
                std::string wire = response.serialize(false);
                size_t sent = 0;
                while (sent < wire.size()) {
                    ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
                    if (n <= 0) break;
                    sent += static_cast<size_t>(n);
                }
                ::close(fd);
            });
        }
    } catch (const std::exception& e) {
        Logger::instance().error(std::string("metrics server stopped: ") + e.what());
    }
}

void Gateway::buildRouter() {
    for (auto& group : cfg_.backendGroups) {
        std::vector<std::shared_ptr<Backend>> backends;
        for (auto& b : group.backends) {
            auto backend = std::make_shared<Backend>();
            backend->host = b.host;
            backend->port = b.port;
            backend->circuitBreaker = std::make_shared<CircuitBreaker>(
                cfg_.circuitFailureThreshold, std::chrono::milliseconds(cfg_.circuitResetTimeoutMs));
            backends.push_back(backend);
            allBackends_.push_back(backend);
        }
        LbStrategy strategy = (group.strategy == "least_connections") ? LbStrategy::LeastConnections : LbStrategy::RoundRobin;
        loadBalancers_[group.name] = std::make_shared<LoadBalancer>(backends, strategy);
    }

    for (auto& r : cfg_.routes) {
        auto it = loadBalancers_.find(r.backendGroup);
        if (it == loadBalancers_.end()) {
            Logger::instance().warn("route references unknown backend group",
                "\"path_prefix\":\"" + r.pathPrefix + "\",\"backend_group\":\"" + r.backendGroup + "\"");
            continue;
        }
        router_.addRoute(Route{r.pathPrefix, it->second, r.cacheable});
    }
}

// ----- Backend I/O -----------------------------------------------------
// Deliberately synchronous (blocking-with-timeout via poll()) from the
// worker thread's point of view -- see ConnectionPool's header comment for
// why this tradeoff was made. Only the request currently being forwarded is
// blocked; other client connections on other worker threads are unaffected.

namespace {

std::string buildBackendRequest(const HttpRequest& req, const Backend& backend) {
    std::ostringstream oss;
    oss << req.method << " " << req.path << (req.query.empty() ? "" : "?" + req.query) << " HTTP/1.1\r\n";
    oss << "Host: " << backend.host << "\r\n";
    for (auto& [k, v] : req.headers) {
        // RFC 9110 hop-by-hop fields must never be forwarded verbatim.
        if (k == "connection" || k == "host" || k == "keep-alive" || k == "proxy-connection" ||
            k == "te" || k == "trailer" || k == "transfer-encoding" || k == "upgrade" ||
            k == "content-length") continue;
        oss << k << ": " << v << "\r\n";
    }
    oss << "X-Request-Id: " << req.requestId << "\r\n";
    oss << "X-Forwarded-For: " << "gateway" << "\r\n";
    oss << "Connection: keep-alive\r\n";
    if (!req.body.empty()) oss << "Content-Length: " << req.body.size() << "\r\n";
    oss << "\r\n" << req.body;
    return oss.str();
}

// Returns false on any I/O error or timeout. On success, fills statusCode
// and body via a tiny inline status-line/header/body reader (separate from
// HttpParser, which is client-request-shaped).
bool isConnectionClose(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.find("close") != std::string::npos;
}

// Reads a length-delimited HTTP/1.x response. Chunked upstream responses are
// deliberately rejected: the gateway does not dechunk, so forwarding one
// would produce incorrect client framing.
bool sendAndReceive(int fd, const std::string& request, int timeoutMs, size_t maxBodyBytes,
                    HttpResponse& out, bool& reusable) {
    reusable = false;
    size_t sent = 0;
    while (sent < request.size()) {
        pollfd pfd{fd, POLLOUT, 0};
        int ready = ::poll(&pfd, 1, timeoutMs);
        if (ready <= 0) return false;
        ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;
    bool gotContentLength = false;

    while (true) {
        int remainingMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (remainingMs <= 0) return false;
        pollfd pfd{fd, POLLIN, 0};
        int ready = ::poll(&pfd, 1, remainingMs);
        if (ready <= 0) return false;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        raw.append(buf, static_cast<size_t>(n));

        if (headerEnd == std::string::npos) {
            if (raw.size() > 32768) return false;
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                size_t lineStart = raw.find("\r\n") + 2;
                while (lineStart < headerEnd) {
                    size_t lineEnd = raw.find("\r\n", lineStart);
                    if (lineEnd == std::string::npos || lineEnd > headerEnd) return false;
                    std::string line = raw.substr(lineStart, lineEnd - lineStart);
                    size_t colon = line.find(':');
                    if (colon == std::string::npos) return false;
                    std::string key = line.substr(0, colon);
                    std::string value = line.substr(colon + 1);
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
                    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
                    if (key == "content-length") {
                        if (gotContentLength) return false;
                        const char* first = value.data();
                        const char* last = first + value.size();
                        auto [end, ec] = std::from_chars(first, last, contentLength);
                        if (ec != std::errc{} || end != last || contentLength > maxBodyBytes) return false;
                        gotContentLength = true;
                    } else if (key == "transfer-encoding") {
                        return false;
                    } else if (key == "connection") {
                        reusable = !isConnectionClose(value);
                    } else if (key != "keep-alive" && key != "proxy-connection" && key != "upgrade") {
                        out.headers.emplace_back(std::move(key), std::move(value));
                    }
                    lineStart = lineEnd + 2;
                }
                if (!gotContentLength) return false;
            }
        }
        if (headerEnd != std::string::npos) {
            size_t bodyBytes = raw.size() - (headerEnd + 4);
            if (bodyBytes >= contentLength) break;
        }
    }

    if (headerEnd == std::string::npos) return false;
    // Parse status line.
    size_t firstSpace = raw.find(' ');
    size_t secondSpace = raw.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) return false;
    out.statusCode = std::atoi(raw.substr(firstSpace + 1, secondSpace - firstSpace - 1).c_str());
    size_t lineEnd = raw.find("\r\n");
    out.statusText = raw.substr(secondSpace + 1, lineEnd - secondSpace - 1);
    out.body = raw.substr(headerEnd + 4, contentLength);
    return true;
}

} // namespace

HttpResponse Gateway::forwardWithResilience(const routing::Route& route, HttpRequest& req) {
    int attempts = retryPolicy_.maxRetries() + 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto backend = route.lb->pick();
        if (!backend) {
            return HttpResponse::make(503, "Service Unavailable", "no healthy backends");
        }
        if (!backend->circuitBreaker->allowRequest()) {
            Metrics::instance().incCircuitOpen();
            continue; // try picking again (may land on a different, closed-breaker backend)
        }

        backend->activeConnections.fetch_add(1, std::memory_order_relaxed);
        int fd = pool_.acquire(backend->host, backend->port);
        HttpResponse resp;
        bool ok = false;
        bool reusable = false;
        if (fd >= 0) {
            std::string wire = buildBackendRequest(req, *backend);
            ok = sendAndReceive(fd, wire, cfg_.backendReadTimeoutMs, cfg_.maxResponseBodyBytes, resp, reusable);
        }
        backend->activeConnections.fetch_sub(1, std::memory_order_relaxed);

        if (ok) {
            backend->circuitBreaker->onSuccess();
            if (reusable) pool_.release(backend->host, backend->port, fd);
            else pool_.discard(fd);
            if (resp.statusCode < 500) return resp;
            // 5xx counts as a resilience-relevant failure for retry purposes
            // but we still return it if we've exhausted retries below.
            if (attempt == attempts - 1) return resp;
            if (!RetryPolicy::isRetryable(resp.statusCode)) return resp;
        } else {
            if (fd >= 0) pool_.discard(fd);
            backend->circuitBreaker->onFailure();
            if (attempt == attempts - 1) {
                return HttpResponse::make(502, "Bad Gateway", "backend request failed");
            }
        }

        Metrics::instance().incRetries();
        auto delay = retryPolicy_.nextDelay(attempt);
        Logger::instance().warn("retrying backend request",
            "\"request_id\":\"" + req.requestId + "\",\"attempt\":" + std::to_string(attempt + 1) +
            ",\"delay_ms\":" + std::to_string(delay.count()));
        std::this_thread::sleep_for(delay);
    }
    return HttpResponse::make(503, "Service Unavailable", "all retries exhausted");
}

void Gateway::run() {
    running_ = true;
    startTime_ = std::chrono::steady_clock::now();
    healthChecker_->start();
    metricsThread_ = std::thread([this] { runMetricsServer(); });

    Logger::instance().info("gateway starting",
        "\"port\":" + std::to_string(cfg_.listenPort) + ",\"workers\":" + std::to_string(cfg_.workerThreads));

    for (int i = 0; i < cfg_.workerThreads; ++i) {
        workerThreads_.emplace_back([this, i] {
            Worker w(*this, i);
            w.run();
        });
    }
    for (auto& t : workerThreads_) t.join();
    workerThreads_.clear();
    if (metricsThread_.joinable()) metricsThread_.join();
    if (healthChecker_) healthChecker_->stop();
    pool_.closeAll();
}

void Gateway::stop() {
    if (!running_.exchange(false)) return;
    if (healthChecker_) healthChecker_->stop();
}

} // namespace gw::core
