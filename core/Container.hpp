#pragma once
#include "factories/Factories.hpp"
#include "services/DbConnection.hpp"
#include "services/HttpService.hpp"
#include "services/JsonRpcDispatcher.hpp"
#include "services/SessionManager.hpp"
#include "services/WebSocketService.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace odoo::infrastructure {

// ============================================================
// AppConfig
// ============================================================
/**
 * @brief Top-level application configuration.
 *
 * Aggregates every subsystem's config in one place so main() only
 * touches a single object.  All fields have sane defaults so the
 * minimum viable launch only needs a DbConfig.
 *
 * Environment-variable loading (12-factor style):
 * @code
 *   auto cfg = AppConfig::fromEnv();
 *   auto container = std::make_shared<Container>(cfg);
 * @endcode
 */
struct AppConfig {
    DbConfig   db;
    HttpConfig http;

    /**
     * @brief Build AppConfig from environment variables.
     *
     * Recognised variables:
     *   DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASSWORD, DB_POOL_SIZE
     *   HTTP_HOST, HTTP_PORT, HTTP_THREADS
     */
    static AppConfig fromEnv();
};


// ============================================================
// Container
// ============================================================
/**
 * @brief Application-level DI container.
 *
 * Single source of truth for infrastructure construction and factory wiring.
 * Owns every long-lived singleton. Constructed once in main(), then modules
 * are registered via addModule<TModule>() before boot() is called.
 *
 * Typical main():
 * @code
 *   auto cfg = AppConfig::fromEnv();
 *   auto c   = std::make_shared<Container>(cfg);
 *
 *   c->addModule<BaseModule>();
 *   c->addModule<AccountModule>();
 *   c->boot();   // topological module boot + service init + route wiring
 *   c->run();    // blocks until SIGINT / SIGTERM
 * @endcode
 *
 * Boot stages (in order):
 *   1. Modules register components into factories (registerModels, etc.)
 *   2. Every IService::initialize() is called (cross-service wiring)
 *   3. JSON-RPC dispatcher mounts its routes on the HTTP server
 *   4. WebSocket service mounts its routes on the HTTP server
 *
 * Shutdown (RAII + explicit):
 *   Container::shutdown() is called by run() on exit.  It calls
 *   IService::shutdown() on every registered service before stopping HTTP.
 */
class Container {
public:
    // ----------------------------------------------------------
    // Infrastructure — long-lived singletons, publicly readable
    // ----------------------------------------------------------
    std::shared_ptr<DbConnection>      db;
    std::shared_ptr<HttpService>       http;
    std::shared_ptr<WebSocketService>  ws;
    std::shared_ptr<SessionManager>    sessions;
    std::shared_ptr<JsonRpcDispatcher> rpc;

    // ----------------------------------------------------------
    // Factories — publicly readable so modules can capture refs
    // ----------------------------------------------------------
    std::shared_ptr<core::ModelFactory>     models;
    std::shared_ptr<core::ServiceFactory>   services;
    std::shared_ptr<core::ViewModelFactory> viewModels;
    std::shared_ptr<core::ViewFactory>      views;
    std::shared_ptr<core::ModuleFactory>    modules;

    // ----------------------------------------------------------
    // Construction
    // ----------------------------------------------------------

    /**
     * @brief Construct all infrastructure and empty factories.
     *
     * Does NOT boot modules or start the HTTP server.
     * Call addModule<T>() then boot(), then run().
     *
     * @param cfg  Full application configuration.
     */
    explicit Container(const AppConfig& cfg) {
        // --- Infrastructure ---
        db       = std::make_shared<DbConnection>(cfg.db);
        http     = std::make_shared<HttpService>(cfg.http);
        ws       = std::make_shared<WebSocketService>();
        sessions = std::make_shared<SessionManager>();

        // --- Factories ---
        // Models and services receive the DB connection; the others are stateless.
        models     = std::make_shared<core::ModelFactory>(db);
        services   = std::make_shared<core::ServiceFactory>(db);
        viewModels = std::make_shared<core::ViewModelFactory>();
        views      = std::make_shared<core::ViewFactory>();
        modules    = std::make_shared<core::ModuleFactory>();

        // --- RPC dispatcher ---
        // Wired to viewModelFactory + session store; HTTP routes added in boot().
        rpc = std::make_shared<JsonRpcDispatcher>(viewModels, sessions);
    }

    // ----------------------------------------------------------
    // Module registration
    // ----------------------------------------------------------

    /**
     * @brief Register a module type to be booted.
     *
     * TModule is constructed with references to all five factories so it can
     * wire its models/services/viewmodels/views/routes during bootAll().
     *
     * @tparam TModule  Concrete module implementing IModule.
     *                  Must be constructible as:
     *                    TModule(ModelFactory&, ServiceFactory&,
     *                            ViewModelFactory&, ViewFactory&)
     *
     * Example:
     * @code
     *   container->addModule<BaseModule>();
     * @endcode
     */
    template<typename TModule>
    void addModule() {
        // Capture factory shared_ptrs by value so the lambda is self-contained.
        auto mf  = models;
        auto sf  = services;
        auto vmf = viewModels;
        auto vf  = views;

        const std::string key = TModule::staticModuleName(); // constexpr name tag

        modules->registerCreator(key, [mf, sf, vmf, vf] {
            return std::make_shared<TModule>(*mf, *sf, *vmf, *vf);
        });
    }

    /**
     * @brief Register a pre-built module instance (useful in tests).
     *
     * @param mod  Fully constructed module; must already be wired.
     */
    void addModuleInstance(std::shared_ptr<core::IModule> mod) {
        modules->registerSingleton(mod->moduleName(), std::move(mod));
    }

