#include "core/Gateway.hpp"
#include "config/Config.hpp"
#include "observability/Logger.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
std::unique_ptr<gw::core::Gateway> g_gateway;

void handleSignal(int) {
    if (g_gateway) g_gateway->stop();
}
} // namespace

int main(int argc, char** argv) {
    std::string configPath = "config/gateway.yaml";
    if (argc > 1) configPath = argv[1];

    gw::config::GatewayConfig cfg;
    try {
        cfg = gw::config::GatewayConfig::loadFromFile(configPath);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load config '" << configPath << "': " << e.what() << std::endl;
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN); // writes to a closed backend socket must not kill the process

    g_gateway = std::make_unique<gw::core::Gateway>(cfg);
    try {
        g_gateway->run();
    } catch (const std::exception& e) {
        gw::observability::Logger::instance().error(std::string("gateway crashed: ") + e.what());
        return 1;
    }
    return 0;
}
