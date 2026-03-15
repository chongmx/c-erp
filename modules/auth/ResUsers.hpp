#pragma once
#include "BaseModel.hpp"
#include "FieldRegistry.hpp"

namespace odoo::modules::auth {

// ================================================================
// ResUsers — res.users
// ================================================================
/**
 * User accounts. Each user has an associated res.partner record.
 *
 * Password storage (Odoo 19 / Django PBKDF2 format):
 *   $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
 *
 * The raw password is never stored — only the hash.
 * AuthService::authenticate() verifies via PBKDF2-SHA512 (OpenSSL).
 */
class ResUsers : public core::BaseModel<ResUsers> {
public:
    ODOO_MODEL("res.users", "res_users")

    std::string login;
    std::string password;       ///< PBKDF2 hash, never plaintext
    int         partnerId  = 0;
    int         companyId  = 0;
    std::string lang       = "en_US";
    std::string tz         = "UTC";
    bool        active     = true;
    bool        share      = false; ///< portal/external user

    explicit ResUsers(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResUsers>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({ "login",      core::FieldType::Char,    "Login",      true });
        // password is excluded from normal field listing — never sent to client
        fieldRegistry_.add({ "partner_id", core::FieldType::Many2one,"Partner",
                             true, false, true, false, "res.partner" });
        fieldRegistry_.add({ "company_id", core::FieldType::Many2one,"Company",
                             true, false, true, false, "res.company" });
        fieldRegistry_.add({ "lang",       core::FieldType::Char,    "Language" });
        fieldRegistry_.add({ "tz",         core::FieldType::Char,    "Timezone" });
        fieldRegistry_.add({ "active",     core::FieldType::Boolean, "Active" });
        fieldRegistry_.add({ "share",      core::FieldType::Boolean, "Share User" });
    }

    void serializeFields(nlohmann::json& j) const override {
        j["login"]      = login;
        // password intentionally omitted
        j["partner_id"] = partnerId > 0
                          ? nlohmann::json{partnerId, login}
                          : nlohmann::json(false);
        j["company_id"] = companyId > 0
                          ? nlohmann::json{companyId, "Company"}
                          : nlohmann::json(false);
        j["lang"]       = lang;
        j["tz"]         = tz;
        j["active"]     = active;
        j["share"]      = share;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("login")      && j["login"].is_string())
            login      = j["login"].get<std::string>();
        if (j.contains("password")   && j["password"].is_string())
            password   = j["password"].get<std::string>();
        if (j.contains("partner_id") && j["partner_id"].is_number_integer())
            partnerId  = j["partner_id"].get<int>();
        if (j.contains("company_id") && j["company_id"].is_number_integer())
            companyId  = j["company_id"].get<int>();
        if (j.contains("lang")       && j["lang"].is_string())
            lang       = j["lang"].get<std::string>();
        if (j.contains("tz")         && j["tz"].is_string())
            tz         = j["tz"].get<std::string>();
        if (j.contains("active")     && j["active"].is_boolean())
            active     = j["active"].get<bool>();
        if (j.contains("share")      && j["share"].is_boolean())
            share      = j["share"].get<bool>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (login.empty())    errors.push_back("Login is required");
        if (password.empty()) errors.push_back("Password is required");
        return errors;
    }
};

} // namespace odoo::modules::auth
