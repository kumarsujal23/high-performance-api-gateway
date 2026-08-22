#pragma once
#include "routing/LoadBalancer.hpp"
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

namespace gw::routing {

struct Route {
    std::string pathPrefix;               // e.g. "/api/users"
    std::shared_ptr<LoadBalancer> lb;
    bool cacheable = false;                // whether GET responses may be cached
};

// Longest-prefix-match router, same principle used by real reverse proxies
// (nginx location blocks, Envoy route tables). Prefixes are checked longest
// first so a specific route like "/api/users/admin" wins over the more
// general "/api/users".
class Router {
public:
    void addRoute(Route route) {
        routes_.push_back(std::move(route));
        std::sort(routes_.begin(), routes_.end(), [](const Route& a, const Route& b) {
            return a.pathPrefix.size() > b.pathPrefix.size();
        });
    }

    const Route* match(const std::string& path) const {
        for (auto& r : routes_) {
            bool exact = path == r.pathPrefix;
            bool nested = r.pathPrefix == "/" ||
                          (path.size() > r.pathPrefix.size() &&
                           path.compare(0, r.pathPrefix.size(), r.pathPrefix) == 0 &&
                           path[r.pathPrefix.size()] == '/');
            if (exact || nested) return &r;
        }
        return nullptr;
    }

    const std::vector<Route>& routes() const { return routes_; }

private:
    std::vector<Route> routes_;
};

} // namespace gw::routing
