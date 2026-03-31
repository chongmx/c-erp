#include "AuthSignupModule.hpp"
#include "AuthService.hpp"
#include "DbConnection.hpp"
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

namespace odoo::modules::auth {

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
    drogon::app().registerHandler("/web/signup",
        [this](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->addHeader("Content-Type",                "application/json");
            res->addHeader("Access-Control-Allow-Origin", "*");

            try {
                if (!configBool_("auth_signup.allow")) {
                    res->setStatusCode(drogon::k403Forbidden);
                    res->setBody(nlohmann::json{
                        {"error", "Self-registration is disabled"}}.dump());
                    cb(res); return;
                }

                const auto body = nlohmann::json::parse(req->body());
                const std::string login    = body.value("login",    "");
                const std::string password = body.value("password", "");
                const std::string name     = body.value("name",     login);

                if (login.empty() || password.empty()) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error", "login and password are required"}}.dump());
                    cb(res); return;
                }

                createUser_(login, password, name);

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"result", {{"login", login}}}}.dump());
            } catch (const std::exception& e) {
                LOG_ERROR << "[auth_signup/signup] " << e.what();
                res->setStatusCode(drogon::k500InternalServerError);
                res->setBody(nlohmann::json{
                    {"error", devMode_ ? e.what() : "Registration failed"}}.dump());
            }
            cb(res);
        },
        {drogon::Post});

    // --- POST /web/reset_password ---
    drogon::app().registerHandler("/web/reset_password",
        [this](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->addHeader("Content-Type",                "application/json");
            res->addHeader("Access-Control-Allow-Origin", "*");

            try {
                if (!configBool_("auth_signup.reset_pwd")) {
                    res->setStatusCode(drogon::k403Forbidden);
                    res->setBody(nlohmann::json{
                        {"error", "Password reset is disabled"}}.dump());
                    cb(res); return;
                }

                const auto body  = nlohmann::json::parse(req->body());
                const std::string login = body.value("login", "");

                if (login.empty()) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{
                        {"error", "login is required"}}.dump());
                    cb(res); return;
                }

                // If "token" + "password" are provided, complete the reset
                if (body.contains("token") && body.contains("password")) {
                    const std::string token    = body["token"].get<std::string>();
                    const std::string password = body["password"].get<std::string>();
                    if (password.empty()) {
                        res->setStatusCode(drogon::k400BadRequest);
                        res->setBody(nlohmann::json{
                            {"error", "password is required"}}.dump());
                        cb(res); return;
                    }
                    completeReset_(login, token, password);
                    res->setStatusCode(drogon::k200OK);
                    res->setBody(nlohmann::json{
                        {"result", {{"login", login}}}}.dump());
                    cb(res); return;
                }

                // Otherwise, generate a token and return it (email stub)
                const std::string token = generateToken_();
                storeResetToken_(login, token);

                res->setStatusCode(drogon::k200OK);
                // In production, the token would be emailed.
                // We return it here for testing/frontend development.
                res->setBody(nlohmann::json{
                    {"result", {{"token", token}, {"login", login}}}}.dump());
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

} // namespace odoo::modules::auth
