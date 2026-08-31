// Simple, dependency-free HTTP load generator for benchmarking the gateway.
// Not a replacement for wrk/hey -- deliberately simple so every line of the
// measurement methodology is visible and explainable. Opens `connections`
// sockets (optionally reused across requests to demonstrate the effect of
// keep-alive/connection pooling) and fires requests for `durationSeconds`,
// recording latency per request.
//
// Usage: gateway_benchmark <host> <port> <path> <connections> <duration_s> [--no-keepalive]
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <array>

namespace {

struct WorkerStats {
    uint64_t successfulRequests = 0;
    uint64_t transportErrors = 0;
    std::array<uint64_t, 6> responsesByClass{};
    std::vector<double> latenciesMs;
};

int connectTo(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// Sends one GET request on fd and reads the full response. Returns false on
// any I/O error (caller should reconnect). If keepAlive is false, closes and
// returns fd = -1 via output param.
bool doRequest(int& fd, const std::string& host, int port, const std::string& path, bool keepAlive,
               int& statusCode) {
    if (fd < 0) {
        fd = connectTo(host, port);
        if (fd < 0) return false;
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: " +
                       (keepAlive ? "keep-alive" : "close") + "\r\n\r\n";
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { close(fd); fd = -1; return false; }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    long contentLength = -1;
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { close(fd); fd = -1; return false; }
        raw.append(buf, static_cast<size_t>(n));
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                size_t clPos = raw.find("Content-Length:");
                contentLength = (clPos != std::string::npos && clPos < headerEnd)
                                     ? std::strtol(raw.c_str() + clPos + 15, nullptr, 10) : 0;
            }
        }
        if (headerEnd != std::string::npos) {
            size_t bodyBytes = raw.size() - (headerEnd + 4);
            if (contentLength <= 0 || static_cast<long>(bodyBytes) >= contentLength) break;
        }
    }
    if (!keepAlive) { close(fd); fd = -1; }
    size_t firstSpace = raw.find(' ');
    if (firstSpace == std::string::npos || firstSpace + 4 > raw.size()) return false;
    statusCode = std::atoi(raw.c_str() + firstSpace + 1);
    return statusCode >= 100 && statusCode <= 599;
}

void workerLoop(const std::string& host, int port, const std::string& path, bool keepAlive,
                 std::chrono::steady_clock::time_point deadline, WorkerStats& stats) {
    int fd = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        auto start = std::chrono::steady_clock::now();
        int statusCode = 0;
        bool gotResponse = doRequest(fd, host, port, path, keepAlive, statusCode);
        auto end = std::chrono::steady_clock::now();
        if (gotResponse) {
            stats.responsesByClass[static_cast<size_t>(statusCode / 100)]++;
            if (statusCode >= 200 && statusCode < 300) {
                stats.successfulRequests++;
            }
            stats.latenciesMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        } else {
            stats.transportErrors++;
        }
    }
    if (fd >= 0) close(fd);
}

double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
    return sorted[idx];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: " << argv[0] << " <host> <port> <path> <connections> <duration_s> [--no-keepalive]\n";
        return 1;
    }
    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    std::string path = argv[3];
    int connections = std::atoi(argv[4]);
    int durationS = std::atoi(argv[5]);
    bool keepAlive = true;
    for (int i = 6; i < argc; ++i) if (std::string(argv[i]) == "--no-keepalive") keepAlive = false;

    std::cout << "Benchmarking " << host << ":" << port << path
              << " | connections=" << connections << " duration=" << durationS << "s"
              << " keepalive=" << (keepAlive ? "on" : "off") << std::endl;

    std::vector<WorkerStats> stats(connections);
    std::vector<std::thread> threads;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(durationS);
    auto wallStart = std::chrono::steady_clock::now();

    for (int i = 0; i < connections; ++i) {
        threads.emplace_back(workerLoop, host, port, path, keepAlive, deadline, std::ref(stats[i]));
    }
    for (auto& t : threads) t.join();
    double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();

    uint64_t totalSuccessfulRequests = 0, totalTransportErrors = 0;
    std::array<uint64_t, 6> responsesByClass{};
    std::vector<double> allLatencies;
    for (auto& s : stats) {
        totalSuccessfulRequests += s.successfulRequests;
        totalTransportErrors += s.transportErrors;
        for (size_t i = 1; i <= 5; ++i) responsesByClass[i] += s.responsesByClass[i];
        allLatencies.insert(allLatencies.end(), s.latenciesMs.begin(), s.latenciesMs.end());
    }
    std::sort(allLatencies.begin(), allLatencies.end());

    std::cout << "\n--- Results ---\n";
    uint64_t totalResponses = 0;
    for (size_t i = 1; i <= 5; ++i) totalResponses += responsesByClass[i];
    std::cout << "Total responses: " << totalResponses << "\n";
    std::cout << "Successful (2xx) responses: " << totalSuccessfulRequests << "\n";
    std::cout << "Transport errors: " << totalTransportErrors << "\n";
    for (size_t i = 1; i <= 5; ++i) {
        if (responsesByClass[i] > 0) std::cout << i << "xx responses: " << responsesByClass[i] << "\n";
    }
    std::cout << "Wall time: " << wallSeconds << "s\n";
    std::cout << "Successful throughput: " << (totalSuccessfulRequests / wallSeconds) << " req/s\n";
    if (!allLatencies.empty()) {
        std::cout << "Latency p50: " << percentile(allLatencies, 0.50) << " ms\n";
        std::cout << "Latency p95: " << percentile(allLatencies, 0.95) << " ms\n";
        std::cout << "Latency p99: " << percentile(allLatencies, 0.99) << " ms\n";
        std::cout << "Latency max: " << allLatencies.back() << " ms\n";
    }
    return 0;
}
