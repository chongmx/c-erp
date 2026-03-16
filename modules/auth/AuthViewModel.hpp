#pragma once
#include "BaseViewModel.hpp"
#include "AuthService.hpp"
#include "SessionManager.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace odoo::modules::auth {

// ================================================================
// AuthViewModel
// ================================================================
/**
 * @brief Handles authentication JSON-RPC methods for model "res.users".
 *
 * Mapped methods:
 *   authenticate      — login + password → session
 *   logout            — clear session
 *   read              — return current user fields
 *   fields_get        — introspect res.users fields
 *   change_password   — update password (requires current password)
 *
 * The "authenticate" method is on the public bypass list in
 * JsonRpcDispatcher, so it reaches here even without a valid session.
 *
 * Odoo 19 authenticate call format:
 * @code
 * {
 *   "model": "res.users",
 *   "method": "authenticate",
 *   "args": ["odoo", "admin", "password", {}],
 *   "kwargs": {}
 * }
 * @endcode
 * args[0] = db, args[1] = login, args[2] = password, args[3] = user_agent_env
 */
class AuthViewModel : public core::BaseViewModel {
public:
    explicit AuthViewModel(
        std::shared_ptr<AuthService>              authService,
        std::shared_ptr<infrastructure::SessionManager> sessions,
        std::shared_ptr<infrastructure::DbConnection>    db,
        std::string                               dbName)
        : auth_    (std::move(authService))
        , sessions_(std::move(sessions))
        , db_      (std::move(db))
        , dbName_  (std::move(dbName))
    {
        REGISTER_METHOD("authenticate",     handleAuthenticate)
        REGISTER_METHOD("logout",           handleLogout)
        REGISTER_METHOD("search_read",      handleSearchRead)
        REGISTER_METHOD("web_search_read",  handleSearchRead)
        REGISTER_METHOD("read",             handleRead)
        REGISTER_METHOD("web_read",         handleRead)
        REGISTER_METHOD("fields_get",       handleFieldsGet)
        REGISTER_METHOD("change_password",  handleChangePassword)
    }

    std::string modelName() const override { return "res.users"; }

private:
    std::shared_ptr<AuthService>                    auth_;
    std::shared_ptr<infrastructure::SessionManager> sessions_;
    std::shared_ptr<infrastructure::DbConnection>   db_;
    std::string                                     dbName_;

    // ----------------------------------------------------------
    // authenticate — args[0]=db, args[1]=login, args[2]=password
    // ----------------------------------------------------------
    nlohmann::json handleAuthenticate(const core::CallKwArgs& call) {
        // Support both Odoo's positional format and kwargs format
        std::string db       = call.arg(0).is_string()
                               ? call.arg(0).get<std::string>() : dbName_;
        std::string login    = call.arg(1).is_string()
                               ? call.arg(1).get<std::string>()
                               : call.kwargs.value("login", std::string{});
        std::string password = call.arg(2).is_string()
                               ? call.arg(2).get<std::string>()
                               : call.kwargs.value("password", std::string{});

        if (login.empty() || password.empty())
            throw std::runtime_error("Login and password are required");

        // The session_id comes from the context injected by JsonRpcDispatcher
        const std::string sid = call.kwargs.value("context",
                                    nlohmann::json::object())
                                    .value("session_id", std::string{});

        // Create a fresh anonymous session if none exists yet
        const std::string activeSid = sid.empty() ? sessions_->create(db) : sid;

        auto sessionOpt = sessions_->get(activeSid);
        if (!sessionOpt) {
            // Session expired between request and here — create fresh
            const std::string newSid = sessions_->create(db);
            sessionOpt = sessions_->get(newSid);
        }

        infrastructure::Session session = *sessionOpt;

        const bool ok = auth_->authenticate(login, password, db, session);
        if (!ok)
            throw std::runtime_error("Invalid credentials");

        // Enrich session with display name, partner, company, and admin flag
        {
            auto conn = db_->acquire();
            pqxx::work etxn{conn.get()};
            auto rows = etxn.exec(
                "SELECT u.partner_id, u.company_id, "
                "       COALESCE(p.name, u.login) AS uname, "
                "       COALESCE(c.name, '')      AS cname, "
                "       EXISTS(SELECT 1 FROM res_groups_users_rel r "
                "              WHERE r.uid = u.id AND r.gid = 3) AS is_admin "
                "FROM res_users u "
                "LEFT JOIN res_partner p ON p.id = u.partner_id "
                "LEFT JOIN res_company c ON c.id = u.company_id "
                "WHERE u.id = $1",
                pqxx::params{session.uid});

            if (!rows.empty()) {
                const auto& r = rows[0];
                session.partnerId   = r["partner_id"].is_null() ? 0 : r["partner_id"].as<int>();
                session.companyId   = r["company_id"].is_null() ? 0 : r["company_id"].as<int>();
                session.name        = r["uname"].c_str();
                session.companyName = r["cname"].c_str();
                session.isAdmin     = std::string(r["is_admin"].c_str()) == "t";
            }
        }

        // Write the populated session back
        sessions_->update(activeSid, [&](infrastructure::Session& s) {
            s.uid         = session.uid;
            s.login       = session.login;
            s.db          = session.db;
            s.context     = session.context;
            s.name        = session.name;
            s.partnerId   = session.partnerId;
            s.companyId   = session.companyId;
            s.companyName = session.companyName;
            s.isAdmin     = session.isAdmin;
        });

        return {
            {"uid",          session.uid},
            {"login",        session.login},
            {"session_id",   activeSid},
            {"db",           db},
            {"name",         session.name},
            {"partner_id",   session.partnerId > 0
                             ? nlohmann::json(session.partnerId)
                             : nlohmann::json(false)},
            {"company_id",   session.companyId > 0
                             ? nlohmann::json(session.companyId)
                             : nlohmann::json(false)},
            {"is_admin",     session.isAdmin},
            {"context",      session.context},
        };
    }

