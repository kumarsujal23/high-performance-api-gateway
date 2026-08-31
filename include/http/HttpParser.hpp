#pragma once
#include "http/HttpRequest.hpp"
#include <string>
#include <cstddef>

namespace gw::http {

enum class ParseStatus {
    Incomplete,   // need more bytes
    Complete,     // full request parsed
    Error         // malformed request
};

// Incremental HTTP/1.1 request parser designed for non-blocking sockets:
// bytes arrive in arbitrary chunk sizes across multiple epoll wakeups, so the
// parser must be resumable rather than assuming a full request is available
// in one read(). It keeps an internal buffer and a small state machine
// (request line -> headers -> body) instead of using regex/streams, which
// keeps it allocation-light on the hot path.
class HttpParser {
public:
    // Feed newly-read bytes. Call parse() afterwards (or automatically here).
    void append(const char* data, size_t len) { buffer_.append(data, len); }

    // Attempt to parse a full request out of the buffer. On Complete, the
    // consumed bytes are erased from the buffer so pipelined/keep-alive
    // requests can be parsed one after another. On Incomplete, buffer is left
    // untouched so the caller can append more data and retry.
    ParseStatus parse(HttpRequest& out);

    void reset() { buffer_.clear(); state_ = State::RequestLine; consumed_ = 0; headers_.clear(); headerCount_ = 0; }

    size_t bufferedBytes() const { return buffer_.size(); }

    static constexpr size_t kMaxRequestLineLen = 8192;
    static constexpr size_t kMaxHeaderBytes = 32768;
    static constexpr size_t kMaxHeaderCount = 100;
    static constexpr size_t kMaxBodyBytes = 10 * 1024 * 1024; // 10MB

private:
    enum class State { RequestLine, Headers, Body, Done };

    std::string buffer_;
    State state_ = State::RequestLine;

    // Parsing progress preserved across resumptions.
    std::string method_, path_, query_, version_;
    std::unordered_map<std::string, std::string> headers_;
    size_t contentLength_ = 0;
    bool chunked_ = false;
    size_t headerCount_ = 0;
    size_t consumed_ = 0;

    bool parseRequestLine(size_t& pos);
    bool parseHeaders(size_t& pos);
    static std::string toLower(std::string s);
    static std::string trim(const std::string& s);
};

} // namespace gw::http
