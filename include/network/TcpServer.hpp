#pragma once
#include <string>
#include <functional>

namespace gw::network {

// Creates a non-blocking TCP listening socket. SO_REUSEADDR and SO_REUSEPORT
// are both set: REUSEPORT lets multiple worker threads each own an
// independent listening socket on the same port, with the kernel
// load-balancing accept()s across them -- this is the standard high
// performance multi-threaded accept pattern (used by nginx, envoy) and
// avoids a single shared accept-mutex bottleneck.
class TcpServer {
public:
    TcpServer(const std::string& host, int port, int backlog = 1024);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    int fd() const { return listenFd_; }

    // Accept as many pending connections as available (non-blocking accept
    // loop). Calls onAccept(clientFd, clientAddrString) for each.
    void acceptAll(const std::function<void(int, const std::string&)>& onAccept);

private:
    int listenFd_ = -1;
};

// Sets O_NONBLOCK on a socket fd.
void setNonBlocking(int fd);
// Disables Nagle's algorithm so small proxied writes aren't held back.
void setTcpNoDelay(int fd);

} // namespace gw::network
