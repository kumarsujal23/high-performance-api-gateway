#pragma once
#include <string>
#include <unordered_map>
#include <optional>

namespace gw::http {

struct HttpRequest {
    std::string method;       // GET, POST, ...
    std::string path;         // /foo/bar  (without query string)
    std::string query;        // raw query string, may be empty
    std::string version;      // HTTP/1.1
    std::unordered_map<std::string, std::string> headers; // lower-cased keys
    std::string body;

    bool keepAlive = true;
    std::string requestId;    // assigned by the gateway, not the client

    std::optional<std::string> header(const std::string& lowerKey) const {
        auto it = headers.find(lowerKey);
        if (it == headers.end()) return std::nullopt;
        return it->second;
    }

    // Cache key: method + path + query. Only used for GET/HEAD by the caller.
    std::string cacheKey() const {
        return method + " " + path + (query.empty() ? "" : ("?" + query));
    }
};

} // namespace gw::http
