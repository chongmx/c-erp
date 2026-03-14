#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace odoo::core {

// ============================================================
// IService
// ============================================================
/**
 * @brief Interface every business-logic service must satisfy.
 *
 * Services sit between ViewModels and Models:
 *
 *   JsonRpcDispatcher → ViewModel → IService → IModel → DbConnection
 *
 * They own:
 *   - Transaction boundaries (begin / commit / rollback)
 *   - Cross-model orchestration (e.g. create partner + user atomically)
 *   - Business rule enforcement that spans multiple records
 *
 * They do NOT own:
 *   - HTTP / WebSocket concerns (that's HttpService / WebSocketService)
 *   - View rendering (that's IView)
 *   - Request routing (that's IViewModel)
 *
 * Lifetime: Singleton — services are stateless and shared across requests.
 * Per-request state lives in the ViewModel or in local variables.
 *
 * Naming convention: serviceName() returns a short snake_case identifier
 * matching the ServiceFactory key, e.g. "auth", "partner", "accounting".
 */
class IService {
public:
    virtual ~IService() = default;

    // ----------------------------------------------------------
    // Identity
    // ----------------------------------------------------------

    /**
     * @brief Short service identifier, e.g. "auth", "partner".
     * Must match the key used at ServiceFactory registration.
     */
    virtual std::string serviceName() const = 0;

    // ----------------------------------------------------------
    // Lifecycle
    // ----------------------------------------------------------

    /**
     * @brief Called once after all modules have registered their components.
     *
     * Use for deferred initialization that requires other services or models
     * to already be registered (e.g. cross-service wiring, cache warm-up).
     * Default implementation is a no-op.
     */
    virtual void initialize() {}

    /**
     * @brief Called on clean shutdown.
     *
     * Flush caches, close external connections, persist in-memory state.
     * Default implementation is a no-op.
     */
    virtual void shutdown() {}

    // ----------------------------------------------------------
    // Health / diagnostics
    // ----------------------------------------------------------

    /**
     * @brief Return a JSON object describing the service's health.
     *
     * Minimum expected keys:
     *   { "service": "<name>", "status": "ok" | "degraded" | "down" }
     *
     * Services may add extra diagnostic fields (queue depth, cache hit-rate, …).
     * Called by a /healthz HTTP endpoint; must be cheap and non-blocking.
     */
    virtual nlohmann::json healthCheck() const {
        return { {"service", serviceName()}, {"status", "ok"} };
    }
};

} // namespace odoo::core