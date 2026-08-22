#include "network/TcpServer.hpp"
#include "observability/Logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>

namespace gw::network {

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void setTcpNoDelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

TcpServer::TcpServer(const std::string& host, int port, int backlog) {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) throw std::runtime_error("socket() failed");

    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }
    if (listen(listenFd_, backlog) < 0) {
        throw std::runtime_error(std::string("listen() failed: ") + strerror(errno));
    }
    setNonBlocking(listenFd_);
}

TcpServer::~TcpServer() {
    if (listenFd_ >= 0) close(listenFd_);
}

void TcpServer::acceptAll(const std::function<void(int, const std::string&)>& onAccept) {
    while (true) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        int clientFd = accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // no more pending
            if (errno == EINTR) continue;
            break;
        }
        setNonBlocking(clientFd);
        setTcpNoDelay(clientFd);
        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        onAccept(clientFd, std::string(ipBuf));
    }
}

} // namespace gw::network