    // ----------------------------------------------------------
    // Boot
    // ----------------------------------------------------------

    /**
     * @brief Boot all registered modules and wire infrastructure routes.
     *
     * Boot sequence:
     *   1. modules->bootAll()
     *        For each module in dependency order:
     *          a. registerModels()
     *          b. registerServices()
     *          c. registerViewModels()
     *          d. registerViews()
     *          e. registerRoutes()
     *   2. IService::initialize() on every registered service
     *        (cross-service wiring, cache warm-up, etc.)
     *   3. rpc->registerRoutes(*http)
     *        Mounts /web/dataset/call_kw  and /web/dataset/call on Crow.
     *   4. ws->registerRoutes(*http)
     *        Mounts /websocket (bus.Bus pub/sub channel).
     *
     * @throws std::runtime_error from any module or service that fails init.
     */
    void boot() {
        // Stage 1 — module boot (register* calls)
        modules->bootAll();

        // Stage 2 — service post-init (cross-module wiring, warm-up)
        initializeServices_();

        // Stage 3 — HTTP: JSON-RPC dispatcher routes
        rpc->registerRoutes(*http);

        // Stage 4 — HTTP: WebSocket upgrade routes
        ws->registerRoutes(*http);
    }

    // ----------------------------------------------------------
    // Run / shutdown
    // ----------------------------------------------------------

    /**
     * @brief Start the HTTP server and block until stopped.
     *
     * Performs a DB health check before accepting connections.
     * On return (SIGINT / SIGTERM / http->stop()), calls shutdown().
     *
     * @throws std::runtime_error if the DB connection is unhealthy.
     */
    void run() {
        if (!db->isHealthy()) {
            throw std::runtime_error(
                "Container::run() — DB connection unhealthy, refusing to start");
        }
        http->start(); // blocks
        shutdown();    // tidy up after server exits
    }

    /**
     * @brief Graceful shutdown.
     *
     * Called automatically by run() on exit, but can be called directly
     * (e.g. from a signal handler or test fixture teardown).
     *
     * Shutdown order (reverse of boot):
     *   1. Stop accepting new HTTP connections
     *   2. IService::shutdown() on every registered service
     *      (flush queues, close external connections, persist state)
     */
    void shutdown() {
        http->stop();
        shutdownServices_();
    }

    // ----------------------------------------------------------
    // Diagnostics
    // ----------------------------------------------------------

    /**
     * @brief Aggregate health status from all registered services + DB.
     *
     * Returns a JSON object compatible with a /healthz endpoint:
     * @code
     * {
     *   "status":   "ok" | "degraded" | "down",
     *   "db":       { "status": "ok" },
     *   "services": {
     *     "auth":    { "service": "auth",    "status": "ok" },
     *     "partner": { "service": "partner", "status": "ok" }
     *   }
     * }
     * @endcode
     */
    nlohmann::json healthCheck() const {
        nlohmann::json out;
        bool allOk = true;

        // DB health
        const bool dbOk = db->isHealthy();
        out["db"] = { {"status", dbOk ? "ok" : "down"} };
        if (!dbOk) allOk = false;

        // Service health
        nlohmann::json svcHealth = nlohmann::json::object();
        for (const auto& key : services->registeredNames()) {
            auto svc = services->create(key, core::Lifetime::Singleton);
            auto h   = svc->healthCheck();
            if (h.value("status", "ok") != "ok") allOk = false;
            svcHealth[key] = std::move(h);
        }
        out["services"] = std::move(svcHealth);
        out["status"]   = allOk ? "ok" : "degraded";
        return out;
    }

private:
    // ----------------------------------------------------------
    // Internal helpers
    // ----------------------------------------------------------

    /**
     * @brief Call IService::initialize() on every registered service.
     *
     * Run after all modules have registered their components so services
     * can safely resolve cross-module dependencies (e.g. AuthService
     * looking up PartnerService from ServiceFactory).
     */
    void initializeServices_() {
        for (const auto& key : services->registeredNames()) {
            auto svc = services->create(key, core::Lifetime::Singleton);
            svc->initialize();
        }
    }

    /**
     * @brief Call IService::shutdown() on every registered service.
     *
     * Order is unspecified; services must not depend on other services
     * still being alive during their own shutdown().
     */
    void shutdownServices_() {
        for (const auto& key : services->registeredNames()) {
            auto svc = services->create(key, core::Lifetime::Singleton);
            svc->shutdown();
        }
    }
};


// ============================================================
// AppConfig::fromEnv() — inline implementation
// ============================================================
inline AppConfig AppConfig::fromEnv() {
    AppConfig cfg;

    // DB
    auto env = [](const char* var, const std::string& def) -> std::string {
        const char* v = std::getenv(var);
        return v ? std::string(v) : def;
    };
    auto envInt = [&](const char* var, int def) -> int {
        const auto s = env(var, "");
        return s.empty() ? def : std::stoi(s);
    };

    cfg.db.host     = env("DB_HOST",     "localhost");
    cfg.db.port     = envInt("DB_PORT",  5432);
    cfg.db.name     = env("DB_NAME",     "odoo");
    cfg.db.user     = env("DB_USER",     "odoo");
    cfg.db.password = env("DB_PASSWORD", "");
    cfg.db.poolSize = envInt("DB_POOL_SIZE", 10);

    // HTTP
    cfg.http.host    = env("HTTP_HOST",    "0.0.0.0");
    cfg.http.port    = envInt("HTTP_PORT", 8069);
    cfg.http.threads = envInt("HTTP_THREADS", 4);

    return cfg;
}

} // namespace odoo::infrastructure