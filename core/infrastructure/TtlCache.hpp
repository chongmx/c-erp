#pragma once
// =============================================================
// core/infrastructure/TtlCache.hpp  —  PERF-D
//
// Simple thread-safe TTL cache for quasi-static data (menus,
// actions, currencies, field metadata).  Entries are valid for
// ttlSeconds after insertion; stale entries are evicted on the
// next read, not on a background thread.
//
// Usage:
//   TtlCache<std::string, nlohmann::json> cache;
//
//   // Write
//   cache.set("currencies", value, 60);   // expires in 60 s
//
//   // Read
//   if (auto v = cache.get("currencies")) return *v;
//   // ... else rebuild and cache ...
//
//   // Invalidate (e.g. after an admin write)
//   cache.invalidate("currencies");
//   cache.invalidateAll();
// =============================================================
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace odoo::infrastructure {

template<typename K, typename V>
class TtlCache {
public:
    /**
     * @brief Look up a key.
     * @returns The cached value if present and not expired, or std::nullopt.
     */
    std::optional<V> get(const K& key) const {
        std::lock_guard lk{mutex_};
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        if (Clock::now() > it->second.expiry) {
            map_.erase(it);            // lazy eviction
            return std::nullopt;
        }
        return it->second.value;
    }

    /**
     * @brief Insert or replace a key with a TTL.
     * @param ttlSeconds  Time-to-live in seconds (0 = never expires).
     */
    void set(const K& key, V value, int ttlSeconds = 60) {
        const auto expiry = ttlSeconds > 0
            ? Clock::now() + std::chrono::seconds(ttlSeconds)
            : Clock::time_point::max();
        std::lock_guard lk{mutex_};
        map_.insert_or_assign(key, Entry{std::move(value), expiry});
    }

    /** @brief Remove a single key (e.g. after a write to the underlying table). */
    void invalidate(const K& key) {
        std::lock_guard lk{mutex_};
        map_.erase(key);
    }

    /** @brief Flush all entries (e.g. after any admin configuration change). */
    void invalidateAll() {
        std::lock_guard lk{mutex_};
        map_.clear();
    }

    /** @brief Number of currently-cached (possibly expired) entries. */
    std::size_t size() const {
        std::lock_guard lk{mutex_};
        return map_.size();
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        V                    value;
        Clock::time_point    expiry;
    };

    mutable std::mutex                  mutex_;
    mutable std::unordered_map<K, Entry> map_;
};

} // namespace odoo::infrastructure
