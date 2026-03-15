#pragma once
#include "HttpServer.hpp"
#include "SessionManager.hpp"
#include "../../core/factories/Factories.hpp"
#include "../../core/interfaces/IViewModel.hpp"
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
 * Mounts four routes on HttpServer that the OWL/JS frontend uses:
 *   POST /web/dataset/call_kw          — standard model method calls
 *   POST /web/dataset/call             — legacy alias
 *   POST /web/dataset/fields_get       — fields_get shortcut
 *   GET  /web/session/get_session_info — session introspection
 *   POST /web/session/authenticate     — direct login endpoint (Odoo 19)
 *
 * Session cookie:
 *   Resolved from Cookie header on every request.
 *   After authenticate() succeeds the session_id is set as a
 *   Set-Cookie header on the response so the browser stores it.
 *
 * Public methods (bypass auth check):
 *   authenticate, get_session_info, logout, list_db, server_version
 */
class JsonRpcDispatcher {
public:
    JsonRpcDispatcher(std::shared_ptr<core::ViewModelFactory> vmFactory,
                      std::shared_ptr<SessionManager>         sessions)
        : vmFactory_(std::move(vmFactory))
        , sessions_ (std::move(sessions))
    {}

    void registerRoutes(HttpServer& http) {
        // Primary call_kw endpoint
        http.addJsonPostWithResponse("/web/dataset/call_kw",
            [this](const HttpRequestPtr& req,
                   const nlohmann::json& body,
                   HttpResponsePtr&      res) {
                return handleCallKw_(req, body, res);
            });
        http.addCorsOptions("/web/dataset/call_kw");

        // Legacy alias
        http.addJsonPostWithResponse("/web/dataset/call",
            [this](const HttpRequestPtr& req,
                   const nlohmann::json& body,
                   HttpResponsePtr&      res) {
                return handleCallKw_(req, body, res);
            });
        http.addCorsOptions("/web/dataset/call");

        // fields_get shortcut
        http.addJsonPost("/web/dataset/fields_get",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleFieldsGet_(req, body);
            });
        http.addCorsOptions("/web/dataset/fields_get");

        // Session info
        http.addJsonGet("/web/session/get_session_info",
            [this](const HttpRequestPtr& req) {
                return handleGetSessionInfo_(req);
            });

        // Direct authenticate endpoint (Odoo 19 webclient uses this)
        http.addJsonPostWithResponse("/web/session/authenticate",
            [this](const HttpRequestPtr& req,
                   const nlohmann::json& body,
                   HttpResponsePtr&      res) {
                return handleCallKw_(req, body, res);
            });
        http.addCorsOptions("/web/session/authenticate");
    }

private:
    // ----------------------------------------------------------
    // Main handler — with response access for Set-Cookie
    // ----------------------------------------------------------

    nlohmann::json handleCallKw_(const HttpRequestPtr& req,
                                  const nlohmann::json& body,
                                  HttpResponsePtr&      res) {
        const auto id = body.value("id", nlohmann::json{});

        try {
            // Support /web/session/authenticate body format:
            // { "params": { "db": "odoo", "login": "admin", "password": "x" } }
            nlohmann::json workBody = body;
            if (workBody.contains("params") &&
                !workBody["params"].contains("model") &&
                workBody["params"].contains("login")) {
                // Rewrite to call_kw format for res.users.authenticate
                auto& p = workBody["params"];
                workBody["params"] = {
                    {"model",  "res.users"},
                    {"method", "authenticate"},
                    {"args",   nlohmann::json::array({
                        p.value("db",       std::string{}),
                        p.value("login",    std::string{}),
                        p.value("password", std::string{})
                    })},
                    {"kwargs", nlohmann::json::object()},
                };
            }

            const auto& params = workBody.at("params");
            auto call = parseCallKw_(params);

            // Resolve (or create) session
            const std::string sid = resolveOrCreateSid_(req);
            auto sessionOpt = sessions_->get(sid);
            Session session  = sessionOpt.value_or(Session{});

            // Auth check
            if (!isPublicMethod_(call.method) && !session.isAuthenticated())
                return errorResponse_(id, 100, "Session expired",
                                      "Please authenticate first.");

            // Inject session_id + uid into context
            if (!call.kwargs.contains("context"))
                call.kwargs["context"] = nlohmann::json::object();
            call.kwargs["context"]["uid"]        = session.uid;
            call.kwargs["context"]["session_id"] = sid;

            auto vm     = vmFactory_->create(call.model, core::Lifetime::Transient);
            auto result = vm->callKw(call);

            // After authenticate: set session cookie so browser persists it
            if (call.method == "authenticate" && result.contains("uid") &&
                result["uid"].is_number_integer() && result["uid"].get<int>() > 0) {
                const std::string cookieSid = result.value("session_id", sid);
                res->addHeader("Set-Cookie",
                    std::string(SessionManager::cookieName()) +
                    "=" + cookieSid +
                    "; HttpOnly; Path=/; SameSite=Lax");
            }

            return successResponse_(id, result);

        } catch (const std::out_of_range& e) {
            return errorResponse_(id, 400, "Missing required field", e.what());
        } catch (const std::exception& e) {
            return errorResponse_(id, 200, "Odoo Server Error", e.what());
        }
    }

    nlohmann::json handleGetSessionInfo_(const HttpRequestPtr& req) {
        const std::string sid = resolveOrCreateSid_(req);
        auto session = sessions_->get(sid).value_or(Session{});
        return successResponse_(nullptr, session.toJson());
    }

    nlohmann::json handleFieldsGet_(const HttpRequestPtr& req,
                                     const nlohmann::json& body) {
        auto patched = body;
        if (patched.contains("params"))
            patched["params"]["method"] = "fields_get";
        HttpResponsePtr dummy = drogon::HttpResponse::newHttpResponse();
        return handleCallKw_(req, patched, dummy);
    }

    // ----------------------------------------------------------
    // Session helpers
    // ----------------------------------------------------------

    std::string resolveOrCreateSid_(const HttpRequestPtr& req) {
        const std::string cookie = req->getHeader("Cookie");
        const std::string sid    = SessionManager::extractFromCookie(cookie);
        if (!sid.empty() && sessions_->get(sid).has_value()) return sid;
        return sessions_->create();
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
        return {{"jsonrpc","2.0"}, {"id",id}, {"result",result}};
    }

    static nlohmann::json errorResponse_(const nlohmann::json& id,
                                          int                    code,
                                          const std::string&     message,
                                          const std::string&     detail = "") {
        return {
            {"jsonrpc","2.0"}, {"id",id},
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

    std::shared_ptr<core::ViewModelFactory> vmFactory_;
    std::shared_ptr<SessionManager>         sessions_;
};

} // namespace odoo::infrastructure
