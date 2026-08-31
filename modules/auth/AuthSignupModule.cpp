#include "AuthSignupModule.hpp"
#include "AuthService.hpp"
#include "DbConnection.hpp"
#include "Errors.hpp"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <openssl/rand.h>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace cerp::modules::auth {

// ================================================================
// Constructor
// ================================================================
AuthSignupModule::AuthSignupModule(core::ModelFactory&     /*modelFactory*/,
                                   core::ServiceFactory&   serviceFactory,
                                   core::ViewModelFactory& /*viewModelFactory*/,
                                   core::ViewFactory&      /*viewFactory*/)
    : db_(serviceFactory.db())
    , devMode_(serviceFactory.devMode())
{}

std::string AuthSignupModule::moduleName() const { return "auth_signup"; }
std::string AuthSignupModule::version()    const { return "19.0.1.0.0"; }
std::vector<std::string> AuthSignupModule::dependencies() const { return {"auth", "ir"}; }

void AuthSignupModule::registerModels()     {}
void AuthSignupModule::registerServices()   {}
void AuthSignupModule::registerViewModels() {}
void AuthSignupModule::registerViews()      {}

// ----------------------------------------------------------
// registerRoutes — POST /web/signup, POST /web/reset_password
// ----------------------------------------------------------
void AuthSignupModule::registerRoutes() {
    // --- POST /web/signup ---
    //
    // POLICY: self-registration is disabled. The ONLY way to create an account
    // is for an administrator to add it (res.users create, admin-gated). This
    // endpoint used to create a full internal user (share=false) for any
    // stranger who could reach it — the widest possible door on the system —
    // gated only by a config flag that defaulted to ON. It now refuses
    // unconditionally, so the policy cannot be re-opened by a stale config row.
    // The route stays registered so a stray client gets a clear 403 rather than
    // a 404 that reads like a deployment fault.
    drogon::app().registerHandler("/web/signup",
        [](const drogon::HttpRequestPtr& /*req*/,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->addHeader("Content-Type",                "application/json");
            res->addHeader("Access-Control-Allow-Origin", "*");
            res->setStatusCode(drogon::k403Forbidden);
            res->setBody(nlohmann::json{
                {"error", "Self-registration is disabled. "
                          "Ask an administrator to create your account."}}.dump());
            cb(res);
        },
        {drogon::Post});

    // --- POST /web/reset_password ---
    //
    // POLICY: there is NO self-service password reset. A user cannot request a
    // token for their own login — that automated path (submit an email, receive
    // a reset link) is exactly the flow this endpoint no longer offers. The
    // ONLY reset is one an administrator issues deliberately, out of band:
    // res.users.action_generate_reset_link mints a one-time, 24-hour token and
    // returns a link the admin sends to the user by hand.
    //
    // This route therefore does only ONE thing: COMPLETE a reset with a token
    // the admin already generated. A request with no token is refused. The
    // token is still validated, single-use and time-boxed by completeReset_().
    drogon::app().registerHandler("/web/reset_password",
        [this](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->addHeader("Content-Type",                "application/json");
            res->addHeader("Access-Control-Allow-Origin", "*");

            try {
                const auto body  = nlohmann::json::parse(req->body());
                const std::string login = body.value("login", "");

                if (login.empty()) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error", "login is required"}}.dump());
                    cb(res); return;
                }

                // A token + a new password COMPLETES an admin-issued reset.
                // Anything else is a self-service request, which is refused.
                if (!body.contains("token") || !body.contains("password")) {
                    res->setStatusCode(drogon::k403Forbidden);
                    res->setBody(nlohmann::json{
                        {"error", "Password resets are issued by an administrator. "
                                  "Please contact them for a reset link."}}.dump());
                    cb(res); return;
                }

                const std::string token    = body["token"].get<std::string>();
                const std::string password = body["password"].get<std::string>();
                if (password.empty()) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error", "password is required"}}.dump());
                    cb(res); return;
                }
                // Same floor as the portal and the admin form: a valid token is
                // not a licence to set a trivial password. Checked BEFORE the
                // token is spent, so a too-short attempt does not burn the
                // one-time token and lock the user out of their own reset.
                if (password.size() < 8) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error", "Password must be at least 8 characters"}}.dump());
                    cb(res); return;
                }
                completeReset_(login, token, password);
                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"result", {{"login", login}}}}.dump());
            } catch (const cerp::infrastructure::PoolExhaustedException& e) {
                LOG_ERROR << "[auth_signup/reset_password] pool: " << e.what();
                res->setStatusCode(drogon::k503ServiceUnavailable);
                res->setBody(nlohmann::json{
                    {"error", "The server is temporarily overloaded. Please retry."}}.dump());
            } catch (const std::exception& e) {
                LOG_ERROR << "[auth_signup/reset_password] " << e.what();
                res->setStatusCode(drogon::k500InternalServerError);
                res->setBody(nlohmann::json{
                    {"error", devMode_ ? e.what() : "Password reset failed"}}.dump());
            }
            cb(res);
        },
        {drogon::Post});

    // CORS preflight for both routes
    for (const auto& path : {"/web/signup", "/web/reset_password"}) {
        drogon::app().registerHandler(path,
            [](const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb)
            {
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::k204NoContent);
                res->addHeader("Access-Control-Allow-Origin",  "*");
                res->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
                res->addHeader("Access-Control-Allow-Headers",
                               "Content-Type, Authorization");
                cb(res);
            },
            {drogon::Options});
    }
}

