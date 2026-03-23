#pragma once
#include <drogon/drogon.h>
#include <trantor/utils/AsyncFileLogger.h>
#include <nlohmann/json.hpp>
#include <filesystem>
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
     * @brief Allowed CORS origin, e.g. "http://localhost:3000".
     *
     * Empty string (default): no CORS headers are added — correct for
     * same-origin deployments where the frontend is served by this server.
     *
     * Set explicitly only when the JS frontend is on a different origin
     * (e.g. a separate dev server). Never use "*" in production because
     * it exposes all API responses to arbitrary third-party sites.
     */
    std::string corsOrigin  = "";

    /**
     * @brief Development mode — includes e.what() in error responses.
     *
     * false (default): generic "Internal server error" is returned to the
     * client; detail is logged server-side only.
     * true: full exception message is included in {"detail": ...}.
     * Set to true only on local developer machines. Never in production.
     */
    bool devMode = false;

    /**
     * @brief Set the Secure flag on the session cookie.
     *
     * Must be true whenever the server is behind HTTPS (production).
     * Leave false only for plain-HTTP localhost development.
     */
    bool secureCookies = false;

    /**
     * @brief Root directory for static file serving.
     *
     * When non-empty, Drogon serves files from this directory for any
     * request that doesn't match a registered API route.
     *
     * For the Odoo OWL frontend, point this at the Odoo web addon's
     * static directory, e.g.:
     *   /usr/lib/python3/dist-packages/odoo/addons/web/static
     *
     * For a simple test page during development, create a local folder:
     *   mkdir -p web/static && cp index.html web/static/
     *   cfg.http.docRoot = "web/static";
     *
     * Leave empty to disable static file serving (API-only mode).
     */
    std::string docRoot     = "";

    /**
     * @brief File served when the root path "/" is requested.
     * Only used when docRoot is non-empty.
     */
    std::string indexFile   = "index.html";

    /**
     * @brief Path to the log file, e.g. "log/system.log".
     *
     * Empty (default): log to stdout only.
     * Non-empty: log to both stdout and the specified file.
     * The directory is created automatically if it does not exist.
     *
     * config/system.cfg:  logfile = log/system.log
     */
    std::string logFile = "";

    /**
     * @brief Minimum log level written to the log.
     *
     * Accepted values (case-insensitive): trace, debug, info, warn, error, fatal
     * Default: warn
     *
     * config/system.cfg:  log_level = info
     */
    std::string logLevel = "warn";
};


// ============================================================
// Request / Response type aliases
// ============================================================
using HttpRequestPtr  = drogon::HttpRequestPtr;
using HttpResponsePtr = drogon::HttpResponsePtr;
using HttpCallback    = std::function<void(const HttpResponsePtr&)>;


// ============================================================
// HttpServer
// ============================================================
/**
 * @brief Thin wrapper around the Drogon HTTP application.
 *
 * Static file serving:
 *   Set HttpConfig::docRoot to serve a frontend from disk.
 *   API routes (/web/dataset/*, /healthz, /websocket) always take
 *   priority over static files regardless of registration order.
 *
 *   // Serve the Odoo OWL frontend:
 *   cfg.http.docRoot = "/usr/lib/python3/dist-packages/odoo/addons/web/static";
 *
 *   // Serve a local test page:
 *   cfg.http.docRoot = "web/static";   // relative to CWD at launch
 */
