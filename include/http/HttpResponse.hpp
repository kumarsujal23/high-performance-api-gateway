#pragma once
#include <string>
#include <vector>
#include <utility>
#include <sstream>

namespace gw::http {

struct HttpResponse {
    int statusCode = 200;
    std::string statusText = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    void setHeader(const std::string& k, const std::string& v) {
        for (auto& h : headers) {
            if (h.first == k) { h.second = v; return; }
        }
        headers.emplace_back(k, v);
    }

    static HttpResponse make(int code, const std::string& text, std::string body_ = "") {
        HttpResponse r;
        r.statusCode = code;
        r.statusText = text;
        r.body = std::move(body_);
        return r;
    }

    // Serialize into wire format. Content-Length is always computed here so
    // callers never have to keep it in sync with body mutations by hand.
    std::string serialize(bool keepAlive) const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
        bool hasContentLength = false, hasConnection = false;
        for (auto& h : headers) {
            if (h.first == "Content-Length") hasContentLength = true;
            if (h.first == "Connection") hasConnection = true;
            oss << h.first << ": " << h.second << "\r\n";
        }
        if (!hasContentLength) oss << "Content-Length: " << body.size() << "\r\n";
        if (!hasConnection) oss << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
        oss << "\r\n" << body;
        return oss.str();
    }
};

} // namespace gw::http
