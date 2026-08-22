#include "config/Config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

// This is a deliberately minimal parser for the small YAML *subset* used by
// gateway.yaml (2-space indentation, "key: value" pairs, and "- " list
// items). It is NOT a general-purpose YAML parser -- pulling in a full YAML
// library would add a dependency for very little benefit given the config's
// fixed, simple shape. Every line is trimmed and classified by indentation
// level + whether it starts a list item, which keeps the state machine easy
// to follow.

namespace gw::config {
namespace {

struct Line {
    int indent;
    std::string content; // trimmed, may start with "- "
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string stripComment(const std::string& s) {
    size_t h = s.find('#');
    return h == std::string::npos ? s : s.substr(0, h);
}

std::vector<Line> tokenize(std::istream& in) {
    std::vector<Line> lines;
    std::string raw;
    while (std::getline(in, raw)) {
        std::string noComment = stripComment(raw);
        // count leading spaces for indentation
        int indent = 0;
        while (indent < static_cast<int>(noComment.size()) && noComment[indent] == ' ') ++indent;
        std::string content = trim(noComment);
        if (content.empty()) continue;
        lines.push_back({indent, content});
    }
    return lines;
}

// Splits "key: value" -> {key, value}. value may be empty (nested block follows).
std::pair<std::string, std::string> splitKv(const std::string& s) {
    size_t colon = s.find(':');
    if (colon == std::string::npos) return {trim(s), ""};
    std::string key = trim(s.substr(0, colon));
    std::string val = trim(s.substr(colon + 1));
    return {key, val};
}

int toInt(const std::string& s, int def) {
    try { return s.empty() ? def : std::stoi(s); } catch (...) { return def; }
}
double toDouble(const std::string& s, double def) {
    try { return s.empty() ? def : std::stod(s); } catch (...) { return def; }
}

} // namespace

GatewayConfig GatewayConfig::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open config file: " + path);
    }
    auto lines = tokenize(file);
    GatewayConfig cfg;

    // Track which top-level section we're in, based on indentation resets.
    std::string section; // "listen", "rate_limit", "cache", "circuit_breaker",
                          // "retry", "connection", "health_check",
                          // "backend_groups", "routes"

    BackendGroupConfig* curGroup = nullptr;
    RouteConfig* curRoute = nullptr;
    bool inBackendsList = false;
    int groupIndent = -1;       // indent of the current "- name: ..." group marker
    int routeListIndent = -1;   // indent of "- path_prefix: ..." route markers

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& ln = lines[i];

        if (ln.indent == 0) {
            curGroup = nullptr;
            curRoute = nullptr;
            inBackendsList = false;
            groupIndent = -1;
            routeListIndent = -1;
            auto [key, val] = splitKv(ln.content);
            if (key == "workers") { cfg.workerThreads = toInt(val, cfg.workerThreads); section.clear(); continue; }
            if (key == "metrics_port") { cfg.metricsPort = toInt(val, cfg.metricsPort); section.clear(); continue; }
            section = key; // e.g. "listen", "backend_groups", "routes"
            continue;
        }

