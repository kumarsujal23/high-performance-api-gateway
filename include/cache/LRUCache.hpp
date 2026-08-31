#pragma once
#include <unordered_map>
#include <list>
#include <mutex>
#include <chrono>
#include <string>
#include <optional>

namespace gw::cache {

// Thread-safe LRU cache with per-entry TTL, used to cache idempotent (GET)
// backend responses. Classic implementation: a doubly linked list holds
// recency order (front = most recently used), and a hash map gives O(1)
// lookup of list iterators — the standard O(1) get/put LRU used in interviews.
template <typename Value>
class LRUCache {
public:
    explicit LRUCache(size_t capacity, std::chrono::milliseconds ttl = std::chrono::milliseconds(0))
        : capacity_(capacity), ttl_(ttl) {}

    std::optional<Value> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;

        if (ttl_.count() > 0) {
            auto age = std::chrono::steady_clock::now() - it->second->insertedAt;
            if (age > ttl_) {
                list_.erase(it->second);
                index_.erase(it);
                return std::nullopt;
            }
        }
        // Move to front (most recently used).
        list_.splice(list_.begin(), list_, it->second);
        return it->second->value;
    }

    void put(const std::string& key, Value value) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = std::move(value);
            it->second->insertedAt = std::chrono::steady_clock::now();
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        list_.push_front(Entry{key, std::move(value), std::chrono::steady_clock::now()});
        index_[key] = list_.begin();

        if (index_.size() > capacity_) {
            auto& lru = list_.back();
            index_.erase(lru.key);
            list_.pop_back();
        }
    }

    void invalidate(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            list_.erase(it->second);
            index_.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        list_.clear();
        index_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return index_.size();
    }

private:
    struct Entry {
        std::string key;
        Value value;
        std::chrono::steady_clock::time_point insertedAt;
    };

    size_t capacity_;
    std::chrono::milliseconds ttl_;
    mutable std::mutex mu_;
    std::list<Entry> list_;
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
};

} // namespace gw::cache
