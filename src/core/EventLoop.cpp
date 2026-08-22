#include "core/EventLoop.hpp"
#include "observability/Logger.hpp"
#include <unistd.h>
#include <stdexcept>
#include <vector>
#include <cstring>

namespace gw::core {

EventLoop::EventLoop() {
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1 failed: ") + strerror(errno));
    }
}

EventLoop::~EventLoop() {
    if (epollFd_ >= 0) close(epollFd_);
}

void EventLoop::add(int fd, uint32_t events, Callback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl ADD failed: ") + strerror(errno));
    }
    callbacks_[fd] = std::move(cb);
}

void EventLoop::modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl MOD failed: ") + strerror(errno));
    }
}

void EventLoop::remove(int fd) {
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

int EventLoop::poll(int timeoutMs) {
    static thread_local std::vector<epoll_event> events(kMaxEvents);
    int n = epoll_wait(epollFd_, events.data(), kMaxEvents, timeoutMs);
    if (n < 0) {
        if (errno == EINTR) return 0;
        throw std::runtime_error(std::string("epoll_wait failed: ") + strerror(errno));
    }
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) {
            it->second(events[i].events);
        }
    }
    if (tickCb_) tickCb_();
    return n;
}

} // namespace gw::core
