#pragma once
#include <string>
#include <vector>
#include <stdexcept>

namespace gw::config {

struct BackendConfig {
    std::string host;
    int port;
};

struct RouteConfig {
    std::string pathPrefix;
    std::string backendGroup; // name referencing a group of backends below
    bool cacheable = false;
};

struct BackendGroupConfig {
    std::string name;
    std::string strategy = "round_robin"; // or "least_connections"
    std::vector<BackendConfig> backends;
};

struct GatewayConfig {
    std::string listenHost = "0.0.0.0";
    int listenPort = 8080;
    int workerThreads = 4;

    int rateLimitCapacity = 100;
    double rateLimitRefillPerSecond = 50.0;

    size_t cacheCapacity = 1000;
    int cacheTtlMs = 5000;

    int circuitFailureThreshold = 5;
    int circuitResetTimeoutMs = 10000;

    int retryMax = 2;
    int retryBaseDelayMs = 50;
    int retryMaxDelayMs = 1000;

    int connectTimeoutMs = 1000;
    int backendReadTimeoutMs = 3000;
    size_t maxIdleConnsPerBackend = 16;

    int healthCheckIntervalMs = 5000;
    int healthCheckTimeoutMs = 1000;
    std::string healthCheckPath = "/health";

    std::vector<BackendGroupConfig> backendGroups;
    std::vector<RouteConfig> routes;

    int metricsPort = 9090;

    static GatewayConfig loadFromFile(const std::string& path);
};

} // namespace gw::config
