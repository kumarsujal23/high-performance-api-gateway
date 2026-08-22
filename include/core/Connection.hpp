#pragma once
#include "http/HttpParser.hpp"
#include <string>
#include <chrono>

namespace gw::core {

// Per-client-connection state kept alive across epoll wakeups. A client may
// send a request across several read() calls (parser handles that) and a
// response may need several write() calls if the socket buffer is full
// (pendingWrite/writeOffset handle that). One of these exists per accepted
// client fd for the lifetime of that (possibly keep-alive, multi-request)
// connection.
struct ClientConnection {
    int fd = -1;
    std::string clientIp;
    gw::http::HttpParser parser;

    std::string pendingWrite;
    size_t writeOffset = 0;

    bool closeAfterWrite = false; // set when the current response should end the connection
    std::chrono::steady_clock::time_point lastActivity;

    bool hasPendingWrite() const { return writeOffset < pendingWrite.size(); }
};

} // namespace gw::core
