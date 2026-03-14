#pragma once
#include <memory>
#include <string>
#include <vector>

namespace odoo::core {

// ============================================================
// Lifetime
// ============================================================
/**
 * @brief Controls instance reuse within a BaseFactory.
 *
 *  Transient  — new instance on every create() call.
 *  Singleton  — one shared instance per key, created lazily and cached.
 *
 * The enum lives here so every factory and every caller shares one definition
 * without pulling in BaseFactory.hpp (which is heavier).
 */
enum class Lifetime {
    Transient,
    Singleton,
};


// ============================================================
// IFactory<TBase>
// ============================================================
/**
 * @brief Minimal interface that every factory must satisfy.
 *
 * Concrete factories inherit from BaseFactory<TBase>, which implements
 * this interface and adds registration helpers. The interface exists so
 * Container and tests can hold a type-erased factory reference when needed.
 *
 * @tparam TBase  Abstract base type produced by this factory.
 */
template<typename TBase>
class IFactory {
public:
    virtual ~IFactory() = default;

    /**
     * @brief Create (or retrieve) an instance for the given key.
     * @throws std::runtime_error if key is not registered.
     */
    virtual std::shared_ptr<TBase> create(const std::string& key,
                                          Lifetime lifetime = Lifetime::Transient) = 0;

    /** @brief True if key has a registered creator. */
    virtual bool has(const std::string& key) const = 0;

    /** @brief All registered keys in unspecified order. */
    virtual std::vector<std::string> registeredNames() const = 0;
};

} // namespace odoo::core