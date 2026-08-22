#include <gtest/gtest.h>
#include "routing/Router.hpp"
#include "routing/LoadBalancer.hpp"

using namespace gw::routing;

namespace {
std::shared_ptr<LoadBalancer> makeLb() {
    std::vector<std::shared_ptr<Backend>> backends;
    auto b = std::make_shared<Backend>();
    b->host = "127.0.0.1";
    b->port = 9000;
    backends.push_back(b);
    return std::make_shared<LoadBalancer>(backends);
}
} // namespace

TEST(RouterTest, MatchesExactPrefix) {
    Router router;
    router.addRoute(Route{"/api/users", makeLb(), true});
    auto* r = router.match("/api/users");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->pathPrefix, "/api/users");
}

TEST(RouterTest, MatchesSubpath) {
    Router router;
    router.addRoute(Route{"/api/users", makeLb(), false});
    auto* r = router.match("/api/users/42/profile");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->pathPrefix, "/api/users");
}

TEST(RouterTest, PrefersLongestPrefixMatch) {
    Router router;
    router.addRoute(Route{"/api", makeLb(), false});
    router.addRoute(Route{"/api/users/admin", makeLb(), false});
    auto* r = router.match("/api/users/admin/settings");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->pathPrefix, "/api/users/admin");
}

TEST(RouterTest, ReturnsNullWhenNoMatch) {
    Router router;
    router.addRoute(Route{"/api/users", makeLb(), false});
    EXPECT_EQ(router.match("/other"), nullptr);
}

TEST(RouterTest, DoesNotMatchPartialPathSegment) {
    Router router;
    router.addRoute(Route{"/api", makeLb(), false});
    EXPECT_EQ(router.match("/apix"), nullptr);
}

TEST(RouterTest, RootRouteMatchesEveryPath) {
    Router router;
    router.addRoute(Route{"/", makeLb(), false});
    EXPECT_NE(router.match("/anything"), nullptr);
}

TEST(LoadBalancerTest, RoundRobinCyclesThroughBackends) {
    std::vector<std::shared_ptr<Backend>> backends;
    for (int i = 0; i < 3; ++i) {
        auto b = std::make_shared<Backend>();
        b->host = "127.0.0.1";
        b->port = 9000 + i;
        backends.push_back(b);
    }
    LoadBalancer lb(backends, LbStrategy::RoundRobin);
    std::vector<int> seenPorts;
    for (int i = 0; i < 6; ++i) seenPorts.push_back(lb.pick()->port);
    // Every backend should appear exactly twice across 6 picks of 3 backends.
    for (int port : {9000, 9001, 9002}) {
        EXPECT_EQ(std::count(seenPorts.begin(), seenPorts.end(), port), 2);
    }
}

TEST(LoadBalancerTest, SkipsUnhealthyBackends) {
    std::vector<std::shared_ptr<Backend>> backends;
    auto healthy = std::make_shared<Backend>();
    healthy->host = "127.0.0.1"; healthy->port = 1;
    auto unhealthy = std::make_shared<Backend>();
    unhealthy->host = "127.0.0.1"; unhealthy->port = 2;
    unhealthy->healthy = false;
    backends = {healthy, unhealthy};

    LoadBalancer lb(backends, LbStrategy::RoundRobin);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(lb.pick()->port, 1);
    }
}

TEST(LoadBalancerTest, LeastConnectionsPicksLowestLoad) {
    std::vector<std::shared_ptr<Backend>> backends;
    auto a = std::make_shared<Backend>(); a->host = "h"; a->port = 1; a->activeConnections = 5;
    auto b = std::make_shared<Backend>(); b->host = "h"; b->port = 2; b->activeConnections = 1;
    auto c = std::make_shared<Backend>(); c->host = "h"; c->port = 3; c->activeConnections = 3;
    backends = {a, b, c};

    LoadBalancer lb(backends, LbStrategy::LeastConnections);
    EXPECT_EQ(lb.pick()->port, 2);
}

TEST(LoadBalancerTest, ReturnsNullWhenAllUnhealthy) {
    std::vector<std::shared_ptr<Backend>> backends;
    auto a = std::make_shared<Backend>(); a->host = "h"; a->port = 1; a->healthy = false;
    backends = {a};
    LoadBalancer lb(backends, LbStrategy::RoundRobin);
    EXPECT_EQ(lb.pick(), nullptr);
}
