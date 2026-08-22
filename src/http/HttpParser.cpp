#include "http/HttpParser.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace gw::http {

std::string HttpParser::toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string HttpParser::trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool HttpParser::parseRequestLine(size_t& pos) {
    size_t eol = buffer_.find("\r\n", pos);
    if (eol == std::string::npos) {
        if (buffer_.size() > kMaxRequestLineLen) throw std::runtime_error("request line too long");
        return false; // need more data
    }
    std::string line = buffer_.substr(pos, eol - pos);
    pos = eol + 2;

    size_t sp1 = line.find(' ');
    size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == std::string::npos || sp1 == sp2) {
        throw std::runtime_error("malformed request line");
    }
    method_ = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    version_ = line.substr(sp2 + 1);

    size_t q = target.find('?');
    if (q == std::string::npos) {
        path_ = target;
        query_.clear();
    } else {
        path_ = target.substr(0, q);
        query_ = target.substr(q + 1);
    }
    if (path_.empty()) path_ = "/";
    return true;
}

bool HttpParser::parseHeaders(size_t& pos) {
    while (true) {
        size_t eol = buffer_.find("\r\n", pos);
        if (eol == std::string::npos) {
            if (buffer_.size() - pos > kMaxHeaderBytes) throw std::runtime_error("headers too large");
            return false;
        }
        if (eol == pos) { // blank line -> end of headers
            pos = eol + 2;
            return true;
        }
        std::string line = buffer_.substr(pos, eol - pos);
        pos = eol + 2;

        size_t colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("malformed header");
        std::string key = toLower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        headers_[key] = value;
    }
}

ParseStatus HttpParser::parse(HttpRequest& out) {
    try {
        size_t pos = 0;
        if (state_ == State::RequestLine) {
            if (!parseRequestLine(pos)) return ParseStatus::Incomplete;
            state_ = State::Headers;
            consumed_ = pos;
        }
        pos = consumed_;
        if (state_ == State::Headers) {
            if (!parseHeaders(pos)) return ParseStatus::Incomplete;
            consumed_ = pos;

            auto clIt = headers_.find("content-length");
            if (clIt != headers_.end()) {
                char* end = nullptr;
                const char* value = clIt->second.c_str();
                unsigned long parsed = std::strtoul(value, &end, 10);
                if (end == value || *end != '\0') throw std::runtime_error("invalid content length");
                contentLength_ = static_cast<size_t>(parsed);
                if (contentLength_ > kMaxBodyBytes) throw std::runtime_error("body too large");
            }
            auto teIt = headers_.find("transfer-encoding");
            chunked_ = (teIt != headers_.end() && toLower(teIt->second).find("chunked") != std::string::npos);
            if (chunked_) throw std::runtime_error("chunked transfer encoding is unsupported");
            state_ = State::Body;
        }
        if (state_ == State::Body) {
            if (buffer_.size() - consumed_ < contentLength_) return ParseStatus::Incomplete;
            out.body = buffer_.substr(consumed_, contentLength_);
            consumed_ += contentLength_;
        }

        out.method = method_;
        out.path = path_;
        out.query = query_;
        out.version = version_;
        out.headers = headers_;

        auto connIt = headers_.find("connection");
        if (connIt != headers_.end()) {
            out.keepAlive = toLower(connIt->second) != "close";
        } else {
            out.keepAlive = (version_ == "HTTP/1.1"); // 1.1 defaults to keep-alive
        }

        // Erase consumed bytes so a pipelined next request can be parsed.
        buffer_.erase(0, consumed_);
        state_ = State::RequestLine;
        consumed_ = 0;
        method_.clear(); path_.clear(); query_.clear(); version_.clear();
        headers_.clear();
        contentLength_ = 0;
        chunked_ = false;

        return ParseStatus::Complete;
    } catch (const std::exception&) {
        return ParseStatus::Error;
    }
}

} // namespace gw::http