class HttpServer {
public:
    explicit HttpServer(const HttpConfig& cfg = {}) : cfg_(cfg) {
        auto& app = drogon::app();

        // ── Log level ──────────────────────────────────────────────
        {
            const std::string& lvl = cfg_.logLevel;
            trantor::Logger::LogLevel level = trantor::Logger::kWarn;
            if      (lvl == "trace") level = trantor::Logger::kTrace;
            else if (lvl == "debug") level = trantor::Logger::kDebug;
            else if (lvl == "info")  level = trantor::Logger::kInfo;
            else if (lvl == "warn")  level = trantor::Logger::kWarn;
            else if (lvl == "error") level = trantor::Logger::kError;
            else if (lvl == "fatal") level = trantor::Logger::kFatal;
            app.setLogLevel(level);
        }

        // ── File logger ────────────────────────────────────────────
        if (!cfg_.logFile.empty()) {
            // Ensure the log directory exists
            std::filesystem::path logPath(cfg_.logFile);
            if (logPath.has_parent_path())
                std::filesystem::create_directories(logPath.parent_path());

            // trantor uses the path as a base name and appends ".log" if
            // no extension is present; strip ".log" to avoid "system.log.log"
            std::string baseName = cfg_.logFile;
            if (baseName.size() > 4 &&
                baseName.substr(baseName.size() - 4) == ".log")
                baseName = baseName.substr(0, baseName.size() - 4);

            asyncLogger_.setFileName(baseName);
            asyncLogger_.startLogging();
            trantor::Logger::setOutputFunction(
                [this](const char* msg, const uint64_t len) {
                    asyncLogger_.output(msg, len);
                },
                [this]() { asyncLogger_.flush(); }
            );
            std::cout << "[odoo-cpp] Logging to file: " << cfg_.logFile << "\n";
        }

        app.addListener(cfg_.host, cfg_.port)
           .setThreadNum(cfg_.threads);

        // Static file serving — must be configured before run()
        if (!cfg_.docRoot.empty()) {
            app.setDocumentRoot(cfg_.docRoot);
            app.setFileTypes({"html","js","css","png","jpg","jpeg",
                              "gif","svg","ico","woff","woff2","ttf",
                              "eot","map","json","xml","txt"});
            // "/" → index.html
            app.registerHandler("/",
                [this](const HttpRequestPtr&, HttpCallback&& cb) {
                    auto res = drogon::HttpResponse::newFileResponse(
                        cfg_.docRoot + "/" + cfg_.indexFile);
                    cb(res);
                },
                {drogon::Get});
        }

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

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // ----------------------------------------------------------
    // Route registration helpers
    // ----------------------------------------------------------

    template<typename Handler>
    void addJsonPost(const std::string& path, Handler&& handler) {
        const std::string origin  = cfg_.corsOrigin;
        const bool        devMode = cfg_.devMode;

        drogon::app().registerHandler(path,
            [this, h = std::forward<Handler>(handler), origin, devMode]
            (const HttpRequestPtr& req, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                applySecurityHeaders_(res, origin);

                try {
                    const auto body   = nlohmann::json::parse(req->body());
                    const auto result = h(req, body);
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                } catch (const nlohmann::json::exception& e) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error",  "Invalid JSON"},
                        {"detail", devMode ? e.what() : "Bad request"}
                    }.dump());
                } catch (const std::exception& e) {
                    LOG_ERROR << "[http] POST " << req->getPath()
                              << " exception: " << e.what();
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{
                        {"error",  "Internal server error"},
                        {"detail", devMode ? e.what() : "An internal error occurred"}
                    }.dump());
                }
                cb(res);
            },
            {drogon::Post});
    }

    /**
     * @brief Like addJsonPost but passes the response object to the handler
     * so it can set extra headers (e.g. Set-Cookie after authenticate).
     *
     * Handler signature:
     *   nlohmann::json handler(const HttpRequestPtr&, const nlohmann::json& body,
     *                          HttpResponsePtr& res);
     */
    template<typename Handler>
    void addJsonPostWithResponse(const std::string& path, Handler&& handler) {
        const std::string origin  = cfg_.corsOrigin;
        const bool        devMode = cfg_.devMode;

        drogon::app().registerHandler(path,
            [this, h = std::forward<Handler>(handler), origin, devMode]
            (const HttpRequestPtr& req, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                applySecurityHeaders_(res, origin);

                try {
                    const auto body   = nlohmann::json::parse(req->body());
                    const auto result = h(req, body, res);  // res passed by ref
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(result.dump());
                } catch (const nlohmann::json::exception& e) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error",  "Invalid JSON"},
                        {"detail", devMode ? e.what() : "Bad request"}
                    }.dump());
                } catch (const std::exception& e) {
                    LOG_ERROR << "[http] POST " << req->getPath()
                              << " exception: " << e.what();
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{
                        {"error",  "Internal server error"},
                        {"detail", devMode ? e.what() : "An internal error occurred"}
                    }.dump());
                }
                cb(res);
            },
            {drogon::Post});
    }

    template<typename Handler>
    void addJsonGet(const std::string& path, Handler&& handler) {
        const std::string origin  = cfg_.corsOrigin;
        const bool        devMode = cfg_.devMode;

        drogon::app().registerHandler(path,
            [this, h = std::forward<Handler>(handler), origin, devMode]
            (const HttpRequestPtr& req, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->addHeader("Content-Type", "application/json");
                applySecurityHeaders_(res, origin);

                try {
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(h(req).dump());
                } catch (const std::exception& e) {
                    LOG_ERROR << "[http] GET " << req->getPath()
                              << " exception: " << e.what();
                    res->setStatusCode(drogon::k500InternalServerError);
                    res->setBody(nlohmann::json{
                        {"error", devMode ? e.what() : "An internal error occurred"}
                    }.dump());
                }
                cb(res);
            },
            {drogon::Get});
    }

    void addCorsOptions(const std::string& path) {
        const std::string origin = cfg_.corsOrigin;

        drogon::app().registerHandler(path,
            [this, origin](const HttpRequestPtr&, HttpCallback&& cb) {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::k204NoContent);
                applySecurityHeaders_(res, origin);
                if (!origin.empty()) {
                    res->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                    res->addHeader("Access-Control-Allow-Headers",
                                   "Content-Type, Authorization, X-Requested-With");
                }
                cb(res);
            },
            {drogon::Options});
    }

    // ----------------------------------------------------------
    // Lifecycle
    // ----------------------------------------------------------
    void start() { drogon::app().run(); }
    void stop()  { drogon::app().quit(); }

    // ----------------------------------------------------------
    // Accessors
    // ----------------------------------------------------------
    drogon::HttpAppFramework& app() { return drogon::app(); }
    const HttpConfig& config() const { return cfg_; }

private:
    HttpConfig cfg_;
    trantor::AsyncFileLogger asyncLogger_;

    /** Apply security headers + CORS to @p res. Call on every outgoing response. */
    void applySecurityHeaders_(const HttpResponsePtr& res,
                                const std::string&     origin) const {
        // Defense-in-depth headers — safe for all response types
        res->addHeader("X-Content-Type-Options", "nosniff");
        res->addHeader("X-Frame-Options",        "DENY");
        res->addHeader("Referrer-Policy",        "strict-origin-when-cross-origin");
        // CSP: for pure JSON API responses nothing should execute.
        // Static HTML/JS files are served by Drogon directly and will need a
        // separate hook; this at minimum protects API endpoints.
        res->addHeader("Content-Security-Policy", "default-src 'none'");

        if (!origin.empty())
            res->addHeader("Access-Control-Allow-Origin", origin);
    }
};

} // namespace odoo::infrastructure
