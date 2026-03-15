#pragma once
#include "BaseModel.hpp"
#include "FieldRegistry.hpp"

namespace odoo::modules::auth {

// ================================================================
// ResCompany — res.company
// ================================================================
/**
 * Minimal company model required by res.users context fields.
 * Odoo 19 adds branch support via parent_id/child_ids.
 */
class ResCompany : public core::BaseModel<ResCompany> {
public:
    ODOO_MODEL("res.company", "res_company")

    std::string name;
    std::string email;
    std::string phone;
    std::string website;
    std::string vat;
    int         parentId   = 0;
    int         partnerId  = 0;   // res_partner.id backing this company
    int         currencyId = 0;   // res_currency.id

    explicit ResCompany(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResCompany>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        core::FieldType::Char,    "Company Name", true});
        fieldRegistry_.add({"email",       core::FieldType::Char,    "Email"});
        fieldRegistry_.add({"phone",       core::FieldType::Char,    "Phone"});
        fieldRegistry_.add({"website",     core::FieldType::Char,    "Website"});
        fieldRegistry_.add({"vat",         core::FieldType::Char,    "Tax ID"});
        fieldRegistry_.add({"parent_id",   core::FieldType::Many2one,"Parent Company",
                             false, false, true, false, "res.company"});
        fieldRegistry_.add({"partner_id",  core::FieldType::Many2one,"Partner",
                             false, false, true, false, "res.partner"});
        fieldRegistry_.add({"currency_id", core::FieldType::Many2one,"Currency",
                             false, false, true, false, "res.currency"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]        = name;
        j["email"]       = email;
        j["phone"]       = phone;
        j["website"]     = website;
        j["vat"]         = vat;
        j["parent_id"]   = parentId   > 0
                           ? nlohmann::json{parentId,   name}
                           : nlohmann::json(false);
        j["partner_id"]  = partnerId  > 0
                           ? nlohmann::json{partnerId,  name}
                           : nlohmann::json(false);
        j["currency_id"] = currencyId > 0
                           ? nlohmann::json{currencyId, "Currency"}
                           : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")        && j["name"].is_string())
            name       = j["name"].get<std::string>();
        if (j.contains("email")       && j["email"].is_string())
            email      = j["email"].get<std::string>();
        if (j.contains("phone")       && j["phone"].is_string())
            phone      = j["phone"].get<std::string>();
        if (j.contains("website")     && j["website"].is_string())
            website    = j["website"].get<std::string>();
        if (j.contains("vat")         && j["vat"].is_string())
            vat        = j["vat"].get<std::string>();
        if (j.contains("parent_id")   && j["parent_id"].is_number_integer())
            parentId   = j["parent_id"].get<int>();
        if (j.contains("partner_id")  && j["partner_id"].is_number_integer())
            partnerId  = j["partner_id"].get<int>();
        if (j.contains("currency_id") && j["currency_id"].is_number_integer())
            currencyId = j["currency_id"].get<int>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Company name is required");
        return errors;
    }
};

} // namespace odoo::modules::auth
