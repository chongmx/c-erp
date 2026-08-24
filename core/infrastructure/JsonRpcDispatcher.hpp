#pragma once
#include "HttpServer.hpp"
#include "SessionManager.hpp"
#include "ClientIp.hpp"
#include "Errors.hpp"
#include "TtlCache.hpp"
#include "../../core/factories/Factories.hpp"
#include "../../core/ControlPlane.hpp"
#include "../../core/DbBackup.hpp"
#include "../../core/DbExplorer.hpp"
#include "../../core/interfaces/IViewModel.hpp"
#include "AuthService.hpp"          // password re-verification for destructive DB ops
#include "AuditService.hpp"
#include "../../core/interfaces/IView.hpp"
#include <drogon/Cookie.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <fstream>

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
                      bool                                    devMode = false,
                      const std::string& trustedProxies = "127.0.0.1,::1",
                      std::shared_ptr<DbConnection>           db = nullptr)
        : vmFactory_    (std::move(vmFactory))
        , sessions_     (std::move(sessions))
        , viewFactory_  (std::move(viewFactory))
        , secureCookies_(secureCookies)
        , devMode_      (devMode)
        , clientIp_     (trustedProxies)
        , db_           (std::move(db))
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

        // Multi-company (docs/072 Phase 2): the companies the current identity can
        // reach, and cross-tenant SSO switching between them (the top-bar switcher).
        http.addJsonPost("/web/session/companies",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleListCompanies_(req, body);
            });
        http.addCorsOptions("/web/session/companies");
        http.addJsonPostWithResponse("/web/session/switch_company",
            [this](const HttpRequestPtr& req, const nlohmann::json& body, HttpResponsePtr& res) {
                return handleSwitchCompany_(req, body, res);
            });
        http.addCorsOptions("/web/session/switch_company");

        // Phase 3 (docs/072): pull the shared catalogue into this tenant, and a
        // consolidated cross-company report over the identity's companies.
        http.addJsonPost("/web/session/import_shared_products",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleImportSharedProducts_(req, body);
            });
        http.addCorsOptions("/web/session/import_shared_products");
        http.addJsonPost("/web/session/consolidated",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleConsolidated_(req, body);
            });
        http.addCorsOptions("/web/session/consolidated");

        // Pre-login company chooser: the companies an email/login can reach.
        http.addJsonPost("/web/session/lookup_companies",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleLookupCompanies_(req, body);
            });
        http.addCorsOptions("/web/session/lookup_companies");
        // Control-plane admin (identity memberships + shared catalogue), admin-gated.
        http.addJsonPost("/web/control/admin",
            [this](const HttpRequestPtr& req, const nlohmann::json& body) {
                return handleControlAdmin_(req, body);
            });
        http.addCorsOptions("/web/control/admin");

        // In-app Database section (docs/075) — per-tenant snapshot/restore, admin
        // + password gated. Every op targets ONLY the caller's own tenant db.
        http.addJsonPost("/web/db/list",    [this](const HttpRequestPtr& r, const nlohmann::json& b){ return handleDbList_(r, b); });
        http.addJsonPost("/web/db/backup",  [this](const HttpRequestPtr& r, const nlohmann::json& b){ return handleDbBackup_(r, b); });
        http.addJsonPost("/web/db/restore", [this](const HttpRequestPtr& r, const nlohmann::json& b){ return handleDbRestore_(r, b); });
        http.addJsonPost("/web/db/delete",  [this](const HttpRequestPtr& r, const nlohmann::json& b){ return handleDbDelete_(r, b); });
        http.addCorsOptions("/web/db/list");
        http.addCorsOptions("/web/db/backup");
        http.addCorsOptions("/web/db/restore");
        http.addCorsOptions("/web/db/delete");
        http.addCorsOptions("/web/db/upload");
        // upload (raw .dump body) + download (file response) — raw drogon handlers.
        drogon::app().registerHandler("/web/db/upload",
            [this](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
                nlohmann::json j = handleDbUploadRaw_(req);
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(j.dump());
                cb(r);
            }, {drogon::Post});
        drogon::app().registerHandler("/web/db/download",
            [this](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
                cb(handleDbDownload_(req));
            }, {drogon::Get});

        // Database Tools (docs/093) — read-only browser, SQL console and schema
        // map. One endpoint, `op` selects the query. Admin-gated and scoped to
        // the caller's own tenant, exactly like /web/db/* above.
        http.addJsonPost("/web/dbtool", [this](const HttpRequestPtr& r, const nlohmann::json& b) {
            return handleDbTool_(r, b);
        });
        http.addCorsOptions("/web/dbtool");

        // docs/094 — in-DATABASE company switching. Distinct from
        // /web/session/switch_company above, which moves the session to another
        // tenant database entirely (docs/072). This one stays in the same
        // database and changes which company's records are visible.
        http.addJsonPost("/web/session/my_companies",
            [this](const HttpRequestPtr& r, const nlohmann::json& b) { return handleMyCompanies_(r, b); });
        http.addJsonPost("/web/session/set_active_company",
            [this](const HttpRequestPtr& r, const nlohmann::json& b) { return handleSetActiveCompany_(r, b); });
        http.addJsonPost("/web/company/access",
            [this](const HttpRequestPtr& r, const nlohmann::json& b) { return handleCompanyAccess_(r, b); });
        http.addCorsOptions("/web/session/my_companies");
        http.addCorsOptions("/web/session/set_active_company");
        http.addCorsOptions("/web/company/access");
    }

    // ── docs/094: company switcher + access administration ────────

    /// The companies this user may work in, and which one is active.
    nlohmann::json handleMyCompanies_(const HttpRequestPtr& req, const nlohmann::json& body) {
        core::CallKwArgs call;
        if (body.contains("params") && body["params"].is_object()) call.kwargs = body["params"];
        const std::string sid = resolveSessionId_(req, call);
        auto opt = sessions_->get(sid);
        if (!opt || !opt->isAuthenticated()) return {{"error", "not authenticated"}};

        // Re-read rather than trust the session copy, so a membership granted or
        // revoked since login takes effect without a re-login.
        Session s = *opt;
        loadAllowedCompanies_(s);
        sessions_->update(sid, [&s](Session& t) {
            t.allowedCompanyIds   = s.allowedCompanyIds;
            t.allowedCompanyNames = s.allowedCompanyNames;
            t.companyId           = s.companyId;
            t.companyName         = s.companyName;
        });

        nlohmann::json arr = nlohmann::json::array();
        for (std::size_t i = 0; i < s.allowedCompanyIds.size(); ++i)
            arr.push_back({{"id",   s.allowedCompanyIds[i]},
                           {"name", i < s.allowedCompanyNames.size() ? s.allowedCompanyNames[i] : ""}});
        return {{"ok", true}, {"active", s.companyId}, {"companies", arr}};
    }

    /// Switch the active company. Only ever to one this user is a member of.
    nlohmann::json handleSetActiveCompany_(const HttpRequestPtr& req, const nlohmann::json& body) {
        core::CallKwArgs call;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        if (p.is_object()) call.kwargs = p;
        const std::string sid = resolveSessionId_(req, call);
        auto opt = sessions_->get(sid);
        if (!opt || !opt->isAuthenticated()) return {{"error", "not authenticated"}};

        int want = 0;
        if (p.is_object() && p.contains("company_id") && p["company_id"].is_number_integer())
            want = p["company_id"].get<int>();
        if (want <= 0) return {{"error", "company_id is required"}};

        Session s = *opt;
        loadAllowedCompanies_(s);          // authoritative, from the database
        if (!s.mayUseCompanyId(want))
            // Deliberately the same wording whether the company does not exist
            // or the user simply is not in it — probing this endpoint should not
            // enumerate the companies in the database.
            return {{"error", "You do not have access to that company."}};

        std::string name;
        try {
            TenantScope scope(db_.get(), s.db);
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto r = txn.exec("SELECT name FROM res_company WHERE id=$1", pqxx::params{want});
            if (!r.empty() && !r[0][0].is_null()) name = r[0][0].c_str();
            // Remember it, so the next login lands in the same place.
            txn.exec("UPDATE res_users SET company_id=$1 WHERE id=$2",
                     pqxx::params{want, s.uid});
            txn.commit();
        } catch (const std::exception& ex) {
            LOG_ERROR << "[company/switch] " << ex.what();
            return {{"error", "Could not switch company."}};
        }

        sessions_->update(sid, [want, &name, &s](Session& t) {
            t.companyId           = want;
            t.companyName         = name;
            t.allowedCompanyIds   = s.allowedCompanyIds;
            t.allowedCompanyNames = s.allowedCompanyNames;
        });
        if (AuditService::ready())
            AuditService::instance().log("res.company", "switch:" + std::to_string(want),
                                         std::vector<int>{want}, s.uid);
        LOG_INFO << "[company/switch] uid=" << s.uid << " -> company " << want;
        return {{"ok", true}, {"company_id", want}, {"company_name", name}};
    }

    /// Admin: read and change which users may act for which companies.
    nlohmann::json handleCompanyAccess_(const HttpRequestPtr& req, const nlohmann::json& body) {
        Session s; nlohmann::json err;
        if (!dbAdmin_(req, body, s, err)) return err;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        const std::string op = p.is_object() ? p.value("op", std::string{}) : std::string{};
        auto I = [&](const char* k) {
            return (p.is_object() && p.contains(k) && p[k].is_number_integer()) ? p[k].get<int>() : 0;
        };
        try {
            TenantScope scope(db_.get(), s.db);
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};

            if (op == "list") {
                nlohmann::json companies = nlohmann::json::array(), users = nlohmann::json::array();
                for (const auto& r : txn.exec(
                         "SELECT id, name, COALESCE(active,true) FROM res_company ORDER BY id"))
                    companies.push_back({{"id", r[0].as<int>()}, {"name", r[1].is_null() ? "" : r[1].c_str()},
                                         {"active", r[2].as<bool>(true)}});
                // Two flat queries stitched in C++ rather than array_agg —
                // pqxx has no typed array accessor, and parsing "{1,2}" back
                // out of a text column is more code than this.
                std::map<int, nlohmann::json> allowedBy;
                for (const auto& r : txn.exec(
                         "SELECT user_id, company_id FROM res_company_users_rel ORDER BY user_id, company_id")) {
                    const int uid = r[0].as<int>();
                    if (!allowedBy.count(uid)) allowedBy[uid] = nlohmann::json::array();
                    allowedBy[uid].push_back(r[1].as<int>());
                }
                for (const auto& r : txn.exec(
                         "SELECT id, login, company_id FROM res_users WHERE active ORDER BY id")) {
                    const int uid = r[0].as<int>();
                    users.push_back({{"id", uid}, {"login", r[1].c_str()},
                                     {"active_company", r[2].is_null() ? 0 : r[2].as<int>()},
                                     {"allowed", allowedBy.count(uid) ? allowedBy[uid]
                                                                      : nlohmann::json::array()}});
                }
                txn.commit();
                return {{"ok", true}, {"companies", companies}, {"users", users}};
            }
            if (op == "grant") {
                if (!I("user_id") || !I("company_id")) return {{"error", "user_id and company_id are required"}};
                txn.exec("INSERT INTO res_company_users_rel (company_id, user_id) VALUES ($1,$2) "
                         "ON CONFLICT DO NOTHING", pqxx::params{I("company_id"), I("user_id")});
                txn.commit();
                return {{"ok", true}};
            }
            if (op == "revoke") {
                if (!I("user_id") || !I("company_id")) return {{"error", "user_id and company_id are required"}};
                // A user with no companies at all can see nothing and cannot log
                // in usefully, so the last one is not removable.
                auto n = txn.exec("SELECT count(*) FROM res_company_users_rel WHERE user_id=$1",
                                  pqxx::params{I("user_id")});
                if (n[0][0].as<int>(0) <= 1)
                    return {{"error", "A user must belong to at least one company."}};
                txn.exec("DELETE FROM res_company_users_rel WHERE company_id=$1 AND user_id=$2",
                         pqxx::params{I("company_id"), I("user_id")});
                // If that was their active company, move them to one they still have.
                txn.exec("UPDATE res_users u SET company_id = ("
                         "  SELECT company_id FROM res_company_users_rel WHERE user_id=u.id ORDER BY company_id LIMIT 1) "
                         "WHERE u.id=$1 AND u.company_id=$2", pqxx::params{I("user_id"), I("company_id")});
                txn.commit();
                return {{"ok", true}};
            }
            txn.commit();
            return {{"error", "unknown op"}};
        } catch (const std::exception& ex) {
            LOG_ERROR << "[company/access] " << ex.what();
            return {{"error", devMode_ ? ex.what() : "An internal error occurred"}};
        }
    }

    // ── Multi-company: company chooser + cross-tenant switch (docs/072) ──────

    // Load a tenant user's session fields (uid/name/company/groups) WITHOUT a
    // password — used for cross-tenant SSO, where the control plane has already
    // vouched that the caller's identity owns this login in `tenantDb`.
    bool loadTenantUser_(const std::string& tenantDb, const std::string& login, Session& out) {
        if (!db_) return false;
        struct Scope { DbConnection* d; ~Scope(){ if (d) d->clearCurrentTenant(); } } scope{db_.get()};
        db_->setCurrentTenant(tenantDb);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec(
            "SELECT u.id, u.login, u.partner_id, u.company_id, COALESCE(p.name,u.login) AS name "
            "FROM res_users u LEFT JOIN res_partner p ON p.id=u.partner_id "
            "WHERE u.login=$1 AND u.active=TRUE LIMIT 1", pqxx::params{login});
        if (r.empty()) return false;
        out.uid       = r[0]["id"].as<int>();
        out.login     = r[0]["login"].c_str();
        out.partnerId = r[0]["partner_id"].is_null() ? 0 : r[0]["partner_id"].as<int>();
        out.companyId = r[0]["company_id"].is_null() ? 0 : r[0]["company_id"].as<int>();
        out.name      = r[0]["name"].c_str();
        auto g = txn.exec("SELECT gid FROM res_groups_users_rel WHERE uid=$1", pqxx::params{out.uid});
        for (const auto& row : g) out.groupIds.push_back(row["gid"].as<int>());
        out.isAdmin = std::find(out.groupIds.begin(), out.groupIds.end(), 3) != out.groupIds.end();
        if (out.companyId > 0) {
            auto cn = txn.exec("SELECT name FROM res_company WHERE id=$1", pqxx::params{out.companyId});
            if (!cn.empty() && !cn[0][0].is_null()) out.companyName = cn[0][0].c_str();
        }
        loadAllowedCompaniesTxn_(txn, out);
        return true;
    }

    // ── docs/094: in-database multi-company ───────────────────────
    //
    // Which companies this session may act for. Read from the database on every
    // login and every switch, never from anything the client sends.
    //
    // A user with no rows in res_company_users_rel falls back to the company on
    // their own record: an upgraded database has no rel rows for users created
    // before this feature, and locking those people out of their own data would
    // be a worse failure than the one this prevents.
    void loadAllowedCompaniesTxn_(pqxx::transaction_base& txn, Session& s) {
        s.allowedCompanyIds.clear();
        s.allowedCompanyNames.clear();
        try {
            auto r = txn.exec(
                "SELECT c.id, c.name FROM res_company_users_rel rel "
                "JOIN res_company c ON c.id = rel.company_id "
                "WHERE rel.user_id = $1 AND c.active ORDER BY c.id",
                pqxx::params{s.uid});
            for (const auto& row : r) {
                s.allowedCompanyIds.push_back(row[0].as<int>());
                s.allowedCompanyNames.push_back(row[1].is_null() ? "" : row[1].c_str());
            }
        } catch (const std::exception&) { /* table not created yet on first boot */ }

        if (s.allowedCompanyIds.empty() && s.companyId > 0) {
            s.allowedCompanyIds.push_back(s.companyId);
            s.allowedCompanyNames.push_back(s.companyName);
        }
        // The active company must be one the user is allowed into. If it is not
        // — the membership was revoked while they were logged in — fall back to
        // the first allowed one rather than leaving them pointed at data they
        // may no longer read.
        if (!s.allowedCompanyIds.empty()) {
            bool ok = false;
            for (int c : s.allowedCompanyIds) if (c == s.companyId) { ok = true; break; }
            if (!ok) {
                s.companyId   = s.allowedCompanyIds.front();
                s.companyName = s.allowedCompanyNames.empty() ? "" : s.allowedCompanyNames.front();
            }
        }
    }

    void loadAllowedCompanies_(Session& s) {
        if (!db_ || s.uid <= 0) return;
        try {
            TenantScope scope(db_.get(), s.db);
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            loadAllowedCompaniesTxn_(txn, s);
        } catch (const std::exception& ex) {
            LOG_WARN << "[company] could not load allowed companies for uid="
                     << s.uid << ": " << ex.what();
        }
    }

    nlohmann::json handleListCompanies_(const HttpRequestPtr& req, const nlohmann::json& body) {
        core::CallKwArgs call;
        if (body.contains("params") && body["params"].is_object()) call.kwargs = body["params"];
        const std::string sid = resolveSessionId_(req, call);
        const Session s = sessions_->get(sid).value_or(Session{});
        nlohmann::json out = nlohmann::json::array();
        if (!s.isAuthenticated() || !core::ControlPlane::ready()) return out;
        const std::string identity = s.identity.empty() ? s.login : s.identity;
        for (const auto& m : core::ControlPlane::instance().membershipsFor(identity)) {
            std::string label = m.tenantDb;
            if (db_) {
                try {
                    db_->setCurrentTenant(m.tenantDb);
                    auto conn = db_->acquire();
                    pqxx::work txn{conn.get()};
                    auto r = txn.exec("SELECT name FROM res_company ORDER BY id LIMIT 1");
                    if (!r.empty() && !r[0][0].is_null()) label = r[0][0].c_str();
                    db_->clearCurrentTenant();
                } catch (...) { db_->clearCurrentTenant(); }
            }
            out.push_back({{"db", m.tenantDb}, {"name", label}, {"current", m.tenantDb == s.db}});
        }
        return out;
    }

    nlohmann::json handleSwitchCompany_(const HttpRequestPtr& req, const nlohmann::json& body,
                                        HttpResponsePtr& res) {
        core::CallKwArgs call;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        if (p.is_object()) call.kwargs = p;
        const std::string sid = resolveSessionId_(req, call);
        const Session cur = sessions_->get(sid).value_or(Session{});
        if (!cur.isAuthenticated())        return {{"error", "not authenticated"}};
        if (!core::ControlPlane::ready())  return {{"error", "company switching is not enabled"}};
        const std::string target = p.is_object() ? p.value("company", std::string{}) : std::string{};
        if (target.empty())                return {{"error", "company required"}};
        const std::string identity  = cur.identity.empty() ? cur.login : cur.identity;
        const std::string localLogin = core::ControlPlane::instance().loginFor(identity, target);
        if (localLogin.empty())            return {{"error", "you are not a member of that company"}};

        Session ns;
        if (!loadTenantUser_(target, localLogin, ns))
            return {{"error", "your account was not found in that company"}};
        ns.identity = identity;
        ns.db       = target;

        const std::string newSid = sessions_->create();
        sessions_->update(newSid, [&ns](Session& s) {
            s.uid = ns.uid; s.login = ns.login; s.db = ns.db; s.identity = ns.identity;
            s.name = ns.name; s.partnerId = ns.partnerId; s.companyId = ns.companyId;
            s.companyName = ns.companyName; s.isAdmin = ns.isAdmin; s.groupIds = ns.groupIds;
            s.context = {{"uid", s.uid}, {"lang", "en_US"}, {"tz", "UTC"}};
        });
        drogon::Cookie c(SessionManager::cookieName(), newSid);
        c.setHttpOnly(true);
        c.setPath("/");
        if (secureCookies_) c.setSecure(true);
        res->addCookie(std::move(c));
        LOG_INFO << "[switch_company] identity=" << identity << " -> db=" << target
                 << " uid=" << ns.uid;
        nlohmann::json out = sessions_->get(newSid).value_or(Session{}).toJson();
        out["session_id"] = newSid;
        return out;
    }

    // Phase 3: pull the shared catalogue into the CURRENT tenant (opt-in). Each
    // shared product is copied once (deduped by default_code) — tenants stay
    // independent; sharing is a one-time import, not a live link.
    nlohmann::json handleImportSharedProducts_(const HttpRequestPtr& req, const nlohmann::json& body) {
        core::CallKwArgs call;
        if (body.contains("params") && body["params"].is_object()) call.kwargs = body["params"];
        const std::string sid = resolveSessionId_(req, call);
        const Session s = sessions_->get(sid).value_or(Session{});
        if (!s.isAuthenticated())         return {{"error", "not authenticated"}};
        if (!core::ControlPlane::ready())  return {{"error", "no shared catalogue configured"}};
        const auto shared = core::ControlPlane::instance().sharedProducts();
        int imported = 0;
        if (db_) {
            TenantScope scope(db_.get(), s.db);
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (const auto& sp : shared) {
                auto r = txn.exec(
                    "INSERT INTO product_product (name, default_code, list_price) "
                    "SELECT $1::text, $2::text, $3::bigint WHERE NOT EXISTS "
                    "(SELECT 1 FROM product_product WHERE default_code = $2::text)",
                    pqxx::params{sp.name, sp.code, sp.listPrice});
                imported += static_cast<int>(r.affected_rows());
            }
            txn.commit();
        }
        return {{"imported", imported}, {"available", static_cast<int>(shared.size())}};
    }

    // Phase 3: consolidated cross-company figures over the identity's companies
    // (each queried in its own database). One row per company.
    nlohmann::json handleConsolidated_(const HttpRequestPtr& req, const nlohmann::json& body) {
        core::CallKwArgs call;
        if (body.contains("params") && body["params"].is_object()) call.kwargs = body["params"];
        const std::string sid = resolveSessionId_(req, call);
        const Session s = sessions_->get(sid).value_or(Session{});
        nlohmann::json out = nlohmann::json::array();
        if (!s.isAuthenticated() || !core::ControlPlane::ready() || !db_) return out;
        const std::string identity = s.identity.empty() ? s.login : s.identity;
        for (const auto& m : core::ControlPlane::instance().membershipsFor(identity)) {
            nlohmann::json row = {{"db", m.tenantDb}, {"name", m.tenantDb}};
            try {
                TenantScope scope(db_.get(), m.tenantDb);
                auto conn = db_->acquire();
                pqxx::work txn{conn.get()};
                auto cn = txn.exec("SELECT name FROM res_company ORDER BY id LIMIT 1");
                if (!cn.empty() && !cn[0][0].is_null()) row["name"] = cn[0][0].c_str();
                row["partners"] = txn.exec("SELECT count(*) FROM res_partner")[0][0].as<long long>(0);
                row["invoiced"] = txn.exec(
                    "SELECT COALESCE(SUM(amount_total),0) FROM account_move "
                    "WHERE move_type='out_invoice' AND state='posted'")[0][0].as<long long>(0);
            } catch (const std::exception&) {
                row["error"] = "unavailable";
            }
            out.push_back(std::move(row));
        }
        return out;
    }

    // Pre-login chooser: the companies an email/login can reach (control plane).
    // Pre-auth by design (login page). Returns [] when the control plane is off.
    nlohmann::json handleLookupCompanies_(const HttpRequestPtr&, const nlohmann::json& body) {
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        const std::string login = p.is_object() ? p.value("login", std::string{}) : std::string{};
        nlohmann::json out = nlohmann::json::array();
        if (login.empty() || !core::ControlPlane::ready() || !db_) return out;
        for (const auto& m : core::ControlPlane::instance().membershipsFor(login)) {
            std::string label = m.tenantDb;
            try {
                db_->setCurrentTenant(m.tenantDb);
                auto conn = db_->acquire();
                pqxx::work txn{conn.get()};
                auto r = txn.exec("SELECT name FROM res_company ORDER BY id LIMIT 1");
                if (!r.empty() && !r[0][0].is_null()) label = r[0][0].c_str();
                db_->clearCurrentTenant();
            } catch (...) { db_->clearCurrentTenant(); }
            out.push_back({{"db", m.tenantDb}, {"name", label}, {"login", m.localLogin}});
        }
        return out;
    }

    // Control-plane admin: manage identity memberships + the shared catalogue.
    // Admin-gated (any tenant admin — a super-admin role is a future refinement).
    nlohmann::json handleControlAdmin_(const HttpRequestPtr& req, const nlohmann::json& body) {
        core::CallKwArgs call;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        if (p.is_object()) call.kwargs = p;
        const std::string sid = resolveSessionId_(req, call);
        const Session s = sessions_->get(sid).value_or(Session{});
        if (!s.isAuthenticated() || !s.isAdmin) return {{"error", "administrator access required"}};
        if (!core::ControlPlane::ready())        return {{"error", "control plane is not enabled"}};
        auto& cp = core::ControlPlane::instance();
        const std::string op = p.is_object() ? p.value("op", std::string{}) : std::string{};
        auto S = [&](const char* k){ return p.is_object() ? p.value(k, std::string{}) : std::string{}; };

        if (op == "list_memberships") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& m : cp.allMemberships())
                arr.push_back({{"identity", m.identity}, {"tenant_db", m.tenantDb},
                               {"local_login", m.localLogin}, {"active", m.active}});
            return {{"memberships", arr}};
        }
        if (op == "add_membership") {
            if (S("identity").empty() || S("tenant_db").empty() || S("local_login").empty())
                return {{"error", "identity, tenant_db and local_login are required"}};
            cp.upsertMembership(S("identity"), S("tenant_db"), S("local_login"));
            return {{"ok", true}};
        }
        if (op == "remove_membership") {
            cp.removeMembership(S("identity"), S("tenant_db"));
            return {{"ok", true}};
        }
        if (op == "list_shared") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& sp : cp.sharedProducts())
                arr.push_back({{"code", sp.code}, {"name", sp.name}, {"list_price", sp.listPrice}});
            return {{"shared_products", arr}};
        }
        if (op == "add_shared") {
            if (S("code").empty()) return {{"error", "code is required"}};
            long long price = 0;
            if (p.is_object() && p.contains("list_price") && p["list_price"].is_number())
                price = static_cast<long long>(p["list_price"].get<double>());
            cp.upsertSharedProduct(S("code"), S("name"), price);
            return {{"ok", true}};
        }
        if (op == "remove_shared") {
            cp.removeSharedProduct(S("code"));
            return {{"ok", true}};
        }
        return {{"error", "unknown op"}};
    }

    // ── In-app Database section (docs/075) ─────────────────────────
    // Security envelope: authenticated ADMIN only; every operation targets ONLY
    // the caller's own tenant database (session.db) — never a client-supplied
    // name — so one company can never dump/restore another's; destructive ops
    // (restore/import) additionally require the caller's password; all audited.

    bool dbAdmin_(const HttpRequestPtr& req, const nlohmann::json& body,
                  Session& out, nlohmann::json& err) {
        core::CallKwArgs call;
        if (body.contains("params") && body["params"].is_object()) call.kwargs = body["params"];
        const std::string sid = resolveSessionId_(req, call);
        out = sessions_->get(sid).value_or(Session{});
        if (!out.isAuthenticated()) { err = {{"error", "not authenticated"}}; return false; }
        if (!out.isAdmin)           { err = {{"error", "administrator access required"}}; return false; }
        return true;
    }
    std::string dbBackupDir_(const Session& s) const {
        std::string t = s.db.empty() ? (db_ ? db_->defaultTenant() : std::string("default")) : s.db;
        std::string safe; for (char c : t) if (std::isalnum((unsigned char)c) || c == '_' || c == '-') safe += c;
        return "backups/" + (safe.empty() ? std::string("default") : safe);
    }
    DbConfig dbTenantCfg_(const Session& s) const {
        return db_ ? db_->tenantConfig(s.db.empty() ? db_->defaultTenant() : s.db) : DbConfig{};
    }
    bool verifySessionPassword_(const Session& s, const std::string& password) {
        if (password.empty() || !db_ || s.uid <= 0) return false;
        try {
            TenantScope scope(db_.get(), s.db);
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto r = txn.exec("SELECT password FROM res_users WHERE id=$1", pqxx::params{s.uid});
            if (r.empty() || r[0][0].is_null()) return false;
            return odoo::modules::auth::AuthService::verifyPassword(password, r[0][0].c_str());
        } catch (...) { return false; }
    }
    void dbAudit_(const Session& s, const std::string& op) {
        if (AuditService::ready())
            AuditService::instance().log("db.backup", op, std::vector<int>{}, s.uid);
    }

    nlohmann::json handleDbList_(const HttpRequestPtr& req, const nlohmann::json& body) {
        Session s; nlohmann::json err;
        if (!dbAdmin_(req, body, s, err)) return err;
        return {{"ok", true}, {"company", s.db}, {"backups", core::DbBackup::list(dbBackupDir_(s))}};
    }
    nlohmann::json handleDbBackup_(const HttpRequestPtr& req, const nlohmann::json& body) {
        Session s; nlohmann::json err;
        if (!dbAdmin_(req, body, s, err)) return err;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        const std::string label = p.is_object() ? p.value("label", std::string{}) : std::string{};
        auto r = core::DbBackup::backup(dbBackupDir_(s), dbTenantCfg_(s), label);
        dbAudit_(s, "backup");
        LOG_INFO << "[db.backup] company=" << s.db << " uid=" << s.uid << " -> " << r.value("file", std::string{});
        return r;
    }
    nlohmann::json handleDbRestore_(const HttpRequestPtr& req, const nlohmann::json& body) {
        Session s; nlohmann::json err;
        if (!dbAdmin_(req, body, s, err)) return err;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        const std::string file = p.value("file", std::string{});
        const std::string pw   = p.value("password", std::string{});
        if (!verifySessionPassword_(s, pw)) return {{"error", "password confirmation failed"}};
        auto r = core::DbBackup::restore(dbBackupDir_(s), dbTenantCfg_(s), file);
        dbAudit_(s, "restore:" + file);
        LOG_WARN << "[db.restore] company=" << s.db << " uid=" << s.uid << " file=" << file;
        return r;
    }
    nlohmann::json handleDbDelete_(const HttpRequestPtr& req, const nlohmann::json& body) {
        Session s; nlohmann::json err;
        if (!dbAdmin_(req, body, s, err)) return err;
        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        const std::string file = p.value("file", std::string{});
        if (!core::DbBackup::validFile(file)) return {{"error", "invalid file"}};
        const std::string path = dbBackupDir_(s) + "/" + file;
        if (::unlink(path.c_str()) != 0) return {{"error", "could not delete"}};
        dbAudit_(s, "delete:" + file);
        return {{"ok", true}};
    }
    // Import: raw .dump bytes in the POST body, filename in ?name=. Saved into the
    // caller's tenant dir (non-destructive — the user then restores it explicitly).
    nlohmann::json handleDbUploadRaw_(const HttpRequestPtr& req) {
        core::CallKwArgs call;
        const std::string sid = resolveSessionId_(req, call);
        Session s = sessions_->get(sid).value_or(Session{});
        if (!s.isAuthenticated() || !s.isAdmin) return {{"error", "administrator access required"}};
        std::string base;
        for (char c : std::string(req->getParameter("name")))
            if (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') base += c;
        if (base.find("..") != std::string::npos) return {{"error", "bad file name"}};
        if (base.size() < 5 || base.substr(base.size() - 5) != ".dump")
            base = "import-" + core::DbBackup::stamp() + ".dump";
        const std::string data(req->getBody());
        if (data.empty())                       return {{"error", "empty upload"}};
        if (data.size() > 512ull * 1024 * 1024) return {{"error", "file too large (max 512MB)"}};
        core::dbRunCmd_({"mkdir", "-p", dbBackupDir_(s)});
        std::ofstream f(dbBackupDir_(s) + "/" + base, std::ios::binary);
        if (!f.is_open()) return {{"error", "could not write file"}};
        f.write(data.data(), (std::streamsize)data.size());
        f.close();
        dbAudit_(s, "import:" + base);
        return {{"ok", true}, {"file", base}};
    }
    HttpResponsePtr handleDbDownload_(const HttpRequestPtr& req) {
        core::CallKwArgs call;
        // allow ?session_id= for a plain <a> download link, plus the cookie
        if (!req->getParameter("session_id").empty())
            call.kwargs = {{"context", {{"session_id", std::string(req->getParameter("session_id"))}}}};
        const std::string sid = resolveSessionId_(req, call);
        Session s = sessions_->get(sid).value_or(Session{});
        auto deny = [](drogon::HttpStatusCode c, const char* m) {
            auto r = drogon::HttpResponse::newHttpResponse(); r->setStatusCode(c); r->setBody(m); return r;
        };
        if (!s.isAuthenticated() || !s.isAdmin) return deny(drogon::k403Forbidden, "forbidden");
        const std::string file = std::string(req->getParameter("file"));
        if (!core::DbBackup::validFile(file)) return deny(drogon::k400BadRequest, "bad file");
        const std::string path = dbBackupDir_(s) + "/" + file;
        if (access(path.c_str(), R_OK) != 0)  return deny(drogon::k404NotFound, "not found");
        return drogon::HttpResponse::newFileResponse(path, file, drogon::CT_APPLICATION_OCTET_STREAM);
    }

    // ── Database Tools (docs/093) ──────────────────────────────────
    // The browser, SQL console and schema map behind Settings ▸ Database Tools.
    //
    // Three things make this safe to expose in-app, in order of how much work
    // they do:
    //
    //  1. pqxx::read_transaction — a genuine `BEGIN READ ONLY`. Every write is
    //     refused by PostgreSQL, including the ones a keyword filter misses: a
    //     data-modifying CTE (`WITH x AS (DELETE …) SELECT …`), a volatile
    //     function that writes, DDL. The transaction is never committed.
    //  2. statement_timeout — a cartesian join cannot pin a pool connection.
    //  3. DbExplorer's own checks — one statement per box, an allowlisted
    //     leading keyword, and identifiers resolved against pg_catalog before
    //     they are quoted into SQL (S-49).
    //
    // Plus the same envelope as /web/db/*: authenticated admin only, and scoped
    // to the caller's own tenant, so one company can never read another's rows.
    nlohmann::json handleDbTool_(const HttpRequestPtr& req, const nlohmann::json& body) {
        Session s; nlohmann::json err;
        if (!dbAdmin_(req, body, s, err)) return err;

        const nlohmann::json& p = body.contains("params") ? body["params"] : body;
        const std::string op = p.is_object() ? p.value("op", std::string{}) : std::string{};

        try {
            TenantScope scope(db_.get(), s.db);
            auto conn = db_->acquire();
            pqxx::read_transaction txn{conn.get()};
            // Bound every statement. The console can time out; it cannot hang.
            txn.exec("SET LOCAL statement_timeout = '15s'");

            // The payload key is "data", NOT "result". RpcService._dbPost ends
            // with `data.result ?? data` to peel a JSON-RPC envelope, so a
            // top-level "result" here gets peeled by that line and then peeled
            // AGAIN by dbTool() — which returned {} for every call and made the
            // whole screen render blank. Keeping the key distinct from the
            // JSON-RPC one removes the collision instead of relying on the two
            // unwraps cancelling out.
            if (op == "overview") return {{"ok", true}, {"data", core::DbExplorer::overview(txn)}};
            if (op == "tables")   return {{"ok", true}, {"data", core::DbExplorer::tables(txn)}};
            if (op == "graph")    return {{"ok", true}, {"data", core::DbExplorer::graph(txn)}};
            if (op == "table")    return {{"ok", true}, {"data", core::DbExplorer::table(txn, p.value("table", std::string{}))}};
            if (op == "rows")     return {{"ok", true}, {"data", core::DbExplorer::rows(txn, p)}};
            if (op == "profile")  return {{"ok", true}, {"data", core::DbExplorer::profile(txn, p)}};
            if (op == "query") {
                auto r = core::DbExplorer::query(txn, p);
                LOG_INFO << "[dbtool] query uid=" << s.uid << " db=" << s.db
                         << " rows=" << r.value("row_count", 0LL);
                if (AuditService::ready())
                    AuditService::instance().log("db.tool", "query", std::vector<int>{}, s.uid);
                return {{"ok", true}, {"data", r}};
            }
            return {{"error", "unknown op"}};
            // txn is destroyed un-committed: the read-only snapshot is discarded.
        } catch (const ValidationError& ex) {
            // Ours, and written for the user to read.
            return {{"error", ex.what()}};
        } catch (const pqxx::sql_error& ex) {
            LOG_WARN << "[dbtool/" << op << "] sql: " << ex.what();
            const std::string what = ex.what();
            if (what.find("statement timeout") != std::string::npos)
                return {{"error", "That took longer than 15 seconds and was cancelled. "
                                  "Narrow it down with a WHERE clause or a smaller LIMIT."}};
            // SEC-28 note: the console is the one place where the PostgreSQL
            // error IS the product — "column x does not exist" is what the user
            // came for, and an authenticated admin can already read the whole
            // schema through this very screen. Everywhere else it is masked.
            if (op == "query") return {{"error", what}, {"sql_error", true}};
            return {{"error", devMode_ ? what : "The query could not be run."}};
        } catch (const PoolExhaustedException& ex) {
            LOG_ERROR << "[dbtool] pool: " << ex.what();
            return {{"error", "The server is temporarily overloaded. Please retry."}};
        } catch (const std::exception& ex) {
            LOG_ERROR << "[dbtool/" << op << "] " << ex.what();
            return {{"error", devMode_ ? ex.what() : "An internal error occurred"}};
        }
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

            // Multi-company (docs/072): route this request's DB connections to
            // its tenant database for the request's duration. No-op single-tenant.
            const std::string reqTenant = resolveTenant_(req, call, session);
            TenantScope tenantScope(db_.get(), reqTenant);
            // If authenticating with no explicit db (Host/email routing), bind the
            // new session to the resolved tenant so session.db is correct for every
            // later call. The auth VM echoes args[0] (db) back as result["db"].
            if (call.method == "authenticate" && call.args.is_array() && !call.args.empty()
                && (!call.args[0].is_string() || call.args[0].get<std::string>().empty()))
                call.args[0] = reqTenant;

            const std::string bodySidDbg = call.kwargs.contains("context")
                ? call.kwargs["context"].value("session_id", std::string{"(none)"})
                : "(no context)";
            LOG_INFO << "[rpc] " << call.model << "." << call.method
                     << " sid=" << sid.substr(0, 8) << "... uid=" << session.uid
                     << " body_sid=" << bodySidDbg.substr(0, 12);

            // Rate-limit the login endpoint before doing any work
            if (call.method == "authenticate") {
                // S-40: behind nginx getPeerAddr() is always 127.0.0.1, which
                // would make this one global bucket shared by every user.
                const std::string ip = clientIp_(req);
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
            // docs/094 — assigned, not merged: a client that puts its own
            // allowed_company_ids in the request body has it overwritten here,
            // before any model sees the context.
            {
                nlohmann::json cArr = nlohmann::json::array();
                for (int c : session.allowedCompanyIds) cArr.push_back(c);
                call.kwargs["context"]["allowed_company_ids"] = std::move(cArr);
            }

            // docs/094 — publish the caller for the rest of this request, so a
            // model reached through a hand-written ViewModel that never calls
            // setUserContext is still company-scoped. See core/UserContext.hpp.
            core::UserContext ambient;
            ambient.uid               = session.uid;
            ambient.companyId         = session.companyId;
            ambient.partnerId         = session.partnerId;
            ambient.isAdmin           = session.isAdmin;
            ambient.groupIds          = session.groupIds;
            ambient.allowedCompanyIds = session.allowedCompanyIds;
            core::CurrentUser::Scope userScope(ambient);

            // get_views is handled via ViewFactory when the ViewModel doesn't implement it
            if (call.method == "get_views" && viewFactory_) {
                return successResponse_(id, handleGetViews_(call));
            }

            // PERF-D: fields_get returns pure metadata — serve from cache (300 s TTL)
            if (call.method == "fields_get") {
                if (auto cached = fieldsGetCache_.get(call.model))
                    return successResponse_(id, *cached);
            }

            auto vm = vmFactory_->create(call.model, core::Lifetime::Transient);

            nlohmann::json result;
            if (call.method == "authenticate") {
                // S-49: AuthViewModel THROWS on invalid credentials, so the
                // post-call recordFailure() below was unreachable on exactly
                // the path that matters — the limiter counted nothing and
                // brute-force protection never engaged. Runtime testing found
                // this; static review had passed it repeatedly because the
                // calls look correct in sequence.
                const std::string ip = clientIp_(req);   // S-40
                try {
                    result = vm->callKw(call);
                } catch (...) {
                    rateLimiter_.recordFailure(ip);
                    throw;
                }
                const bool ok = result.contains("uid") &&
                                result["uid"].is_number_integer() &&
                                result["uid"].get<int>() > 0;
                if (ok) rateLimiter_.recordSuccess(ip);
                else    rateLimiter_.recordFailure(ip);
            } else {
                result = vm->callKw(call);
            }

            // PERF-D: cache fields_get result
            if (call.method == "fields_get" && result.is_object())
                fieldsGetCache_.set(call.model, result, 300);

            // After authenticate: sync auth data into dispatcher's SM and set cookie
            if (call.method == "authenticate" && result.contains("uid") &&
                result["uid"].is_number_integer() && result["uid"].get<int>() > 0) {
                // The id the caller arrived with (may be empty — S-43 no longer
                // mints anonymous sessions for unresolved call_kw requests).
                std::string cookieSid = result.value("session_id", sid);
                if (cookieSid.empty() || !sessions_->get(cookieSid).has_value())
                    cookieSid = sessions_->create();

                // S-42: rotate the session id on privilege elevation.
                //
                // Previously the pre-auth id was promoted in place, so an id
                // observed before login stayed valid after it. An attacker
                // could obtain a valid anonymous id (get_session_info returns
                // one), induce the victim to adopt it, and hold an
                // authenticated session once the victim logged in — account
                // takeover with no credential theft. Re-keying makes any
                // previously-observed id worthless.
                const std::string rotated = sessions_->rotate(cookieSid);
                if (!rotated.empty()) {
                    LOG_INFO << "[auth] session rotated " << cookieSid.substr(0, 8)
                             << "... -> " << rotated.substr(0, 8) << "...";
                    cookieSid = rotated;
                }
                // The client must learn the new id: the OWL frontend replays it
                // via context.session_id, so leaving the old value here would
                // log the user straight back out.
                result["session_id"] = cookieSid;

                // Cross-tenant identity (docs/072 Phase 2): if the control plane
                // knows this (tenant, login), record the global identity so the
                // company switcher can find the user's other companies.
                std::string sessionIdentity;
                if (core::ControlPlane::ready())
                    sessionIdentity = core::ControlPlane::instance()
                        .identityFor(reqTenant, result.value("login", std::string{}));
                if (sessionIdentity.empty()) sessionIdentity = result.value("login", std::string{});

                const bool updated = sessions_->update(cookieSid, [&result, &reqTenant, &sessionIdentity](Session& s) {
                    s.uid     = result["uid"].get<int>();
                    s.login   = result.value("login", std::string{});
                    // Multi-company: bind the session to the RESOLVED tenant (the
                    // database this request was actually routed to), not just the
                    // echoed arg — so Host/email routing sticks for later calls.
                    s.db      = reqTenant.empty() ? result.value("db", std::string{}) : reqTenant;
                    s.identity = sessionIdentity;
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
                sessions_->update(cookieSid, [this](Session& s) { loadAllowedCompanies_(s); });
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
        } catch (const ValidationError& e) {
            // Business-rule violations are user-actionable and author-written, so
            // they bypass the devMode gate like AccessDeniedError. Reported as 400:
            // the request was understood but is not allowed to succeed.
            return errorResponse_(id, 400, "Validation Error", e.what(),
                                  "odoo.exceptions.ValidationError");
        } catch (const ConcurrencyConflictException& e) {
            // OCC: another user saved first — return 409 with a distinguishable name
            // so the frontend can show a conflict banner instead of a generic error toast
            return errorResponse_(id, 409, "Conflict", e.what(),
                                  "odoo.exceptions.ConcurrencyConflict");
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

    // S-48: read the cookie via drogon's parsed cookie map.
    //
    // The previous implementation used
    //   SessionManager::extractFromCookie(req->getHeader("Cookie"))
    // which returns EMPTY in this Drogon version — cookies are parsed into a
    // separate map and are not served from the generic header map. The effect
    // was that call_kw ignored the session cookie entirely and resolved
    // sessions only from kwargs.context.session_id; the OWL frontend happened
    // to work because it sends exactly that. Verified by measurement, see
    // docs/042 §4.
    std::string cookieSid_(const HttpRequestPtr& req) const {
        return req->getCookie(SessionManager::cookieName());
    }

    std::string resolveOrCreateSid_(const HttpRequestPtr& req) {
        const std::string sid = cookieSid_(req);
        if (!sid.empty() && sessions_->get(sid).has_value()) return sid;
        return sessions_->create();
    }

    // Resolve session from cookie or a flat body param (for non-callKw endpoints)
    std::string resolveFromBodyOrCookie_(const HttpRequestPtr& req,
                                          const nlohmann::json& params) {
        const std::string cookieSid = cookieSid_(req);
        if (!cookieSid.empty() && sessions_->get(cookieSid).has_value())
            return cookieSid;
        const std::string bodySid = params.value("session_id", std::string{});
        if (!bodySid.empty() && sessions_->get(bodySid).has_value())
            return bodySid;
        return sessions_->create();
    }

    /**
     * @brief Resolve an EXISTING session for a call_kw request.
     *
     * S-43: this deliberately does NOT create a session when none is found.
     * It used to call sessions_->create() on every unresolved request — before
     * the auth check — so any client could allocate a stored Session per HTTP
     * request, and nothing ever reclaimed them (evictExpired() was never
     * called). Combined with S-48 that included ordinary cookie-bearing
     * traffic, not just unauthenticated spray.
     *
     * Returning "" is behaviourally identical for callers: an unresolved id
     * yielded a freshly-created ANONYMOUS session, and the caller falls back to
     * a default Session{} which is anonymous too. Only the logged sid differs.
     * A real session is created at authentication time.
     *
     * @returns the session id, or "" if there is no live session.
     */
    std::string resolveSessionId_(const HttpRequestPtr& req,
                                   const core::CallKwArgs& call) {
        // 1. Cookie (normal browser path)
        const std::string cookieSid = cookieSid_(req);
        if (!cookieSid.empty() && sessions_->get(cookieSid).has_value())
            return cookieSid;

        // 2. session_id from kwargs context (cookie-less clients / the OWL frontend)
        if (call.kwargs.contains("context")) {
            const std::string bodySid =
                call.kwargs["context"].value("session_id", std::string{});
            if (!bodySid.empty() && sessions_->get(bodySid).has_value())
                return bodySid;
        }

        return {};
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
            "part.footprint",        // parts catalogue (PK2)
            "part.unit",             // parametric units (PK3)
            "part.parameter",        // product parametric specs (PK3)
            "part.manufacturer.info",// manufacturer part numbers (PK4)
            "mail.message",          // chatter (gated by the parent document's access)
            "portal.partner",        // portal admin ViewModel (internal RPC only)
            "audit.log",
            "decimal.precision",     // P2: display precision (read by every client)             // audit trail — read-only ViewModel (admins see all)
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
            {"account.analytic.account", 5},
            {"account.analytic.line",    5},
            {"account.bank.statement",       5},
            {"account.bank.statement.line",  5},
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
            {"stock.quant",          11},
            {"stock.valuation.layer",11},
            {"stock.production.lot", 11},
            {"stock.landed.cost",    11},
            {"stock.landed.cost.line",11},
            {"stock.warehouse.orderpoint",11},
            {"stock.putaway.rule",   11},
            // Sales — SALES_USER (7)
            {"sale.order",            7},
            {"sale.order.line",       7},
            // Purchase — PURCHASE_USER (9)
            {"purchase.order",        9},
            {"purchase.order.line",   9},
            // Manufacturing — MRP_USER (13)
            {"mrp.bom",                 13},
            {"mrp.bom.line",            13},
            {"mrp.production",          13},
            {"mrp.workcenter",          13},
            {"mrp.routing.workcenter",  13},
            {"mrp.workorder",           13},
            {"mrp.production.schedule", 13},
            {"mrp.forecast",            13},
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
                                          const std::string&     detail = "",
                                          const std::string&     name   = "odoo.exceptions.UserError") {
        return {
            {"jsonrpc","2.0"}, {"id",id},
            {"error", {
                {"code",    code},
                {"message", message},
                {"data", {
                    {"name",    name},
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
    ClientIpResolver                        clientIp_;      // S-40
    bool                                    secureCookies_ = false;
    bool                                    devMode_       = false;
    std::shared_ptr<DbConnection>           db_;            // multi-tenant router (docs/072)

    // RAII: route this request's DB connections to `tenant` for the request's
    // duration, then restore. Drogon runs the handler on one worker thread, so
    // the thread-local selected tenant is scoped correctly to this request.
    struct TenantScope {
        DbConnection* db;
        explicit TenantScope(DbConnection* d, const std::string& tenant) : db(d) {
            if (db) db->setCurrentTenant(tenant);
        }
        ~TenantScope() { if (db) db->clearCurrentTenant(); }
        TenantScope(const TenantScope&) = delete;
        TenantScope& operator=(const TenantScope&) = delete;
    };

    // Resolve the tenant database for this request:
    //   authenticate → the db arg (args[0]); else Host subdomain; else default
    //   any other call → the authenticated session's db
    // Unknown/empty → the default tenant, so single-tenant deployments and
    // background work are unaffected.
    std::string resolveTenant_(const HttpRequestPtr& req,
                               const core::CallKwArgs& call,
                               const Session& session) const {
        if (!db_) return {};
        std::string t;
        if (call.method == "authenticate") {
            // args = [db, login, password] (rewritten from the authenticate body)
            if (call.args.is_array() && !call.args.empty() && call.args[0].is_string())
                t = call.args[0].get<std::string>();
            // (1) explicit db → (2) Host subdomain → (3) login email-domain
            if ((t.empty() || !db_->hasTenant(t)) && req) {
                const std::string byHost = db_->resolveHost(req->getHeader("host"));
                if (!byHost.empty()) t = byHost;
            }
            if ((t.empty() || !db_->hasTenant(t)) &&
                call.args.is_array() && call.args.size() >= 2 && call.args[1].is_string()) {
                const std::string byEmail = db_->resolveEmailDomain(call.args[1].get<std::string>());
                if (!byEmail.empty()) t = byEmail;
            }
        } else if (session.isAuthenticated()) {
            t = session.db;
        }
        if (t.empty() || !db_->hasTenant(t)) t = db_->defaultTenant();
        return t;
    }

    // PERF-D: TTL caches for quasi-static data
    TtlCache<std::string, nlohmann::json>   currencyCache_;   // 60 s TTL — res.currency rows
    TtlCache<std::string, nlohmann::json>   fieldsGetCache_;  // 300 s TTL — keyed by model name
};

} // namespace odoo::infrastructure
