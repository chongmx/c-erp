#pragma once
#include "HttpServer.hpp"
#include "SessionManager.hpp"
#include "Errors.hpp"
#include "TtlCache.hpp"
#include "../../core/factories/Factories.hpp"
#include "../../core/interfaces/IViewModel.hpp"
#include "../../core/interfaces/IView.hpp"
#include <drogon/Cookie.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

namespace odoo::infrastructure {

// ============================================================
// LoginRateLimiter
// ============================================================
/**
 * @brief Per-IP sliding-window rate limiter for the authenticate endpoint.
 *
 * Allows up to kMaxAttempts failed login attempts within kWindowSeconds.
 * A successful login resets the counter for that IP.
 * Thread-safe via a single mutex; the map is pruned on each check to
 * prevent unbounded growth from unique IPs.
 */
class LoginRateLimiter {
public:
    static constexpr int kMaxAttempts   = 10;
    static constexpr int kWindowSeconds = 300; // 5-minute window

    /** Returns true if the IP is allowed to attempt login. */
    bool allow(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        // PERF-07: only prune when the full window has elapsed to avoid O(n) on every call
        if ((now - lastPrune_) >= std::chrono::seconds(kWindowSeconds)) {
            prune_(now);
            lastPrune_ = now;
        }
        auto& entry = table_[ip];
        if (entry.count >= kMaxAttempts &&
            (now - entry.windowStart) < std::chrono::seconds(kWindowSeconds))
            return false;
        if ((now - entry.windowStart) >= std::chrono::seconds(kWindowSeconds)) {
            entry.windowStart = now;
            entry.count = 0;
        }
        return true;
    }

    /** Call on every failed attempt. */
    void recordFailure(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        auto& entry = table_[ip];
        if ((now - entry.windowStart) >= std::chrono::seconds(kWindowSeconds)) {
            entry.windowStart = now;
            entry.count = 0;
        }
        ++entry.count;
    }

    /** Call on successful login — resets counter for this IP. */
    void recordSuccess(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mutex_);
        table_.erase(ip);
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Clock::time_point windowStart = Clock::now();
        int               count       = 0;
    };

    // Remove entries whose window has fully expired to bound memory usage.
    void prune_(Clock::time_point now) {
        const auto cutoff = std::chrono::seconds(kWindowSeconds * 2);
        for (auto it = table_.begin(); it != table_.end(); ) {
            if ((now - it->second.windowStart) > cutoff)
                it = table_.erase(it);
            else
                ++it;
        }
    }

