#pragma once
#include "BaseModel.hpp"
#include "FieldRegistry.hpp"

namespace cerp::modules::auth {

// ================================================================
// ResCompany — res.company
// ================================================================
/**
 * Minimal company model required by res.users context fields.
 * the reference ERP adds branch support via parent_id/child_ids.
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

    // Letterhead and bank details (docs/094). These columns are what every
    // printed document reads through CompanyIdentity; registering them is what
    // lets Settings edit them. Before this, Settings wrote ir_config_parameter
    // rows that nothing read and that startup deleted.
    std::string regNumber;
    std::string street, street2, street3, cityCountry;
    std::string bankName, bankAccountName, bankAccountNo, bankAddress, bankSwift;
    int         paymentTermDays = 30;

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
        fieldRegistry_.add({"reg_number",        core::FieldType::Char,    "Registration No."});
        fieldRegistry_.add({"street",            core::FieldType::Char,    "Address Line 1"});
        fieldRegistry_.add({"street2",           core::FieldType::Char,    "Address Line 2"});
        fieldRegistry_.add({"street3",           core::FieldType::Char,    "Address Line 3"});
        fieldRegistry_.add({"city_country",      core::FieldType::Char,    "City & Country"});
        fieldRegistry_.add({"bank_name",         core::FieldType::Char,    "Bank Name"});
        fieldRegistry_.add({"bank_account_name", core::FieldType::Char,    "Account Holder Name"});
        fieldRegistry_.add({"bank_account_no",   core::FieldType::Char,    "Account Number"});
        fieldRegistry_.add({"bank_address",      core::FieldType::Char,    "Bank Address"});
        fieldRegistry_.add({"bank_swift",        core::FieldType::Char,    "SWIFT Code"});
        fieldRegistry_.add({"payment_term_days", core::FieldType::Integer, "Payment Terms Days"});
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
        j["reg_number"]        = regNumber;
        j["street"]            = street;
        j["street2"]           = street2;
        j["street3"]           = street3;
        j["city_country"]      = cityCountry;
        j["bank_name"]         = bankName;
        j["bank_account_name"] = bankAccountName;
        j["bank_account_no"]   = bankAccountNo;
        j["bank_address"]      = bankAddress;
        j["bank_swift"]        = bankSwift;
        j["payment_term_days"] = paymentTermDays;
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
        auto str = [&](const char* k, std::string& out) {
            if (j.contains(k) && j[k].is_string()) out = j[k].get<std::string>();
        };
        str("reg_number",        regNumber);
        str("street",            street);
        str("street2",           street2);
        str("street3",           street3);
        str("city_country",      cityCountry);
        str("bank_name",         bankName);
        str("bank_account_name", bankAccountName);
        str("bank_account_no",   bankAccountNo);
        str("bank_address",      bankAddress);
        str("bank_swift",        bankSwift);
        if (j.contains("payment_term_days") && j["payment_term_days"].is_number_integer())
            paymentTermDays = j["payment_term_days"].get<int>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Company name is required");
        return errors;
    }
};

} // namespace cerp::modules::auth