    // ----------------------------------------------------------
    // logout — clear session authentication state
    // ----------------------------------------------------------
    nlohmann::json handleLogout(const core::CallKwArgs& call) {
        const std::string sid = call.kwargs
                                    .value("context", nlohmann::json::object())
                                    .value("session_id", std::string{});
        if (!sid.empty()) {
            sessions_->update(sid, [](infrastructure::Session& s) {
                s.uid   = 0;
                s.login = "";
                s.context = nlohmann::json::object();
            });
        }
        return true;
    }

    // ----------------------------------------------------------
    // search_read — list of users (admin-visible fields only)
    // ----------------------------------------------------------
    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        ResUsers proto(db_);
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }

    // ----------------------------------------------------------
    // read — return fields for the current user (uid in context)
    // ----------------------------------------------------------
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        const int uid = call.kwargs
                            .value("context", nlohmann::json::object())
                            .value("uid", 0);
        if (uid <= 0) throw std::runtime_error("Not authenticated");

        ResUsers proto(db_);
        const auto fields = call.fields();
        return proto.read({uid}, fields);
    }

    // ----------------------------------------------------------
    // fields_get
    // ----------------------------------------------------------
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        ResUsers proto(db_);
        const auto attrs = call.kwargs.contains("attributes") &&
                           call.kwargs["attributes"].is_array()
                           ? call.kwargs["attributes"].get<std::vector<std::string>>()
                           : std::vector<std::string>{};
        return proto.fieldsGet(call.fields(), attrs);
    }

    // ----------------------------------------------------------
    // change_password — args[0]=old_passwd, args[1]=new_passwd
    // ----------------------------------------------------------
    nlohmann::json handleChangePassword(const core::CallKwArgs& call) {
        const int uid = call.kwargs
                            .value("context", nlohmann::json::object())
                            .value("uid", 0);
        if (uid <= 0) throw std::runtime_error("Not authenticated");

        const std::string oldPw = call.arg(0).is_string()
                                  ? call.arg(0).get<std::string>() : "";
        const std::string newPw = call.arg(1).is_string()
                                  ? call.arg(1).get<std::string>() : "";

        if (oldPw.empty() || newPw.empty())
            throw std::runtime_error("Old and new passwords are required");
        if (newPw.size() < 8)
            throw std::runtime_error("New password must be at least 8 characters");

        // Verify old password
        ResUsers proto(db_);
        auto rows = proto.read({uid}, {"login", "password"});
        if (rows.empty()) throw std::runtime_error("User not found");

        const std::string login  = rows[0].value("login", std::string{});
        infrastructure::Session dummy;
        if (!auth_->authenticate(login, oldPw, dbName_, dummy))
            throw std::runtime_error("Current password is incorrect");

        // Hash and store new password
        const std::string hash = AuthService::hashPassword(newPw);
        proto.write({uid}, {{"password", hash}});

        return true;
    }
};

} // namespace odoo::modules::auth