// ----------------------------------------------------------
// initialize — DDL: signup columns on res_partner (idempotent)
// ----------------------------------------------------------
void AuthSignupModule::initialize() {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};

    txn.exec("ALTER TABLE res_partner "
             "ADD COLUMN IF NOT EXISTS signup_token      VARCHAR");
    txn.exec("ALTER TABLE res_partner "
             "ADD COLUMN IF NOT EXISTS signup_expiration TIMESTAMP");
    txn.commit();
}

// ----------------------------------------------------------
// createUser_ — insert partner + user row
// ----------------------------------------------------------
void AuthSignupModule::createUser_(const std::string& login,
                                   const std::string& password,
                                   const std::string& name)
{
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};

    // Check login uniqueness
    auto chk = txn.exec(
        "SELECT id FROM res_users WHERE login=$1 LIMIT 1",
        pqxx::params{login});
    if (!chk.empty())
        throw std::runtime_error("A user with that login already exists");

    // Create partner
    auto partnerRes = txn.exec(
        "INSERT INTO res_partner (name, email) VALUES ($1, $2) RETURNING id",
        pqxx::params{name, login});
    const int partnerId = partnerRes[0][0].as<int>();

    // Hash password and insert user
    const std::string hash = AuthService::hashPassword(password);
    txn.exec(
        "INSERT INTO res_users "
        "  (login, password, partner_id, company_id, lang, tz, active, share) "
        "VALUES ($1, $2, $3, 1, 'en_US', 'UTC', TRUE, FALSE)",
        pqxx::params{login, hash, partnerId});

    txn.commit();
}

// ----------------------------------------------------------
// storeResetToken_ — write token + 24h expiry on res_partner
// ----------------------------------------------------------
void AuthSignupModule::storeResetToken_(const std::string& login, const std::string& token) {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};

    auto r = txn.exec(
        "SELECT u.partner_id FROM res_users u WHERE u.login=$1 LIMIT 1",
        pqxx::params{login});
    if (r.empty())
        throw std::runtime_error("No user found for login: " + login);

    const int partnerId = r[0][0].as<int>();
    txn.exec(
        "UPDATE res_partner "
        "   SET signup_token=$1, signup_expiration=now() + INTERVAL '24 hours' "
        " WHERE id=$2",
        pqxx::params{token, partnerId});
    txn.commit();
}

// ----------------------------------------------------------
// completeReset_ — verify token and update password
// ----------------------------------------------------------
void AuthSignupModule::completeReset_(const std::string& login,
                                      const std::string& token,
                                      const std::string& newPassword)
{
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};

    // Verify token validity
    auto r = txn.exec(
        "SELECT u.id, p.signup_token, p.signup_expiration "
        "  FROM res_users u "
        "  JOIN res_partner p ON p.id = u.partner_id "
        " WHERE u.login=$1 LIMIT 1",
        pqxx::params{login});

    if (r.empty())
        throw std::runtime_error("No user found for login: " + login);

    const std::string storedToken = r[0]["signup_token"].is_null()
                                    ? "" : r[0]["signup_token"].c_str();
    if (storedToken != token || storedToken.empty())
        throw std::runtime_error("Invalid or expired reset token");

    if (!r[0]["signup_expiration"].is_null()) {
        // PostgreSQL will evaluate the expiry comparison server-side
        auto expCheck = txn.exec(
            "SELECT signup_expiration < now() "
            "  FROM res_partner p "
            "  JOIN res_users   u ON u.partner_id = p.id "
            " WHERE u.login=$1 LIMIT 1",
            pqxx::params{login});
        if (!expCheck.empty() && expCheck[0][0].as<bool>())
            throw std::runtime_error("Reset token has expired");
    }

    const int userId = r[0]["id"].as<int>();
    const std::string hash = AuthService::hashPassword(newPassword);
    txn.exec(
        "UPDATE res_users SET password=$1 WHERE id=$2",
        pqxx::params{hash, userId});

    // Clear the token after use
    txn.exec(
        "UPDATE res_partner SET signup_token=NULL, signup_expiration=NULL "
        " WHERE id=(SELECT partner_id FROM res_users WHERE id=$1)",
        pqxx::params{userId});

    txn.commit();
}

// ----------------------------------------------------------
// configBool_ — look up ir_config_parameter by key
// ----------------------------------------------------------
bool AuthSignupModule::configBool_(const std::string& key) {
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec(
            "SELECT value FROM ir_config_parameter WHERE key=$1 LIMIT 1",
            pqxx::params{key});
        if (r.empty()) return true;   // default: enabled
        const std::string val = r[0][0].c_str();
        return val == "True" || val == "true" || val == "1";
    } catch (...) {
        return true;   // if table doesn't exist yet, allow
    }
}

// ----------------------------------------------------------
// generateToken_ — cryptographically random hex string
// ----------------------------------------------------------
std::string AuthSignupModule::generateToken_() {
    unsigned char buf[24];
    // Use rand as fallback if OpenSSL unavailable in test env
#ifdef OPENSSL_VERSION_NUMBER
    RAND_bytes(buf, sizeof(buf));
#else
    for (auto& b : buf) b = static_cast<unsigned char>(rand() % 256);
#endif
    std::ostringstream oss;
    for (unsigned char b : buf)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(b);
    return oss.str();
}

} // namespace cerp::modules::auth