    Clock::time_point                      lastPrune_ = Clock::now();
    std::mutex                             mutex_;
    std::unordered_map<std::string, Entry> table_;
};


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
                      std::shared_ptr<SessionManager>         sessions,
                      std::shared_ptr<core::ViewFactory>      viewFactory = nullptr,
                      bool                                    secureCookies = false,
                      bool                                    devMode = false)
        : vmFactory_    (std::move(vmFactory))
        , sessions_     (std::move(sessions))
        , viewFactory_  (std::move(viewFactory))
        , secureCookies_(secureCookies)
        , devMode_      (devMode)
    {}

    /**
     * @brief Invalidate the currency cache (PERF-D).
     *
     * Call this after any write to the res_currency table so the next
     * session_info fetch re-queries the database immediately.
     * Modules that expose currency write/create/unlink should call this
     * on their ViewModel's write path.
     */
    void invalidateCurrencyCache() { currencyCache_.invalidateAll(); }

    /**
     * @brief Invalidate the fields_get cache (PERF-D).
     *
     * Call when model field metadata changes (rare at runtime; normally only
     * on server restart).  Pass a model name to evict a single entry, or
     * call with no argument to flush the entire cache.
     */
    void invalidateFieldsGetCache()
        { fieldsGetCache_.invalidateAll(); }
    void invalidateFieldsGetCache(const std::string& model)
        { fieldsGetCache_.invalidate(model); }

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

        // Action load — POST /web/action/load {params:{action_id:N}}
        http.addJsonPostWithResponse("/web/action/load",
            [this](const HttpRequestPtr& req,
                   const nlohmann::json& body,
                   HttpResponsePtr&      res) {
                return handleActionLoad_(req, body, res);
            });
        http.addCorsOptions("/web/action/load");

        // Breadcrumbs stub
        http.addJsonPost("/web/action/load_breadcrumbs",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleActionLoadBreadcrumbs_(req, body);
            });
        http.addCorsOptions("/web/action/load_breadcrumbs");
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

            // Resolve session: cookie first, then body context (for cookie-less clients)
            const std::string sid = resolveSessionId_(req, call);
            auto sessionOpt = sessions_->get(sid);
            Session session  = sessionOpt.value_or(Session{});

            const std::string bodySidDbg = call.kwargs.contains("context")
                ? call.kwargs["context"].value("session_id", std::string{"(none)"})
                : "(no context)";
            LOG_INFO << "[rpc] " << call.model << "." << call.method
                     << " sid=" << sid.substr(0, 8) << "... uid=" << session.uid
                     << " body_sid=" << bodySidDbg.substr(0, 12);

            // Rate-limit the login endpoint before doing any work
            if (call.method == "authenticate") {
                const std::string ip = req->getPeerAddr().toIp();
                if (!rateLimiter_.allow(ip))
                    return errorResponse_(id, 429, "Too many requests",
                                         "Too many failed login attempts. Try again later.");
            }

            // Auth check
            if (!isPublicMethod_(call.method) && !session.isAuthenticated())
                return errorResponse_(id, 100, "Session expired",
                                      "Please authenticate first.");

            // Model-level access check (SEC-04)
            if (session.isAuthenticated())
                checkModelAccess_(call.model, session);

            // Inject session identity into context for record-rule evaluation (S-30)
            if (!call.kwargs.contains("context"))
                call.kwargs["context"] = nlohmann::json::object();
            call.kwargs["context"]["uid"]        = session.uid;
            call.kwargs["context"]["session_id"] = sid;
            call.kwargs["context"]["company_id"] = session.companyId;
            call.kwargs["context"]["partner_id"] = session.partnerId;
            call.kwargs["context"]["is_admin"]   = session.isAdmin;
            {
                nlohmann::json gArr = nlohmann::json::array();
                for (int g : session.groupIds) gArr.push_back(g);
                call.kwargs["context"]["group_ids"] = std::move(gArr);
            }

            // get_views is handled via ViewFactory when the ViewModel doesn't implement it
            if (call.method == "get_views" && viewFactory_) {
                return successResponse_(id, handleGetViews_(call));
            }

            // PERF-D: fields_get returns pure metadata — serve from cache (300 s TTL)
            if (call.method == "fields_get") {
                if (auto cached = fieldsGetCache_.get(call.model))
                    return successResponse_(id, *cached);
            }

            auto vm     = vmFactory_->create(call.model, core::Lifetime::Transient);
            auto result = vm->callKw(call);

            // PERF-D: cache fields_get result
            if (call.method == "fields_get" && result.is_object())
                fieldsGetCache_.set(call.model, result, 300);

            // After authenticate: record rate-limiter outcome
            if (call.method == "authenticate") {
                const std::string ip = req->getPeerAddr().toIp();
                const bool ok = result.contains("uid") &&
                                result["uid"].is_number_integer() &&
                                result["uid"].get<int>() > 0;
                if (ok) rateLimiter_.recordSuccess(ip);
                else    rateLimiter_.recordFailure(ip);
            }

            // After authenticate: sync auth data into dispatcher's SM and set cookie
            if (call.method == "authenticate" && result.contains("uid") &&
                result["uid"].is_number_integer() && result["uid"].get<int>() > 0) {
                const std::string cookieSid = result.value("session_id", sid);
                const bool updated = sessions_->update(cookieSid, [&result](Session& s) {
                    s.uid     = result["uid"].get<int>();
                    s.login   = result.value("login", std::string{});
                    s.db      = result.value("db",    std::string{});
                    s.name    = result.value("name",  std::string{});
                    s.isAdmin = result.value("is_admin", false);
                    if (result.contains("partner_id") && result["partner_id"].is_number_integer())
                        s.partnerId = result["partner_id"].get<int>();
                    if (result.contains("company_id") && result["company_id"].is_number_integer())
                        s.companyId = result["company_id"].get<int>();
                    s.context = {{"uid", s.uid}, {"lang", "en_US"}, {"tz", "UTC"}};
                    if (result.contains("group_ids") && result["group_ids"].is_array()) {
                        s.groupIds.clear();
                        for (const auto& g : result["group_ids"])
                            if (g.is_number_integer()) s.groupIds.push_back(g.get<int>());
                    }
                });
                LOG_INFO << "[auth] session sync for " << cookieSid
                         << " uid=" << result["uid"].get<int>()
                         << " updated=" << updated;
                drogon::Cookie c(SessionManager::cookieName(), cookieSid);
                c.setHttpOnly(true);
                c.setPath("/");
                c.setSameSite(drogon::Cookie::SameSite::kLax);
                c.setMaxAge(3600);
                if (secureCookies_) c.setSecure(true);
                res->addCookie(c);
            }

            return successResponse_(id, result);

        } catch (const AccessDeniedError& e) {
            // SEC-25: authorization errors are always shown — client must know why
            return errorResponse_(id, 403, "Access Denied", e.what());
        } catch (const PoolExhaustedException& e) {
            // PERF-C: pool exhausted — return 503 so load balancers can route elsewhere
            LOG_ERROR << "[rpc] pool exhausted: " << e.what();
            return errorResponse_(id, 503, "Service Unavailable",
                                  "The server is temporarily overloaded. Please retry.");
        } catch (const std::out_of_range& e) {
            LOG_ERROR << "[rpc] " << e.what();
            return errorResponse_(id, 400, "Missing required field",
                                  devMode_ ? e.what() : "An internal error occurred");
        } catch (const std::exception& e) {
            // SEC-25: gate internal details (SQL errors, stack traces) behind devMode
            LOG_ERROR << "[rpc] " << e.what();
            return errorResponse_(id, 200, "Odoo Server Error",
                                  devMode_ ? e.what() : "An internal error occurred");
        }
    }

    nlohmann::json handleGetSessionInfo_(const HttpRequestPtr& req) {
        const std::string sid     = resolveOrCreateSid_(req);
        const Session     session = sessions_->get(sid).value_or(Session{});

        nlohmann::json info = session.toJson();

        // Standard Odoo session_info fields expected by the webclient
        info["server_version"]   = "19.0+e (odoo-cpp)";
        info["is_public"]        = !session.isAuthenticated();
        info["is_internal_user"] = session.isAuthenticated() && session.hasGroup(2);
        info["username"]         = session.login;

        info["user_context"] = {
            {"uid",        session.uid},
            {"lang",       session.context.value("lang", "en_US")},
            {"tz",         session.context.value("tz",   "UTC")},
        };

        // Minimal user_companies structure (populated when a company is set)
        if (session.companyId > 0) {
            info["user_companies"] = {
                {"current_company", session.companyId},
                {"allowed_companies", {
                    {std::to_string(session.companyId), {
                        {"id",       session.companyId},
                        {"name",     session.companyName},
                        {"sequence", 1},
                        {"child_ids", nlohmann::json::array()},
                        {"parent_id", false},
                    }},
                }},
            };
        }

        // Active currencies indexed by code — cached 60 s (PERF-D)
        nlohmann::json currencies = nlohmann::json::object();
        if (vmFactory_) {
            try {
                // Fast path: serve from cache
                if (auto cached = currencyCache_.get("currencies"))  {
                    currencies = *cached;
                } else {
                    // Slow path: query DB, then cache result
                    core::CallKwArgs cc;
                    cc.model  = "res.currency";
                    cc.method = "search_read";
                    cc.args   = nlohmann::json::array({nlohmann::json::array()});
                    cc.kwargs = {{"fields", nlohmann::json::array(
                                    {"name", "symbol", "position", "decimal_places"})}};
                    auto vm   = vmFactory_->create("res.currency", core::Lifetime::Transient);
                    auto rows = vm->callKw(cc);
                    if (rows.is_array()) {
                        for (const auto& row : rows) {
                            const std::string code   = row.value("name",           std::string{});
                            const std::string symbol = row.value("symbol",         code);
                            const std::string pos    = row.value("position",       std::string{"after"});
                            const int         dec    = row.value("decimal_places", 2);
                            if (!code.empty())
                                currencies[code] = {
                                    {"symbol",   symbol},
                                    {"position", pos},
                                    {"digits",   nlohmann::json::array({0, dec})},
                                };
                        }
                    }
                    currencyCache_.set("currencies", currencies, 60);
                }
            } catch (...) { /* currencies are nice-to-have — never break session_info */ }
        }
        info["currencies"] = currencies;

        return successResponse_(nullptr, info);
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
        const std::string sid = SessionManager::extractFromCookie(req->getHeader("Cookie"));
        if (!sid.empty() && sessions_->get(sid).has_value()) return sid;
        return sessions_->create();
    }

    // Resolve session from cookie or a flat body param (for non-callKw endpoints)
    std::string resolveFromBodyOrCookie_(const HttpRequestPtr& req,
                                          const nlohmann::json& params) {
        const std::string cookieSid = SessionManager::extractFromCookie(
            req->getHeader("Cookie"));
        if (!cookieSid.empty() && sessions_->get(cookieSid).has_value())
            return cookieSid;
        const std::string bodySid = params.value("session_id", std::string{});
        if (!bodySid.empty() && sessions_->get(bodySid).has_value())
            return bodySid;
        return sessions_->create();
    }

    std::string resolveSessionId_(const HttpRequestPtr& req,
                                   const core::CallKwArgs& call) {
        // 1. Try cookie
        const std::string cookieSid = SessionManager::extractFromCookie(
            req->getHeader("Cookie"));
        if (!cookieSid.empty() && sessions_->get(cookieSid).has_value())
            return cookieSid;

        // 2. Try session_id from kwargs context (sent by JS client in body)
        if (call.kwargs.contains("context")) {
            const std::string bodySid =
                call.kwargs["context"].value("session_id", std::string{});
            if (!bodySid.empty() && sessions_->get(bodySid).has_value())
                return bodySid;
        }

        // 3. Create fresh anonymous session
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

    // Model-level access control (SEC-04 / SEC-26).
    //
    // Policy (deny-by-default):
    //   1. Admins bypass all checks.
    //   2. Models in kAllowed: require auth only (any BASE_INTERNAL user).
    //   3. Models in kRequired: require the specified group.
    //   4. Models not in either: require BASE_INTERNAL (2) — deny-by-default for unknown models.
    //      This prevents newly-registered ViewModels from being accidentally exposed.
    //
    // Group IDs (modules/auth/Groups.hpp):
    //   2  = BASE_INTERNAL        3  = BASE_ADMIN
    //   4  = SETTINGS_CONFIGURATION
    //   5  = ACCOUNT_BILLING      6  = ACCOUNT_MANAGER
    //   7  = SALES_USER           8  = SALES_MANAGER
    //   9  = PURCHASE_USER       10  = PURCHASE_MANAGER
    //  11  = INVENTORY_USER      12  = INVENTORY_MANAGER
    //  13  = MRP_USER            14  = MRP_MANAGER
    //  15  = HR_EMPLOYEE         16  = HR_MANAGER
    static void checkModelAccess_(const std::string& model, const Session& session) {
        if (session.isAdmin) return;

        // Models accessible to any authenticated internal user (no extra group needed)
        static const std::unordered_set<std::string> kAllowed = {
            "ir.ui.menu",            // sidebar rendering
            "ir.actions.act_window", // navigation
            "res.currency",          // currency display
            "res.partner",           // contacts (all employees)
            "res.users",             // user data (AuthViewModel has its own authz)
            "uom.uom",               // units of measure (product display)
            "product.product",       // products (sales/purchase/inventory all need this)
            "product.category",      // product categories
            "product.supplierinfo",  // vendor pricelist (viewed in product form)
            "mail.message",          // chatter (gated by the parent document's access)
            "portal.partner",        // portal admin ViewModel (internal RPC only)
        };
        if (kAllowed.count(model)) {
            // Still require at least a logged-in internal user (BASE_INTERNAL = 2)
            if (!session.hasGroup(2))
                throw AccessDeniedError(
                    "Access denied: internal login required to access " + model);
            return;
        }

        // Models that require a specific module group
        static const std::unordered_map<std::string, int> kRequired = {
            // Accounting — ACCOUNT_BILLING (5)
            {"account.move",          5},
            {"account.move.line",     5},
            {"account.account",       5},
            {"account.journal",       5},
            {"account.tax",           5},
            {"account.payment",       5},
            {"account.payment.term",  5},
            // HR — HR_EMPLOYEE (15)
            {"hr.employee",          15},
            {"hr.department",        15},
            {"hr.job",               15},
            // Inventory — INVENTORY_USER (11)
            {"stock.move",           11},
            {"stock.picking",        11},
            {"stock.location",       11},
            {"stock.picking.type",   11},
            {"stock.warehouse",      11},
            // Sales — SALES_USER (7)
            {"sale.order",            7},
            {"sale.order.line",       7},
            // Purchase — PURCHASE_USER (9)
            {"purchase.order",        9},
            {"purchase.order.line",   9},
            // Manufacturing — MRP_USER (13)
            {"mrp.bom",              13},
            {"mrp.production",       13},
            // Settings — SETTINGS_CONFIGURATION (4)
            {"res.company",           4},
            {"res.groups",            4},
            // System parameters — BASE_ADMIN (3): may contain SMTP credentials / API keys
            {"ir.config.parameter",   3},
        };
        auto it = kRequired.find(model);
        if (it != kRequired.end()) {
            if (!session.hasGroup(it->second))
                throw AccessDeniedError(
                    "Access denied: insufficient permissions to access " + model);
            return;
        }

        // Deny-by-default (SEC-26): unknown models require BASE_INTERNAL (2)
        if (!session.hasGroup(2))
            throw AccessDeniedError(
                "Access denied: internal login required to access " + model);
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

    // ----------------------------------------------------------
    // get_views — builds view descriptor from ViewFactory
    // args[0] = [[view_id_or_false, view_type], ...]
    // ----------------------------------------------------------
    nlohmann::json handleGetViews_(const core::CallKwArgs& call) {
        const std::string& model = call.model;

        // Parse requested views from args[0]
        std::vector<std::string> requestedTypes;
        if (!call.args.empty() && call.args[0].is_array()) {
            for (const auto& pair : call.args[0]) {
                if (pair.is_array() && pair.size() >= 2 && pair[1].is_string())
                    requestedTypes.push_back(pair[1].get<std::string>());
            }
        }
        if (requestedTypes.empty()) requestedTypes = {"list", "form"};

        nlohmann::json views    = nlohmann::json::object();
        nlohmann::json allFields= nlohmann::json::object();

        for (const auto& vtype : requestedTypes) {
            nlohmann::json viewEntry;
            viewEntry["id"]    = 0;
            viewEntry["type"]  = vtype;
            viewEntry["model"] = model;
            viewEntry["toolbar"] = nlohmann::json::object();

            if (viewFactory_ && viewFactory_->hasView(model, vtype)) {
                auto view  = viewFactory_->getView(model, vtype);
                auto flds  = view->fields();   // must be a named variable — items() holds a ref
                viewEntry["arch"]   = view->arch();
                viewEntry["fields"] = flds;
                for (auto& [k,v] : flds.items())
                    allFields[k] = v;
            } else {
                // Fallback: populate fields from fields_get so generic list/form work
                viewEntry["arch"] = "<" + vtype + "/>";
                try {
                    auto vm = vmFactory_->create(model, core::Lifetime::Transient);
                    core::CallKwArgs fg;
                    fg.model  = model;
                    fg.method = "fields_get";
                    fg.args   = nlohmann::json::array();
                    fg.kwargs = nlohmann::json::object();
                    auto flds = vm->callKw(fg);
                    viewEntry["fields"] = flds.is_object() ? flds : nlohmann::json::object();
                    for (auto& [k, v] : viewEntry["fields"].items())
                        allFields[k] = v;
                } catch (...) {
                    viewEntry["fields"] = nlohmann::json::object();
                }
            }
            views[vtype] = std::move(viewEntry);
        }

        // models section: field metadata keyed by model name
        nlohmann::json models = nlohmann::json::object();
        if (!allFields.empty())
            models[model] = {{"fields", allFields}};

        return {{"views", views}, {"models", models}};
    }

    // ----------------------------------------------------------
    // /web/action/load
    // ----------------------------------------------------------
    nlohmann::json handleActionLoad_(const HttpRequestPtr& req,
                                      const nlohmann::json& body,
                                      HttpResponsePtr&      /*res*/) {
        const auto id  = body.value("id", nlohmann::json{});
        try {
            const auto& params = body.value("params", nlohmann::json::object());
            // action_id can be int or string path
            int actionId = 0;
            if (params.contains("action_id")) {
                if (params["action_id"].is_number_integer())
                    actionId = params["action_id"].get<int>();
                else if (params["action_id"].is_string()) {
                    try { actionId = std::stoi(params["action_id"].get<std::string>()); }
                    catch (...) {}
                }
            }

            // Auth check
            const std::string sid     = resolveFromBodyOrCookie_(req, params);
            const Session     session = sessions_->get(sid).value_or(Session{});
            if (!session.isAuthenticated())
                return errorResponse_(id, 100, "Session expired", "Please authenticate first.");

            // Delegate to ir.actions.act_window viewmodel
            auto vm = vmFactory_->create("ir.actions.act_window", core::Lifetime::Transient);
            core::CallKwArgs call;
            call.model  = "ir.actions.act_window";
            call.method = "read";
            call.args   = nlohmann::json::array({nlohmann::json::array({actionId})});
            call.kwargs = nlohmann::json::object();

            auto rows = vm->callKw(call);
            if (!rows.is_array() || rows.empty())
                return errorResponse_(id, 404, "Action not found",
                                      "No action with id " + std::to_string(actionId));

            // Build full action dict
            nlohmann::json row  = rows[0];
            nlohmann::json act  = row;
            act["id"]           = actionId;
            act["type"]         = "ir.actions.act_window";
            act["display_name"] = row.value("name", std::string{});
            act["xml_id"]       = false;
            act["binding_model_id"]    = false;
            act["binding_type"]        = "action";
            act["binding_view_types"]  = "list,form";

            // Build views array from view_mode
            nlohmann::json viewsArr = nlohmann::json::array();
            std::string viewMode = row.value("view_mode", std::string{"list,form"});
            std::istringstream ss(viewMode);
            std::string tok;
            while (std::getline(ss, tok, ','))
                viewsArr.push_back(nlohmann::json::array({false, tok}));
            act["views"] = viewsArr;

            return successResponse_(id, act);

        } catch (const std::exception& e) {
            LOG_ERROR << "[rpc/action_load] " << e.what();
            return errorResponse_(id, 200, "Odoo Server Error",
                                  devMode_ ? e.what() : "An internal error occurred");
        }
    }

    // Stub — webclient calls this to restore breadcrumbs after reload
    nlohmann::json handleActionLoadBreadcrumbs_(const HttpRequestPtr& /*req*/,
                                                 const nlohmann::json& body) {
        const auto id = body.value("id", nlohmann::json{});
        return successResponse_(id, nlohmann::json::array());
    }

    std::shared_ptr<core::ViewModelFactory> vmFactory_;
    std::shared_ptr<SessionManager>         sessions_;
    std::shared_ptr<core::ViewFactory>      viewFactory_;
    LoginRateLimiter                        rateLimiter_;
    bool                                    secureCookies_ = false;
    bool                                    devMode_       = false;

    // PERF-D: TTL caches for quasi-static data
    TtlCache<std::string, nlohmann::json>   currencyCache_;   // 60 s TTL — res.currency rows
    TtlCache<std::string, nlohmann::json>   fieldsGetCache_;  // 300 s TTL — keyed by model name
};

} // namespace odoo::infrastructure
