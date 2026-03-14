#pragma once
#include "interfaces/IFactory.hpp"
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace odoo::core {

// Lifetime is defined in IFactory.hpp — no redeclaration needed here.

// ============================================================
// BaseFactory<TBase>
// ============================================================
/**
 * @brief Generic keyed factory supporting Transient and Singleton lifetimes.
 *
 * Implements IFactory<TBase> and adds registration helpers used by all
 * domain factories (Model, Service, ViewModel, View, Module).
 *
 * Thread-safety: NOT thread-safe by design — construction happens during
 * single-threaded boot. Runtime create() for Singletons is read-only after
 * boot. Transient create() should be guarded externally if called from
 * multiple threads.
 */
template<typename TBase>
class BaseFactory : public IFactory<TBase> {
public:
    using Creator  = std::function<std::shared_ptr<TBase>()>;
    using Registry = std::unordered_map<std::string, Creator>;
    using Cache    = std::unordered_map<std::string, std::shared_ptr<TBase>>;

    virtual ~BaseFactory() = default;

    // ----------------------------------------------------------
    // Registration
    // ----------------------------------------------------------

    /**
     * @brief Register a creator lambda for a given key.
     * @throws std::invalid_argument if key is already registered (use overrideCreator to replace).
     */
    void registerCreator(const std::string& key, Creator creator) {
        if (registry_.count(key))
            throw std::invalid_argument("BaseFactory: key already registered: " + key);
        registry_[key] = std::move(creator);
    }

    /**
     * @brief Register a pre-built singleton instance directly.
     * Useful when the caller owns construction (e.g. injecting a mock in tests).
     */
    void registerSingleton(const std::string& key, std::shared_ptr<TBase> instance) {
        // Store a trivial creator (never called) and prime the cache.
        registerCreator(key, [instance] { return instance; });
        singletonCache_[key] = std::move(instance);
    }

    /**
     * @brief Replace an existing creator (e.g. plugin override, test mock).
     * Clears any cached Singleton so the new creator is used next time.
     */
    void overrideCreator(const std::string& key, Creator creator) {
        registry_[key] = std::move(creator);
        singletonCache_.erase(key);
    }

    // ----------------------------------------------------------
    // Instantiation
    // ----------------------------------------------------------

    /**
     * @brief Create (or retrieve) an instance for key.
     * @param lifetime  Transient = new instance; Singleton = cached instance.
     * @throws std::runtime_error if key not registered.
     */
    std::shared_ptr<TBase> create(const std::string& key,
                                  Lifetime lifetime = Lifetime::Transient) {
        auto it = registry_.find(key);
        if (it == registry_.end())
            throw std::runtime_error("BaseFactory: no creator registered for key: " + key);

        if (lifetime == Lifetime::Singleton) {
            auto& cached = singletonCache_[key];
            if (!cached) cached = it->second();
            return cached;
        }

        return it->second();
    }

    // ----------------------------------------------------------
    // Introspection
    // ----------------------------------------------------------

    bool has(const std::string& key) const {
        return registry_.count(key) > 0;
    }

    std::vector<std::string> registeredNames() const {
        std::vector<std::string> names;
        names.reserve(registry_.size());
        for (const auto& [k, _] : registry_) names.push_back(k);
        return names;
    }

    /**
     * @brief Evict a cached Singleton (useful in tests or hot-reload scenarios).
     */
    void evictSingleton(const std::string& key) {
        singletonCache_.erase(key);
    }

    /**
     * @brief Evict all Singleton caches.
     */
    void evictAllSingletons() {
        singletonCache_.clear();
    }

private:
    Registry registry_;
    Cache    singletonCache_;
};

} // namespace odoo::core