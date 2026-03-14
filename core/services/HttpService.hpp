#pragma once
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace odoo::infrastructure {

// ============================================================
// HttpConfig
// ============================================================
struct HttpConfig {
    std::string host        = "0.0.0.0";
    int         port        = 8069;
    int         threads     = 4;
    bool        logRequests = true;

    /**
     * @brief Permissive CORS origin sent in every response.
     * Set to the OWL dev server in development, restrict in production.
     */
    std::string corsOrigin  = "*";
};


// ============================================================
// Request / Response type aliases
// ============================================================
/**
 * @brief Public aliases for Drogon request/response types.
 *
 * All other infrastructure files (JsonRpcDispatcher, WebSocketService)
 * include only HttpService.hpp — they never import drogon/drogon.h
 * directly.  These aliases are the stable API boundary.
 */
using HttpRequestPtr  = drogon::HttpRequestPtr;
using HttpResponsePtr = drogon::HttpResponsePtr;

/**
 * @brief Async callback type Drogon expects from every handler.
 * Handlers must call this exactly once with the response.
 */
using HttpCallback = std::function<void(const HttpResponsePtr&)>;


// ============================================================
// HttpService
// ============================================================
/**
 * @brief Thin wrapper around the Drogon HTTP application.
 *
 * Provides a stable API surface so the rest of the codebase never
 * imports drogon/drogon.h directly — only HttpService.hpp and
 * WebSocketService.hpp carry that dependency.
 *
 * Route registration happens before start() is called:
 * @code
 *   dispatcher->registerRoutes(httpService);
 *   wsService->registerRoutes(httpService);
 *   httpService.start();   // blocks
 * @endcode
 *
 * Handler contract:
 *   addJsonPost / addJsonGet wrap handlers in try/catch and convert
 *   exceptions to JSON error responses automatically, so ViewModel code
 *   can throw std::runtime_error freely without touching Drogon types.
 *
 * CORS:
 *   Every response includes Access-Control-Allow-Origin from
 *   HttpConfig::corsOrigin. addCorsOptions() mounts the OPTIONS
 *   preflight handler for paths that need it.
 */
class HttpService {
public:
    explicit HttpService(const HttpConfig& cfg = {}) : cfg_(cfg) {
        auto& app = drogon::app();

        app.setLogLevel(cfg_.logRequests
            ? trantor::Logger::kInfo
            : trantor::Logger::kWarn);

        app.addListener(cfg_.host, cfg_.port)
           .setThreadNum(cfg_.threads);

        // Built-in health endpoint — no auth required
        app.registerHandler("/healthz",
            [](const HttpRequestPtr&, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::k200OK);
                res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                res->setBody(R"({"status":"ok"})");
                cb(res);
            },
            {drogon::Get});
    }

    // Non-copyable, non-movable — shared via shared_ptr
    HttpService(const HttpService&)            = delete;
    HttpService& operator=(const HttpService&) = delete;

    // ----------------------------------------------------------
    // Route registration helpers
    // ----------------------------------------------------------

    /**
     * @brief Register a POST route that receives and returns JSON.
     *
     * Handler signature:
     * @code
     *   nlohmann::json handler(const HttpRequestPtr& req,
     *                          const nlohmann::json& body);
     * @endcode
     *
     * Exceptions are caught and converted to structured JSON error responses:
     *   json::exception  → HTTP 400
     *   std::exception   → HTTP 500
     *
     * @param path     URL path, e.g. "/web/dataset/call_kw"
     * @param handler  Synchronous callable returning nlohmann::json.
     */
    template<typename Handler>
    void addJsonPost(const std::string& path, Handler&& handler) {
        const std::string origin = cfg_.corsOrigin;

        drogon::app().registerHandler(path,
            [h = std::forward<Handler>(handler), origin]
            (const HttpRequestPtr& req, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type",                "application/json");
                res->addHeader("Access-Control-Allow-Origin", origin);

                try {
                    const auto body   = nlohmann::json::parse(req->body());
                    const auto result = h(req, body);
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                } catch (const nlohmann::json::exception& e) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error",  "Invalid JSON"},
                        {"detail", e.what()}
                    }.dump());
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{
                        {"error",  "Internal server error"},
                        {"detail", e.what()}
                    }.dump());
                }
                cb(res);
            },
            {drogon::Post});
    }

    /**
     * @brief Register a GET route that returns JSON.
     *
     * Handler signature:
     * @code
     *   nlohmann::json handler(const HttpRequestPtr& req);
     * @endcode
     *
     * @param path     URL path, e.g. "/web/session/get_session_info"
     * @param handler  Synchronous callable returning nlohmann::json.
     */
    template<typename Handler>
    void addJsonGet(const std::string& path, Handler&& handler) {
        const std::string origin = cfg_.corsOrigin;

        drogon::app().registerHandler(path,
            [h = std::forward<Handler>(handler), origin]
            (const HttpRequestPtr& req, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type",                "application/json");
                res->addHeader("Access-Control-Allow-Origin", origin);

                try {
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(h(req).dump());
                } catch (const std::exception& e) {
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{
                        {"error", e.what()}
                    }.dump());
                }
                cb(res);
            },
            {drogon::Get});
    }

    /**
     * @brief Register an OPTIONS preflight handler for CORS.
     *
     * Call this for every path registered with addJsonPost() so browsers
     * can complete their preflight check before the actual POST.
     *
     * @param path  Must match the path passed to addJsonPost().
     */
    void addCorsOptions(const std::string& path) {
        const std::string origin = cfg_.corsOrigin;

        drogon::app().registerHandler(path,
            [origin](const HttpRequestPtr&, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::k204NoContent);
                res->addHeader("Access-Control-Allow-Origin",  origin);
                res->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                res->addHeader("Access-Control-Allow-Headers",
                               "Content-Type, Authorization, X-Requested-With");
                cb(res);
            },
            {drogon::Options});
    }

    // ----------------------------------------------------------
    // Lifecycle
    // ----------------------------------------------------------

    /**
     * @brief Start the HTTP server and block until stop() is called.
     *
     * Must be called from the main thread after all routes are registered.
     * Drogon runs its own internal event loop; this call does not return
     * until drogon::app().quit() is invoked (via stop()).
     */
    void start() {
        drogon::app().run();
    }

    /**
     * @brief Signal the server to stop and unblock start().
     * Thread-safe — safe to call from a signal handler or another thread.
     */
    void stop() {
        drogon::app().quit();
    }

    // ----------------------------------------------------------
    // Accessors
    // ----------------------------------------------------------

    /**
     * @brief Direct access to the Drogon application singleton.
     *
     * Used by WebSocketService to register WebSocket upgrade handlers
     * via drogon::app().registerHandler() with the WebSocket controller.
     * Avoid using this in business logic — prefer addJsonPost/addJsonGet.
     */
    drogon::HttpAppFramework& app() { return drogon::app(); }

    const HttpConfig& config() const { return cfg_; }

private:
    HttpConfig cfg_;
};

} // namespace odoo::infrastructure