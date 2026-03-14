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
};


// ============================================================
// Request / Response type aliases
// ============================================================
using HttpRequestPtr  = drogon::HttpRequestPtr;
using HttpResponsePtr = drogon::HttpResponsePtr;
using HttpCallback    = std::function<void(const HttpResponsePtr&)>;


// ============================================================
// HttpService
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
class HttpService {
public:
    explicit HttpService(const HttpConfig& cfg = {}) : cfg_(cfg) {
        auto& app = drogon::app();

        app.setLogLevel(cfg_.logRequests
            ? trantor::Logger::kInfo
            : trantor::Logger::kWarn);

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

    HttpService(const HttpService&)            = delete;
    HttpService& operator=(const HttpService&) = delete;

    // ----------------------------------------------------------
    // Route registration helpers
    // ----------------------------------------------------------

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
    void start() { drogon::app().run(); }
    void stop()  { drogon::app().quit(); }

    // ----------------------------------------------------------
    // Accessors
    // ----------------------------------------------------------
    drogon::HttpAppFramework& app() { return drogon::app(); }
    const HttpConfig& config() const { return cfg_; }

private:
    HttpConfig cfg_;
};

} // namespace odoo::infrastructure