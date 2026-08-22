#include "network/ConnectionPool.hpp"
#include "network/TcpServer.hpp"
#include "observability/Logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace gw::network {

int ConnectionPool::dialNew(const std::string& host, int port) const {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    setNonBlocking(fd);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc < 0) { // EINPROGRESS: wait for writability with timeout
        pollfd pfd{fd, POLLOUT, 0};
        int ready = ::poll(&pfd, 1, static_cast<int>(connectTimeout_.count()));
        if (ready <= 0) { close(fd); return -1; }
        int soError = 0;
        socklen_t len = sizeof(soError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        if (soError != 0) { close(fd); return -1; }
    }

    setTcpNoDelay(fd);
    return fd;
}

int ConnectionPool::acquire(const std::string& host, int port) {
    std::string k = key(host, port);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = idle_.find(k);
        if (it != idle_.end() && !it->second.empty()) {
            int fd = it->second.front();
            it->second.pop_front();
            return fd;
        }
    }
    return dialNew(host, port);
}

void ConnectionPool::release(const std::string& host, int port, int fd) {
    std::string k = key(host, port);
    std::lock_guard<std::mutex> lock(mu_);
    auto& dq = idle_[k];
    if (dq.size() >= maxIdlePerBackend_) {
        close(fd);
        return;
    }
    dq.push_back(fd);
}

void ConnectionPool::discard(int fd) {
    if (fd >= 0) close(fd);
}

size_t ConnectionPool::idleCount(const std::string& host, int port) const {
    std::string k = key(host, port);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = idle_.find(k);
    return it == idle_.end() ? 0 : it->second.size();
}

void ConnectionPool::closeAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [k, dq] : idle_) {
        for (int fd : dq) close(fd);
        dq.clear();
    }
}

} // namespace gw::network
