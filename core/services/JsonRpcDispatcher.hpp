#pragma once
#include "HttpService.hpp"       // HttpRequestPtr, HttpCallback — no drogon.h needed here
#include "SessionManager.hpp"
#include "factories/Factories.hpp"
#include "interfaces/IViewModel.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <unordered_set>

namespace odoo::infrastructure {

// ============================================================
// JsonRpcDispatcher
// ============================================================
/**
 * @brief Routes Odoo JSON-RPC 2.0 requests to the correct ViewModel.
 *
 * Mounts four routes on HttpService that the OWL/JS frontend uses:
 *
 *   POST /web/dataset/call_kw          — standard model method calls
 *   POST /web/dataset/call             — legacy alias (same handler)
 *   POST /web/dataset/fields_get       — fields_get shortcut
 *   GET  /web/session/get_session_info — session introspection
 *
 * Wire format (request):
 * @code
 * {
 *   "jsonrpc": "2.0",
 *   "method":  "call",
 *   "id":      1,
 *   "params": {
 *     "model":  "res.partner",
 *     "method": "search_read",
 *     "args":   [[["active","=",true]]],
 *     "kwargs": { "fields": ["name","email"], "limit": 80 }
 *   }
 * }
 * @endcode
 *
 * Wire format (success response):
 * @code
 * { "jsonrpc": "2.0", "id": 1, "result": { ... } }
 * @endcode
 *
 * Wire format (error response):
 * @code
 * {
 *   "jsonrpc": "2.0", "id": 1,
 *   "error": { "code": 200, "message": "...", "data": { "name": "...", "message": "..." } }
 * }
 * @endcode
 *
 * Session handling:
 *   The session_id cookie is extracted from the Cookie header.
 *   If missing or expired an anonymous session is created on-the-fly
 *   (the OWL frontend always expects a valid session cookie to be present).
 *
 * Public methods (no auth required):
 *   "authenticate", "get_session_info", "logout", "list_db", "server_version"
 */
class JsonRpcDispatcher {
public:
    JsonRpcDispatcher(std::shared_ptr<core::ViewModelFactory> vmFactory,
                      std::shared_ptr<SessionManager>         sessions)
        : vmFactory_(std::move(vmFactory))
        , sessions_ (std::move(sessions))
    {}

    // ----------------------------------------------------------
    // Route registration
    // ----------------------------------------------------------

    /**
     * @brief Mount all JSON-RPC routes onto the HTTP server.
     * Called once from Container::boot() after all modules are loaded.
     */
    void registerRoutes(HttpService& http) {
        // Primary endpoint
        http.addJsonPost("/web/dataset/call_kw",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleCallKw_(req, body);
            });
        http.addCorsOptions("/web/dataset/call_kw");

        // Legacy alias used by older Odoo JS bundles
        http.addJsonPost("/web/dataset/call",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleCallKw_(req, body);
            });
        http.addCorsOptions("/web/dataset/call");

        // fields_get shortcut (some Odoo versions call this separately)
        http.addJsonPost("/web/dataset/fields_get",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleFieldsGet_(req, body);
            });
        http.addCorsOptions("/web/dataset/fields_get");

        // Session info (GET — no body)
        http.addJsonGet("/web/session/get_session_info",
            [this](const HttpRequestPtr& req) {
                return handleGetSessionInfo_(req);
            });
    }

private:
    // ----------------------------------------------------------
    // Handlers
    // ----------------------------------------------------------

    nlohmann::json handleCallKw_(const HttpRequestPtr&  req,
                                  const nlohmann::json&  body) {
        const auto id = body.value("id", nlohmann::json{});

        try {
            const auto& params = body.at("params");
            auto call = parseCallKw_(params);

            auto session = resolveSession_(req);

            if (!isPublicMethod_(call.method) && !session.isAuthenticated())
                return errorResponse_(id, 100, "Session expired",
                                      "Please authenticate first.");

            // Inject uid into kwargs.context so ViewModels can read it
            if (!call.kwargs.contains("context"))
                call.kwargs["context"] = nlohmann::json::object();
            call.kwargs["context"]["uid"] = session.uid;

            auto vm     = vmFactory_->create(call.model, core::Lifetime::Transient);
            auto result = vm->callKw(call);
            return successResponse_(id, result);

        } catch (const std::out_of_range& e) {
            return errorResponse_(id, 400, "Missing required field", e.what());
        } catch (const std::exception& e) {
            return errorResponse_(id, 200, "Odoo Server Error", e.what());
        }
    }

    nlohmann::json handleGetSessionInfo_(const HttpRequestPtr& req) {
        return successResponse_(nullptr, resolveSession_(req).toJson());
    }

    nlohmann::json handleFieldsGet_(const HttpRequestPtr&  req,
                                     const nlohmann::json&  body) {
        auto patched = body;
        if (patched.contains("params"))
            patched["params"]["method"] = "fields_get";
        return handleCallKw_(req, patched);
    }

    // ----------------------------------------------------------
    // Parsing
    // ----------------------------------------------------------

    static core::CallKwArgs parseCallKw_(const nlohmann::json& params) {
        core::CallKwArgs call;
        call.model  = params.at("model").get<std::string>();
        call.method = params.at("method").get<std::string>();
        call.args   = params.value("args",   nlohmann::json::array());
        call.kwargs = params.value("kwargs", nlohmann::json::object());
        return call;
    }

    // ----------------------------------------------------------
    // Session
    // ----------------------------------------------------------

    Session resolveSession_(const HttpRequestPtr& req) {
        const std::string cookie = req->getHeader("Cookie");
        const std::string sid    = SessionManager::extractFromCookie(cookie);

        if (!sid.empty()) {
            auto s = sessions_->get(sid);
            if (s.has_value()) return *s;
        }

        // Always return a valid session object — create anonymous if needed
        const std::string newSid = sessions_->create();
        return sessions_->get(newSid).value_or(Session{});
    }

    // ----------------------------------------------------------
    // Auth bypass list
    // ----------------------------------------------------------

    static bool isPublicMethod_(const std::string& method) {
        static const std::unordered_set<std::string> kPublic = {
            "authenticate",
            "get_session_info",
            "logout",
            "list_db",
            "server_version",
        };
        return kPublic.count(method) > 0;
    }

    // ----------------------------------------------------------
    // JSON-RPC 2.0 envelope builders
    // ----------------------------------------------------------

    static nlohmann::json successResponse_(const nlohmann::json& id,
                                            const nlohmann::json& result) {
        return {
            {"jsonrpc", "2.0"},
            {"id",      id},
            {"result",  result},
        };
    }

    static nlohmann::json errorResponse_(const nlohmann::json& id,
                                          int                    code,
                                          const std::string&     message,
                                          const std::string&     detail = "") {
        return {
            {"jsonrpc", "2.0"},
            {"id",      id},
            {"error", {
                {"code",    code},
                {"message", message},
                {"data", {
                    {"name",    "odoo.exceptions.UserError"},
                    {"message", detail.empty() ? message : detail},
                }},
            }},
        };
    }

    // ----------------------------------------------------------
    // Members
    // ----------------------------------------------------------
    std::shared_ptr<core::ViewModelFactory> vmFactory_;
    std::shared_ptr<SessionManager>         sessions_;
};

} // namespace odoo::infrastructure