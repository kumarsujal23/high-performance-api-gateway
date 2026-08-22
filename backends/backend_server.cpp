// Lightweight test backend used for local development and integration tests.
// Not part of the gateway itself -- a minimal, dependency-free HTTP/1.1
// server so the gateway has something real to proxy to. Supports optional
// chaos flags to exercise the gateway's retry/circuit-breaker logic:
//   ./backend_server <port> [--latency-ms N] [--fail-rate P] [--name NAME]
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>

namespace {
std::atomic<uint64_t> g_requestCount{0};

void handleClient(int fd, int latencyMs, double failRate, std::string name, int port) {
    char buf[8192];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    std::string request(buf);

    std::string path = "/";
    size_t sp1 = request.find(' ');
    size_t sp2 = request.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
        path = request.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    uint64_t count = g_requestCount.fetch_add(1) + 1;

    if (latencyMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(latencyMs));

    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    bool shouldFail = (path != "/health") && (failRate > 0.0) && (dist(rng) < failRate);

    std::ostringstream body;
    int status = 200;
    std::string statusText = "OK";
    if (shouldFail) {
        status = 500;
        statusText = "Internal Server Error";
        body << "{\"error\":\"simulated failure\",\"backend\":\"" << name << ":" << port << "\"}";
    } else {
        body << "{\"backend\":\"" << name << ":" << port << "\",\"path\":\"" << path
             << "\",\"request_count\":" << count << "}";
    }

    std::string bodyStr = body.str();
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << statusText << "\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << bodyStr.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << bodyStr;
    std::string respStr = resp.str();
    send(fd, respStr.data(), respStr.size(), 0);
    close(fd);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <port> [--latency-ms N] [--fail-rate P] [--name NAME]\n";
        return 1;
    }
    int port = std::atoi(argv[1]);
    int latencyMs = 0;
    double failRate = 0.0;
    std::string name = "backend";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--latency-ms" && i + 1 < argc) latencyMs = std::atoi(argv[++i]);
        else if (arg == "--fail-rate" && i + 1 < argc) failRate = std::atof(argv[++i]);
        else if (arg == "--name" && i + 1 < argc) name = argv[++i];
    }

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed on port " << port << ": " << strerror(errno) << std::endl;
        return 1;
    }
    listen(listenFd, 512);

    std::cout << "[" << name << "] listening on port " << port
              << " (latency=" << latencyMs << "ms, fail_rate=" << failRate << ")" << std::endl;

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        int clientFd = accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (clientFd < 0) continue;
        std::thread(handleClient, clientFd, latencyMs, failRate, name, port).detach();
    }
    return 0;
}
