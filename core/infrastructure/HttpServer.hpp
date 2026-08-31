#pragma once
#include <drogon/drogon.h>
#include <trantor/utils/AsyncFileLogger.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>

namespace cerp::infrastructure {

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
     * @brief Comma-separated addresses of reverse proxies in front of us.
     *
     * S-40: X-Forwarded-For / X-Real-IP are honoured for rate limiting only
     * when the immediate peer appears in this list; otherwise the socket
     * address is used. Defaults to loopback, which matches the intended
     * deployment (nginx on the same host, app bound to 127.0.0.1).
     *
     * Set to "" to disable header trust entirely (direct-exposure setups).
     */
    std::string trustedProxies = "127.0.0.1,::1";

    /**
     * @brief Idle session lifetime, in minutes.
     *
     * Sessions expire this long after their last use and are reclaimed by the
     * eviction timer (S-43). Lower values reduce the window in which a stolen
     * session id is useful; higher values mean fewer surprise logouts.
     */
    int sessionTtlMinutes = 60;

    /**
     * @brief Root directory for static file serving.
     *
     * When non-empty, Drogon serves files from this directory for any
     * request that doesn't match a registered API route.
     *
     * For the reference ERP OWL frontend, point this at the reference ERP web addon's
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
     * @brief File served for the application shell.
     * Only used when docRoot is non-empty.
     */
    std::string indexFile   = "index.html";

    /**
     * @brief Path the ERP application (and its login page) is served at.
     *
     * "/" belongs to the public website: a visitor arriving at the domain
     * should see the site, not a login form for a system they have no account
     * on. The application answers at /login instead (docs/126).
     *
     * config.json:  "app_path": "/login"
     */
    std::string appPath = "/login";

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

    /**
     * @brief How many rotated log files to keep. 0 = keep every one.
     *
     * trantor rolls the log by size and, on its own, keeps every roll forever:
     * a long-lived checkout had accumulated 1,807 files (29 MB) with nothing
     * pruning them. setMaxFiles() existed and was simply never called
     * (docs/092).
     *
     * config/system.cfg:  log_max_files = 30
     */
    size_t logMaxFiles = 30;

    /**
     * @brief Roll the log once it reaches this many bytes. 0 = trantor default.
     *
     * config/system.cfg:  log_size_limit_mb = 20
     */
    uint64_t logSizeLimitBytes = 20ULL * 1024 * 1024;
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
 *   // Serve the reference ERP OWL frontend:
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

            // setFileName takes the directory and the base name SEPARATELY.
            // Passing "log/system" as the base name writes to the right place
            // — fileFullName_ is just path + base + ext — but the rotation
            // bookkeeping scans `path` for files beginning with `base`, so it
            // looked in "./" for names starting with "log/system" and found
            // none. Retention could never have worked, whatever the limit was
            // set to (docs/092).
            const std::string dir  = logPath.has_parent_path()
                                   ? logPath.parent_path().string() : std::string(".");
            std::string       base = logPath.stem().string();      // "system.log" → "system"
            if (base.empty()) base = "system";

            asyncLogger_.setFileName(base, ".log", dir);
            // Retention. Without these two the logger rolls forever and nothing
            // ever deletes a roll — 1,807 files had accumulated (docs/092).
            if (cfg_.logSizeLimitBytes > 0) asyncLogger_.setFileSizeLimit(cfg_.logSizeLimitBytes);
            if (cfg_.logMaxFiles > 0)       asyncLogger_.setMaxFiles(cfg_.logMaxFiles);
            asyncLogger_.startLogging();
            trantor::Logger::setOutputFunction(
                [this](const char* msg, const uint64_t len) {
                    asyncLogger_.output(msg, len);
                },
                [this]() { asyncLogger_.flush(); }
            );
            std::cout << "[c-erp] Logging to file: " << cfg_.logFile << "\n";
        }

        app.addListener(cfg_.host, cfg_.port)
           .setThreadNum(cfg_.threads)
           // Drogon's own default is about 1 MB, which is below the size of an
           // ordinary phone photo: the website media upload (docs/124) caps
           // itself at 8 MB for images and 24 MB for video, and without this every
           // image over ~1 MB died with
           // a bare 413 before the handler ever ran — so the handler's own cap
           // was unreachable and its explanation never shown. The headroom is
           // deliberate: the route that cares enforces its own limit and says
           // why, and this only stops something absurd reaching the process.
           .setClientMaxBodySize(28 * 1024 * 1024);

        // Static file serving — must be configured before run()
        if (!cfg_.docRoot.empty()) {
            app.setDocumentRoot(cfg_.docRoot);
            app.setFileTypes({"html","js","css","png","jpg","jpeg",
                              "gif","svg","ico","woff","woff2","ttf",
                              "eot","map","json","xml","txt"});
            // "/" → index.html
            // Where the application shell lives. Not hardcoded to "/" any
            // more: the public website is the front door of the product, so
            // "/" belongs to the website module and the ERP answers at
            // /login (docs/126). index.html references its assets with
            // absolute paths (/lib, /src), so it loads correctly from any
            // path this is set to.
            app.registerHandler(cfg_.appPath,
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

} // namespace cerp::infrastructure
