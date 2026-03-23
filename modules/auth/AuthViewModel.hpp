#pragma once
#include "BaseViewModel.hpp"
#include "AuthService.hpp"
#include "SessionManager.hpp"
#include "Errors.hpp"
#include "Groups.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace odoo::modules::auth {

using odoo::infrastructure::AccessDeniedError;

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
        REGISTER_METHOD("create",           handleCreate)
        REGISTER_METHOD("write",            handleWrite)
        REGISTER_METHOD("unlink",           handleUnlink)
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
    // Authorization helper — resolve the caller's session from context
    // ----------------------------------------------------------
    infrastructure::Session callerSession_(const core::CallKwArgs& call) const {
        const std::string sid = call.kwargs.contains("context")
            ? call.kwargs["context"].value("session_id", std::string{})
            : std::string{};
        if (!sid.empty()) {
            auto s = sessions_->get(sid);
            if (s) return *s;
        }
        return infrastructure::Session{};
    }

    void requireAdmin_(const core::CallKwArgs& call) const {
        const auto s = callerSession_(call);
        if (!s.isAdmin && !s.hasGroup(Groups::SETTINGS_CONFIGURATION))
            throw AccessDeniedError(
                "Access denied: user management requires Administrator or "
                "Settings / Configuration group");
    }

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

        // Enrich session with display name, partner, company, admin flag, and group IDs
        {
            auto conn = db_->acquire();
            pqxx::work etxn{conn.get()};

            // User details + admin flag
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

            // All group IDs for this user
            auto gRows = etxn.exec(
                "SELECT gid FROM res_groups_users_rel WHERE uid = $1 ORDER BY gid",
                pqxx::params{session.uid});
            session.groupIds.clear();
            for (const auto& gr : gRows)
                session.groupIds.push_back(gr["gid"].as<int>());
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
            s.groupIds    = session.groupIds;
        });

        // Build group_ids JSON array for the response
        nlohmann::json groupArr = nlohmann::json::array();
        for (int g : session.groupIds) groupArr.push_back(g);

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
            {"group_ids",    groupArr},
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
    // create — new user; direct INSERT bypasses BaseModel field registry
    // (password is intentionally excluded from ResUsers fieldRegistry_ to
    //  prevent it being sent to the client — so we must INSERT it manually)
    // ----------------------------------------------------------
    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        requireAdmin_(call);
        auto vals = call.arg(0).is_object() ? call.arg(0) : nlohmann::json::object();
        const std::string pw    = vals.value("password", std::string{});
        const std::string login = vals.value("login", std::string{});
        if (pw.empty())    throw std::runtime_error("password is required");
        if (login.empty()) throw std::runtime_error("login is required");

        nlohmann::json groupsCmds;
        if (vals.contains("groups_id")) groupsCmds = vals["groups_id"];

        // Resolve nullable FK values (false / 0 / [id,"Name"] → int or NULL)
        auto extractFk = [](const nlohmann::json& v) -> int {
            if (v.is_number_integer() && v.get<int>() > 0) return v.get<int>();
            if (v.is_array() && !v.empty() && v[0].is_number_integer() && v[0].get<int>() > 0)
                return v[0].get<int>();
            return 0;
        };
        const int  partnerId = vals.contains("partner_id") ? extractFk(vals["partner_id"]) : 0;
        const int  companyId = vals.contains("company_id") ? extractFk(vals["company_id"]) : 0;
        const bool active    = vals.value("active", true);
        const bool share     = vals.value("share",  false);
        const std::string hash = AuthService::hashPassword(pw);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Build params — append nullptr for 0 FK values (NULL in DB)
        pqxx::params p;
        p.append(login);
        p.append(hash);
        if (partnerId > 0) p.append(partnerId); else p.append(nullptr);
        if (companyId > 0) p.append(companyId); else p.append(nullptr);
        p.append(active);
        p.append(share);

        auto rows = txn.exec(
            "INSERT INTO res_users "
            "  (login, password, partner_id, company_id, active, share) "
            "VALUES ($1,$2,$3,$4,$5,$6) RETURNING id",
            p);
        const int newId = rows[0][0].as<int>();

        // Apply group assignments
        if (!groupsCmds.is_null() && groupsCmds.is_array()) {
            for (const auto& cmd : groupsCmds) {
                if (!cmd.is_array() || cmd.empty()) continue;
                const int op = cmd[0].get<int>();
                if (op == 6 && cmd.size() >= 3 && cmd[2].is_array()) {
                    for (const auto& gid : cmd[2])
                        txn.exec(
                            "INSERT INTO res_groups_users_rel (gid,uid) "
                            "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                            pqxx::params{gid.get<int>(), newId});
                } else if (op == 4 && cmd.size() >= 2) {
                    txn.exec(
                        "INSERT INTO res_groups_users_rel (gid,uid) "
                        "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                        pqxx::params{cmd[1].get<int>(), newId});
                }
            }
        }
        txn.commit();
        return newId;
    }

    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        const auto session = callerSession_(call);
        const auto ids = call.ids();
        if (!session.isAdmin) {
            // Non-admin: can only write their own record and cannot modify groups or password
            if (ids.size() != 1 || ids[0] != session.uid)
                throw AccessDeniedError("Access denied: cannot modify other users");
            const auto& v = call.arg(1);
            if (v.is_object() && v.contains("groups_id"))
                throw AccessDeniedError("Access denied: cannot modify group memberships");
            if (v.is_object() && v.contains("password"))
                throw AccessDeniedError("Access denied: use change_password to update your password");
        }
        auto vals = call.arg(1).is_object() ? call.arg(1) : nlohmann::json::object();

        // Handle password directly — BaseModel::write() skips it because
        // password is excluded from ResUsers::registerFields() for security
        if (vals.contains("password") && vals["password"].is_string()) {
            const std::string pw = vals["password"].get<std::string>();
            if (!pw.empty()) {
                const std::string hash = (pw.front() == '$')
                    ? pw : AuthService::hashPassword(pw);
                auto conn = db_->acquire();
                pqxx::work txn{conn.get()};
                for (int uid : call.ids())
                    txn.exec(
                        "UPDATE res_users SET password=$2, write_date=now() WHERE id=$1",
                        pqxx::params{uid, hash});
                txn.commit();
            }
            vals.erase("password");
        }

        // Handle groups_id many2many commands
        if (vals.contains("groups_id") && vals["groups_id"].is_array()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (int uid : call.ids()) {
                for (const auto& cmd : vals["groups_id"]) {
                    if (!cmd.is_array() || cmd.empty()) continue;
                    const int op = cmd[0].get<int>();
                    if (op == 6) {
                        txn.exec(
                            "DELETE FROM res_groups_users_rel WHERE uid=$1",
                            pqxx::params{uid});
                        if (cmd.size() >= 3 && cmd[2].is_array()) {
                            for (const auto& gid : cmd[2])
                                txn.exec(
                                    "INSERT INTO res_groups_users_rel (gid,uid) "
                                    "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                                    pqxx::params{gid.get<int>(), uid});
                        }
                    } else if (op == 4 && cmd.size() >= 2) {
                        txn.exec(
                            "INSERT INTO res_groups_users_rel (gid,uid) "
                            "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                            pqxx::params{cmd[1].get<int>(), uid});
                    } else if (op == 3 && cmd.size() >= 2) {
                        txn.exec(
                            "DELETE FROM res_groups_users_rel WHERE gid=$1 AND uid=$2",
                            pqxx::params{cmd[1].get<int>(), uid});
                    }
                }
            }
            txn.commit();
            vals.erase("groups_id");
        }

        // Write remaining scalar fields via BaseModel
        vals.erase("id");
        if (!vals.empty()) {
            ResUsers proto(db_);
            proto.write(call.ids(), vals);
        }
        return true;
    }

    nlohmann::json handleUnlink(const core::CallKwArgs& call) {
        const auto session = callerSession_(call);
        if (!session.isAdmin && !session.hasGroup(Groups::SETTINGS_CONFIGURATION))
            throw AccessDeniedError(
                "Access denied: user management requires Administrator or "
                "Settings / Configuration group");
        for (int id : call.ids())
            if (id == session.uid)
                throw AccessDeniedError("Cannot delete your own user account");
        ResUsers proto(db_);
        return proto.unlink(call.ids());
    }

    // ----------------------------------------------------------
    // search_read — list of users with optional groups_id
    // ----------------------------------------------------------
    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        requireAdmin_(call);
        ResUsers proto(db_);
        const auto fields = call.fields();
        auto result = proto.searchRead(call.domain(), fields,
                                       call.limit() > 0 ? call.limit() : 80,
                                       call.offset(), "id ASC");

        const bool wantGroups = fields.empty() ||
            std::find(fields.begin(), fields.end(), "groups_id") != fields.end();
        if (wantGroups && !result.empty()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (auto& rec : result) {
                const int uid = rec.value("id", 0);
                auto gRows = txn.exec(
                    "SELECT gid FROM res_groups_users_rel WHERE uid=$1 ORDER BY gid",
                    pqxx::params{uid});
                nlohmann::json gArr = nlohmann::json::array();
                for (const auto& gr : gRows) gArr.push_back(gr["gid"].as<int>());
                rec["groups_id"] = gArr;
            }
        }
        return result;
    }

    // ----------------------------------------------------------
    // read — return fields for given ids (falls back to current uid)
    // ----------------------------------------------------------
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        std::vector<int> ids = call.ids();
        if (ids.empty()) {
            const int uid = call.kwargs
                                .value("context", nlohmann::json::object())
                                .value("uid", 0);
            if (uid <= 0) throw std::runtime_error("Not authenticated");
            ids = {uid};
        }

        ResUsers proto(db_);
        const auto fields = call.fields();
        auto result = proto.read(ids, fields);

        // Append groups_id to each record if requested or no specific fields asked
        const bool wantGroups = fields.empty() ||
            std::find(fields.begin(), fields.end(), "groups_id") != fields.end();
        if (wantGroups) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (auto& rec : result) {
                const int uid = rec.value("id", 0);
                auto gRows = txn.exec(
                    "SELECT gid FROM res_groups_users_rel WHERE uid=$1 ORDER BY gid",
                    pqxx::params{uid});
                nlohmann::json gArr = nlohmann::json::array();
                for (const auto& gr : gRows) gArr.push_back(gr["gid"].as<int>());
                rec["groups_id"] = gArr;
            }
        }
        return result;
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
