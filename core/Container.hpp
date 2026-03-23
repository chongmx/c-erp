#pragma once
#include "factories/Factories.hpp"
#include "infrastructure/DbConnection.hpp"
#include "infrastructure/HttpServer.hpp"
#include "infrastructure/JsonRpcDispatcher.hpp"
#include "infrastructure/SessionManager.hpp"
#include "infrastructure/WebSocketServer.hpp"
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
     * @brief Build AppConfig from an INI-style config file (Odoo format).
     *
     * Recognised keys under [options]:
     *   db_host, db_port, db_name, db_user, db_password, db_maxconn
     *   http_interface, http_port, workers, http_doc_root, http_index
     *   log_level, logfile, smtp_server, smtp_port, smtp_ssl, smtp_user,
     *   smtp_password, email_from
     *
     * Throws std::runtime_error if the file cannot be opened.
     */
    static AppConfig fromFile(const std::string& path);

    /**
     * @brief Load config from file if it exists, otherwise fall back to
     *        environment variables.
     */
    static AppConfig fromFileOrEnv(const std::string& path = "config/system.cfg");

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
    std::shared_ptr<HttpServer>       http;
    std::shared_ptr<WebSocketServer>  ws;
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
        http     = std::make_shared<HttpServer>(cfg.http);
        ws       = std::make_shared<WebSocketServer>();
        sessions = std::make_shared<SessionManager>();

        // --- Factories ---
        // Models and services receive the DB connection; the others are stateless.
        models     = std::make_shared<core::ModelFactory>(db);
        services   = std::make_shared<core::ServiceFactory>(db);
        viewModels = std::make_shared<core::ViewModelFactory>();
        views      = std::make_shared<core::ViewFactory>();
        modules    = std::make_shared<core::ModuleFactory>();

        // --- RPC dispatcher ---
        // Wired to viewModelFactory + viewFactory + session store; HTTP routes added in boot().
        rpc = std::make_shared<JsonRpcDispatcher>(viewModels, sessions, views,
                                                   cfg.http.secureCookies);
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

        // Stage 2a — service post-init (cross-module wiring, warm-up)
        initializeServices_();

        // Stage 2b — module initialize() hooks (DDL, seeding)
        //            Runs after services so DB connections are ready,
        //            and in registration order so dependencies are satisfied.
        initializeModules_();

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
     * @brief Call IModule::initialize() on every registered module in order.
     *
     * Runs after initializeServices_() — DDL and seeding go here.
     * Modules execute in registration order (base before auth, etc.).
     */
    void initializeModules_() {
        for (const auto& name : modules->registeredNames()) {
            auto mod = modules->create(name, core::Lifetime::Singleton);
            mod->initialize();
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
// ── INI parser helpers ────────────────────────────────────────────────────────

static std::string cfgTrim_(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Parse [options]-style INI file into a flat key→value map.
static std::unordered_map<std::string,std::string> parseCfgFile_(const std::string& path) {
    std::unordered_map<std::string,std::string> kv;
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open config file: " + path);

    std::string line;
    bool inOptions = false;
    while (std::getline(f, line)) {
        // Strip inline comments (# or ;)
        for (std::size_t i = 0; i < line.size(); ++i) {
            if ((line[i] == '#' || line[i] == ';') &&
                (i == 0 || line[i-1] == ' ' || line[i-1] == '\t')) {
                line = line.substr(0, i);
                break;
            }
        }
        std::string t = cfgTrim_(line);
        if (t.empty()) continue;

        // Section header
        if (t.front() == '[') {
            inOptions = (t == "[options]");
            continue;
        }
        if (!inOptions) continue;

        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = cfgTrim_(t.substr(0, eq));
        std::string val = cfgTrim_(t.substr(eq + 1));
        kv[key] = val;
    }
    return kv;
}

// ── AppConfig::fromFile ───────────────────────────────────────────────────────

inline AppConfig AppConfig::fromFile(const std::string& path) {
    auto kv = parseCfgFile_(path);

    auto get = [&](const std::string& key, const std::string& def) -> std::string {
        auto it = kv.find(key);
        if (it == kv.end() || it->second.empty() ||
            it->second == "False" || it->second == "false") return def;
        return it->second;
    };
    auto getInt = [&](const std::string& key, int def) -> int {
        auto it = kv.find(key);
        if (it == kv.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    };

    AppConfig cfg;
    cfg.db.host     = get("db_host",     "localhost");
    cfg.db.port     = getInt("db_port",  5432);
    cfg.db.name     = get("db_name",     "odoo");
    cfg.db.user     = get("db_user",     "odoo");
    cfg.db.password = get("db_password", "");
    cfg.db.poolSize = getInt("db_maxconn", 10);

    cfg.http.host          = get("http_interface", "0.0.0.0");
    cfg.http.port          = getInt("http_port",   8069);
    cfg.http.threads       = getInt("workers",     4);
    cfg.http.docRoot       = get("http_doc_root",  "web/static");
    cfg.http.indexFile     = get("http_index",     "index.html");
    cfg.http.corsOrigin    = get("cors_origin",    "");
    cfg.http.devMode       = (get("dev_mode",       "false") == "true"
                           || get("dev_mode",       "false") == "True");
    cfg.http.secureCookies = (get("secure_cookies", "false") == "true"
                           || get("secure_cookies", "false") == "True");

    return cfg;
}

// ── AppConfig::fromFileOrEnv ──────────────────────────────────────────────────

inline AppConfig AppConfig::fromFileOrEnv(const std::string& path) {
    std::ifstream probe(path);
    if (probe.is_open()) {
        probe.close();
        std::cout << "[odoo-cpp] Loading config from " << path << "\n";
        return fromFile(path);
    }
    std::cout << "[odoo-cpp] Config file not found (" << path << "), using environment variables.\n";
    return fromEnv();
}

// ── AppConfig::fromEnv ────────────────────────────────────────────────────────

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
    cfg.db.password = env("DB_PASSWORD", "odoo");
    cfg.db.poolSize = envInt("DB_POOL_SIZE", 10);

    // HTTP
    cfg.http.host      = env("HTTP_HOST",    "0.0.0.0");
    cfg.http.port      = envInt("HTTP_PORT", 8069);
    cfg.http.threads   = envInt("HTTP_THREADS", 4);
    cfg.http.docRoot   = env("HTTP_DOC_ROOT",  "web/static");
    cfg.http.indexFile = env("HTTP_INDEX",     "index.html");

    return cfg;
}

} // namespace odoo::infrastructure