        if (section == "listen") {
            auto [key, val] = splitKv(ln.content);
            if (key == "host") cfg.listenHost = val;
            else if (key == "port") cfg.listenPort = toInt(val, cfg.listenPort);
        } else if (section == "rate_limit") {
            auto [key, val] = splitKv(ln.content);
            if (key == "capacity") cfg.rateLimitCapacity = toInt(val, cfg.rateLimitCapacity);
            else if (key == "refill_per_second") cfg.rateLimitRefillPerSecond = toDouble(val, cfg.rateLimitRefillPerSecond);
        } else if (section == "cache") {
            auto [key, val] = splitKv(ln.content);
            if (key == "capacity") cfg.cacheCapacity = static_cast<size_t>(toInt(val, static_cast<int>(cfg.cacheCapacity)));
            else if (key == "ttl_ms") cfg.cacheTtlMs = toInt(val, cfg.cacheTtlMs);
        } else if (section == "circuit_breaker") {
            auto [key, val] = splitKv(ln.content);
            if (key == "failure_threshold") cfg.circuitFailureThreshold = toInt(val, cfg.circuitFailureThreshold);
            else if (key == "reset_timeout_ms") cfg.circuitResetTimeoutMs = toInt(val, cfg.circuitResetTimeoutMs);
        } else if (section == "retry") {
            auto [key, val] = splitKv(ln.content);
            if (key == "max_retries") cfg.retryMax = toInt(val, cfg.retryMax);
            else if (key == "base_delay_ms") cfg.retryBaseDelayMs = toInt(val, cfg.retryBaseDelayMs);
            else if (key == "max_delay_ms") cfg.retryMaxDelayMs = toInt(val, cfg.retryMaxDelayMs);
        } else if (section == "connection") {
            auto [key, val] = splitKv(ln.content);
            if (key == "connect_timeout_ms") cfg.connectTimeoutMs = toInt(val, cfg.connectTimeoutMs);
            else if (key == "backend_read_timeout_ms") cfg.backendReadTimeoutMs = toInt(val, cfg.backendReadTimeoutMs);
            else if (key == "max_idle_per_backend") cfg.maxIdleConnsPerBackend = static_cast<size_t>(toInt(val, static_cast<int>(cfg.maxIdleConnsPerBackend)));
        } else if (section == "health_check") {
            auto [key, val] = splitKv(ln.content);
            if (key == "interval_ms") cfg.healthCheckIntervalMs = toInt(val, cfg.healthCheckIntervalMs);
            else if (key == "timeout_ms") cfg.healthCheckTimeoutMs = toInt(val, cfg.healthCheckTimeoutMs);
            else if (key == "path") cfg.healthCheckPath = val;
        } else if (section == "backend_groups") {
            bool isListItem = ln.content.rfind("- ", 0) == 0;
            if (isListItem && (groupIndent == -1 || ln.indent == groupIndent)) {
                // A "- " marker at the group-list indent level starts a new group.
                groupIndent = ln.indent;
                cfg.backendGroups.push_back({});
                curGroup = &cfg.backendGroups.back();
                inBackendsList = false;
                auto [key, val] = splitKv(ln.content.substr(2));
                if (key == "name") curGroup->name = val;
            } else if (isListItem && curGroup && inBackendsList && ln.indent > groupIndent) {
                // A deeper "- " marker under "backends:" starts a new backend entry.
                curGroup->backends.push_back({});
                auto [k2, v2] = splitKv(ln.content.substr(2));
                if (k2 == "host") curGroup->backends.back().host = v2;
            } else if (curGroup) {
                auto [key, val] = splitKv(ln.content);
                if (key == "name") curGroup->name = val;
                else if (key == "strategy") curGroup->strategy = val;
                else if (key == "backends") inBackendsList = true;
                else if (inBackendsList && key == "host" && !curGroup->backends.empty()) {
                    curGroup->backends.back().host = val;
                } else if (inBackendsList && key == "port" && !curGroup->backends.empty()) {
                    curGroup->backends.back().port = toInt(val, 0);
                }
            }
        } else if (section == "routes") {
            bool isListItem = ln.content.rfind("- ", 0) == 0;
            if (isListItem && (routeListIndent == -1 || ln.indent <= routeListIndent)) {
                routeListIndent = ln.indent;
                cfg.routes.push_back({});
                curRoute = &cfg.routes.back();
                auto [key, val] = splitKv(ln.content.substr(2));
                if (key == "path_prefix") curRoute->pathPrefix = val;
            } else if (curRoute) {
                auto [key, val] = splitKv(ln.content);
                if (key == "path_prefix") curRoute->pathPrefix = val;
                else if (key == "backend_group") curRoute->backendGroup = val;
                else if (key == "cacheable") curRoute->cacheable = (val == "true");
            }
        }
    }

    return cfg;
}

} // namespace gw::config
