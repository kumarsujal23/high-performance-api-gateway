#pragma once
#include <sys/epoll.h>
#include <functional>
#include <unordered_map>
#include <cstdint>

namespace gw::core {

// Thin, explicit wrapper around Linux epoll (level-triggered). Kept small on
// purpose: an interview candidate should be able to explain every line.
// Each registered fd has a callback invoked with the epoll event mask that
// fired (EPOLLIN, EPOLLOUT, EPOLLHUP, EPOLLERR).
class EventLoop {
public:
    using Callback = std::function<void(uint32_t events)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Registers fd for the given event mask (e.g. EPOLLIN). Level-triggered
    // by default (no EPOLLET) so a partial read/write next iteration will
    // re-fire, which is simpler to reason about than edge-triggered mode at
    // the cost of a few extra epoll_wait wakeups.
    void add(int fd, uint32_t events, Callback cb);
    void modify(int fd, uint32_t events);
    void remove(int fd);

    // Runs one iteration: epoll_wait with timeoutMs, then dispatches
    // callbacks. Returns number of events processed. Also invokes any
    // registered periodic tick callback once per iteration (used for timeout
    // sweeps / health checks) regardless of whether events fired.
    int poll(int timeoutMs);

    void setTickCallback(std::function<void()> cb) { tickCb_ = std::move(cb); }

    int fd() const { return epollFd_; }

private:
    int epollFd_;
    std::unordered_map<int, Callback> callbacks_;
    std::function<void()> tickCb_;
    static constexpr int kMaxEvents = 256;
};

} // namespace gw::core
