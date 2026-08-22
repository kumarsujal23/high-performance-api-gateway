#include "network/HealthChecker.hpp"
#include "network/TcpServer.hpp"
#include "observability/Logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

namespace gw::network {

void HealthChecker::start() {
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void HealthChecker::stop() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool HealthChecker::checkOne(const gw::routing::Backend& b) const {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(b.port);
    if (getaddrinfo(b.host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) return false;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }
    setNonBlocking(fd);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) { close(fd); return false; }
    if (rc < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, static_cast<int>(timeout_.count())) <= 0) { close(fd); return false; }
        int soError = 0; socklen_t len = sizeof(soError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        if (soError != 0) { close(fd); return false; }
    }

    std::ostringstream req;
    req << "GET " << path_ << " HTTP/1.1\r\nHost: " << b.host << "\r\nConnection: close\r\n\r\n";
    std::string reqStr = req.str();
    ssize_t sent = send(fd, reqStr.data(), reqStr.size(), 0);
    if (sent < 0) { close(fd); return false; }

    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(timeout_.count())) <= 0) { close(fd); return false; }
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    std::string resp(buf);
    // Consider any 2xx a healthy response.
    return resp.find("HTTP/1.1 2") != std::string::npos || resp.find("HTTP/1.0 2") != std::string::npos;
}

void HealthChecker::run() {
    using namespace gw::observability;
    while (running_) {
        for (auto& b : backends_) {
            bool ok = checkOne(*b);
            bool was = b->healthy.exchange(ok, std::memory_order_relaxed);
            if (was != ok) {
                Logger::instance().info(ok ? "backend became healthy" : "backend became unhealthy",
                                          "\"backend\":\"" + b->address() + "\"");
            }
        }
        std::unique_lock<std::mutex> lock(cvMu_);
        cv_.wait_for(lock, interval_, [this] { return !running_.load(); });
    }
}

} // namespace gw::network
