// =============================================================
// modules/base/BaseModule.cpp
// =============================================================
#include "BaseModule.hpp"
#include "PartnerMigrations.hpp"
#include "BaseModel.hpp"
#include "DecimalPrecision.hpp"
#include "BaseService.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "Errors.hpp"
#include "CacheInvalidation.hpp"
#include "GenericViewModel.hpp"
#include "Domain.hpp"
#include "FieldRegistry.hpp"
#include "WorldData.hpp"
#include "MoneyMigrations.hpp"
#include "MigrationRunner.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::base {

// ================================================================
// 1. MODELS
// ================================================================

// ----------------------------------------------------------------
// ResLang — res.lang
// ----------------------------------------------------------------
class ResLang : public core::BaseModel<ResLang> {
public:
    ODOO_MODEL("res.lang", "res_lang")

    std::string name, code, isoCode, urlCode, direction;
    std::string dateFormat = "%m/%d/%Y";
    std::string timeFormat = "%H:%M:%S";
    bool        active     = true;

    explicit ResLang(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResLang>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        core::FieldType::Char,    "Language Name", true});
        fieldRegistry_.add({"code",        core::FieldType::Char,    "Locale Code",   true});
        fieldRegistry_.add({"iso_code",    core::FieldType::Char,    "ISO Code"});
        fieldRegistry_.add({"url_code",    core::FieldType::Char,    "URL Code",      true});
        fieldRegistry_.add({"active",      core::FieldType::Boolean, "Active"});
        fieldRegistry_.add({"direction",   core::FieldType::Char,    "Direction"});
        fieldRegistry_.add({"date_format", core::FieldType::Char,    "Date Format"});
        fieldRegistry_.add({"time_format", core::FieldType::Char,    "Time Format"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]        = name;
        j["code"]        = code;
        j["iso_code"]    = isoCode;
        j["url_code"]    = urlCode;
        j["active"]      = active;
        j["direction"]   = direction;
        j["date_format"] = dateFormat;
        j["time_format"] = timeFormat;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")        && j["name"].is_string())
            name        = j["name"].get<std::string>();
        if (j.contains("code")        && j["code"].is_string())
            code        = j["code"].get<std::string>();
        if (j.contains("iso_code")    && j["iso_code"].is_string())
            isoCode     = j["iso_code"].get<std::string>();
        if (j.contains("url_code")    && j["url_code"].is_string())
            urlCode     = j["url_code"].get<std::string>();
        if (j.contains("active")      && j["active"].is_boolean())
            active      = j["active"].get<bool>();
        if (j.contains("direction")   && j["direction"].is_string())
            direction   = j["direction"].get<std::string>();
        if (j.contains("date_format") && j["date_format"].is_string())
            dateFormat  = j["date_format"].get<std::string>();
        if (j.contains("time_format") && j["time_format"].is_string())
            timeFormat  = j["time_format"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("name is required");
        if (code.empty()) e.push_back("code is required");
        return e;
    }
};

// ----------------------------------------------------------------
// ResCurrency — res.currency
// ----------------------------------------------------------------
class ResCurrency : public core::BaseModel<ResCurrency> {
public:
    ODOO_MODEL("res.currency", "res_currency")

    std::string name, symbol;
    std::string position      = "after";
    double      rounding      = 0.01;
    int         decimalPlaces = 2;
    // P2: base units per 1 unit of this currency, micro-units (docs/048 4.3).
    // Scaled, so BaseModel converts it at both boundaries like any money column.
    double      rate          = 1.0;
    bool        active        = true;

    explicit ResCurrency(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResCurrency>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",           core::FieldType::Char,    "Currency",  true});
        fieldRegistry_.add({"symbol",         core::FieldType::Char,    "Symbol",    true});
        fieldRegistry_.add({"position",       core::FieldType::Char,    "Position"});
        fieldRegistry_.add({"rounding",       core::FieldType::Float,   "Rounding"});
        fieldRegistry_.add({"decimal_places", core::FieldType::Integer, "Decimals"});
        fieldRegistry_.add({"active",         core::FieldType::Boolean, "Active"});
        fieldRegistry_.add({"rate",           core::FieldType::Float,   "Rate"});
        fieldRegistry_.markScaled({"rate"});          // P2: migration 901
        fieldRegistry_.setPrecision(core::DecimalPrecision::kProductPrice, {"rate"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]           = name;
        j["symbol"]         = symbol;
        j["position"]       = position;
        j["rounding"]       = rounding;
        j["decimal_places"] = decimalPlaces;
        j["active"]         = active;
        j["rate"]           = rate;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())
            name           = j["name"].get<std::string>();
        if (j.contains("symbol")         && j["symbol"].is_string())
            symbol         = j["symbol"].get<std::string>();
        if (j.contains("position")       && j["position"].is_string())
            position       = j["position"].get<std::string>();
        if (j.contains("rounding")       && j["rounding"].is_number())
            rounding       = j["rounding"].get<double>();
        if (j.contains("decimal_places") && j["decimal_places"].is_number_integer())
            decimalPlaces  = j["decimal_places"].get<int>();
        if (j.contains("active")         && j["active"].is_boolean())
            active         = j["active"].get<bool>();
        if (j.contains("rate")           && j["rate"].is_number())
            rate           = j["rate"].get<double>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())   e.push_back("name is required");
        if (symbol.empty()) e.push_back("symbol is required");
        return e;
    }
};

// ----------------------------------------------------------------
// ResCountry — res.country
// ----------------------------------------------------------------
class ResCountry : public core::BaseModel<ResCountry> {
public:
    ODOO_MODEL("res.country", "res_country")

    std::string name, code;
    int         currencyId = 0;
    int         phoneCode  = 0;
    bool        active     = true;

    explicit ResCountry(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResCountry>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        core::FieldType::Char,    "Country",  true});
        fieldRegistry_.add({"code",        core::FieldType::Char,    "ISO Code", true});
        fieldRegistry_.add({"currency_id", core::FieldType::Many2one,"Currency",
                             false, false, true, false, "res.currency"});
        fieldRegistry_.add({"phone_code",  core::FieldType::Integer, "Phone Code"});
        fieldRegistry_.add({"active",      core::FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]        = name;
        j["code"]        = code;
        j["currency_id"] = currencyId > 0
                           ? nlohmann::json{currencyId, "Currency"}
                           : nlohmann::json(false);
        j["phone_code"]  = phoneCode;
        j["active"]      = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string())
            name       = j["name"].get<std::string>();
        if (j.contains("code") && j["code"].is_string())
            code       = j["code"].get<std::string>();
        if (j.contains("currency_id") && j["currency_id"].is_number_integer())
            currencyId = j["currency_id"].get<int>();
        if (j.contains("phone_code")  && j["phone_code"].is_number_integer())
            phoneCode  = j["phone_code"].get<int>();
        if (j.contains("active")      && j["active"].is_boolean())
            active     = j["active"].get<bool>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("name is required");
        if (code.empty()) e.push_back("code is required");
        return e;
    }
};

// ----------------------------------------------------------------
// ResCountryState — res.country.state
// ----------------------------------------------------------------
class ResCountryState : public core::BaseModel<ResCountryState> {
public:
    ODOO_MODEL("res.country.state", "res_country_state")

    std::string name, code;
    int         countryId = 0;

    explicit ResCountryState(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResCountryState>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char,    "State",   true});
        fieldRegistry_.add({"code",       core::FieldType::Char,    "Code",    true});
        fieldRegistry_.add({"country_id", core::FieldType::Many2one,"Country", true,
                             false, true, false, "res.country"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["code"]       = code;
        j["country_id"] = countryId > 0
                          ? nlohmann::json{countryId, "Country"}
                          : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string())
            name      = j["name"].get<std::string>();
        if (j.contains("code") && j["code"].is_string())
            code      = j["code"].get<std::string>();
        if (j.contains("country_id") && j["country_id"].is_number_integer())
            countryId = j["country_id"].get<int>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())   e.push_back("name is required");
        if (countryId <= 0) e.push_back("country_id is required");
        return e;
    }
};

// ----------------------------------------------------------------
// ResPartner — res.partner  (extended with address + localisation)
// ----------------------------------------------------------------
class ResPartner : public core::BaseModel<ResPartner> {
public:
    ODOO_MODEL("res.partner", "res_partner")

    std::string name, companyName, email, phone, mobile, website, street, city, zip, lang, comment, jobPosition;
    bool        isCompany    = false;
    bool        isIndividual = false;
    bool        isContractor = false;
    int         parentId     = 0;    // the COMPANY this contact belongs to
    int         companyId    = 0;    // NOT that: the multi-company owner of the row
    int         commercialPartnerId = 0;  // docs/130: the company at the top of the chain
    std::string street2;
    std::string commercialCompanyName;   // docs/130: what the list shows
    // docs/130 §4. "Carol, Big Carrots" for a person at a company, the
    // bare name for a company. Trigger-maintained; never written from here.
    std::string displayName;
    std::string addrType    = "contact";  // contact | invoice | delivery | other
    int         countryId    = 0;
    int         stateId      = 0;
    int         customerRank = 0;
    int         vendorRank   = 0;
    // Archiving is the answer for a contact that has history and so cannot be
    // deleted. The COLUMN existed all along; the field did not, so `active`
    // was rejected as unknown and a contact could be neither archived nor
    // filtered out of a picker.
    bool        active       = true;

    explicit ResPartner(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResPartner>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",         core::FieldType::Char,    "Name",         true});
        fieldRegistry_.add({"company_name", core::FieldType::Char,    "Company Name"});
        fieldRegistry_.add({"email",        core::FieldType::Char,    "Email"});
        fieldRegistry_.add({"phone",      core::FieldType::Char,    "Phone"});
        fieldRegistry_.add({"street",     core::FieldType::Char,    "Street"});
        fieldRegistry_.add({"city",       core::FieldType::Char,    "City"});
        fieldRegistry_.add({"zip",        core::FieldType::Char,    "ZIP"});
        fieldRegistry_.add({"lang",       core::FieldType::Char,    "Language"});
        fieldRegistry_.add({"is_company", core::FieldType::Boolean, "Is Company"});
        // The company this contact works for — a real partner, so "who works at
        // Acme" is a query and renaming Acme reaches everyone. company_name is
        // kept for imported data that never had a company record.
        fieldRegistry_.add({"parent_id",  core::FieldType::Many2one,"Company",
                             false, false, true, false, "res.partner"});
        // Deliberately NOT called "Company": this is the multi-company owner of
        // the row (docs/094), and labelling both fields the same is how a
        // customer's employer ends up filed as a tenant.
        fieldRegistry_.add({"company_id", core::FieldType::Many2one,"Owner Company",
                             false, false, true, false, "res.company"});
        // docs/130 §4. Maintained by trigger, never written from here — listing
        // it makes it readable and filterable, which is the entire point:
        // "everything for this customer" becomes one indexed equality.
        fieldRegistry_.add({"commercial_partner_id", core::FieldType::Many2one, "Commercial Entity",
                             false, false, true, false, "res.partner"});
        // docs/130 §6. sale_order.partner_invoice_id / partner_shipping_id have
        // been waiting for this since SaleModule.cpp:98.
        // What the CONTACT LIST shows in its Company column. Derived: the
        // commercial partner's name when that is a company, else the free-text
        // company_name. Trigger-maintained; never written by a client.
        fieldRegistry_.add({"commercial_company_name", core::FieldType::Char, "Company"});
        // docs/130 §4. The label every picker, list and lookup shows for a
        // partner: "Carol, Big Carrots" for a person at a company, "Big
        // Carrots" for the company itself. Stored and registered — stored so
        // a picker can ORDER and `ilike` on it, registered so search_read
        // will actually return it (rowsToJson_ projects COLUMNS, so a value
        // that exists only in serializeFields never reaches a client).
        fieldRegistry_.add({"display_name", core::FieldType::Char, "Display Name"});
        fieldRegistry_.add({"type",    core::FieldType::Char, "Address Type"});
        fieldRegistry_.add({"street2", core::FieldType::Char, "Street 2"});
        fieldRegistry_.add({"country_id", core::FieldType::Many2one,"Country",
                             false, false, true, false, "res.country"});
        fieldRegistry_.add({"state_id",    core::FieldType::Many2one,"State",
                             false, false, true, false, "res.country.state"});
        fieldRegistry_.add({"customer_rank",core::FieldType::Integer, "Customer Rank"});
        fieldRegistry_.add({"vendor_rank",  core::FieldType::Integer, "Vendor Rank"});
        fieldRegistry_.add({"active",       core::FieldType::Boolean, "Active"});
        fieldRegistry_.add({"is_contractor", core::FieldType::Boolean, "Is Contractor"});
        fieldRegistry_.add({"is_individual", core::FieldType::Boolean, "Is Individual"});
        fieldRegistry_.add({"mobile",        core::FieldType::Char,    "Mobile"});
        fieldRegistry_.add({"website",      core::FieldType::Char,    "Website"});
        fieldRegistry_.add({"comment",      core::FieldType::Text,    "Notes"});
        fieldRegistry_.add({"job_position", core::FieldType::Char,    "Job Position"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]         = name;
        j["company_name"] = companyName.empty() ? nlohmann::json(false) : nlohmann::json(companyName);
        j["email"]        = email.empty()       ? nlohmann::json(false) : nlohmann::json(email);
        j["phone"]         = phone.empty()       ? nlohmann::json(false) : nlohmann::json(phone);
        j["mobile"]        = mobile.empty()      ? nlohmann::json(false) : nlohmann::json(mobile);
        j["website"]       = website.empty()     ? nlohmann::json(false) : nlohmann::json(website);
        j["street"]        = street.empty()      ? nlohmann::json(false) : nlohmann::json(street);
        j["city"]          = city.empty()        ? nlohmann::json(false) : nlohmann::json(city);
        j["zip"]           = zip.empty()         ? nlohmann::json(false) : nlohmann::json(zip);
        j["lang"]          = lang.empty()        ? nlohmann::json(false) : nlohmann::json(lang);
        j["comment"]       = comment.empty()     ? nlohmann::json(false) : nlohmann::json(comment);
        j["job_position"]  = jobPosition.empty() ? nlohmann::json(false) : nlohmann::json(jobPosition);
        j["is_company"]    = isCompany;
        j["is_individual"] = isIndividual;
        j["is_contractor"] = isContractor;
        j["customer_rank"] = customerRank;
        j["vendor_rank"]   = vendorRank;
        j["active"]        = active;
        j["parent_id"]     = parentId > 0
                             ? nlohmann::json(parentId)
                             : nlohmann::json(false);
        j["commercial_partner_id"] = commercialPartnerId > 0
                             ? nlohmann::json(commercialPartnerId)
                             : nlohmann::json(false);
        j["commercial_company_name"] = commercialCompanyName.empty()
                             ? nlohmann::json(false) : nlohmann::json(commercialCompanyName);
        // Falls back to the bare name so a row written before migration 15
        // backfilled — or by a test that bypassed the trigger — still labels.
        j["display_name"] = displayName.empty() ? name : displayName;
        j["type"]          = addrType.empty() ? nlohmann::json("contact") : nlohmann::json(addrType);
        j["street2"]       = street2.empty() ? nlohmann::json(false) : nlohmann::json(street2);
        j["company_id"]    = companyId > 0
                             ? nlohmann::json{companyId, "Owner Company"}
                             : nlohmann::json(false);
        j["country_id"]    = countryId > 0
                             ? nlohmann::json{countryId, "Country"}
                             : nlohmann::json(false);
        j["state_id"]      = stateId > 0
                             ? nlohmann::json{stateId, "State"}
                             : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        auto str = [](const nlohmann::json& v) -> std::string {
            if (v.is_string()) return v.get<std::string>();
            return "";
        };
        auto m2o = [](const nlohmann::json& v) -> int {
            if (v.is_number_integer()) return v.get<int>();
            if (v.is_array() && v.size() >= 1 && v[0].is_number_integer()) return v[0].get<int>();
            return 0;
        };
        if (j.contains("name"))         name        = str(j["name"]);
        if (j.contains("company_name")) companyName = str(j["company_name"]);
        if (j.contains("email"))        email       = str(j["email"]);
        if (j.contains("phone"))       phone       = str(j["phone"]);
        if (j.contains("mobile"))      mobile      = str(j["mobile"]);
        if (j.contains("website"))     website     = str(j["website"]);
        if (j.contains("street"))      street      = str(j["street"]);
        if (j.contains("city"))        city        = str(j["city"]);
        if (j.contains("zip"))         zip         = str(j["zip"]);
        if (j.contains("lang"))        lang        = str(j["lang"]);
        if (j.contains("comment"))     comment     = str(j["comment"]);
        if (j.contains("job_position"))jobPosition = str(j["job_position"]);
        if (j.contains("is_company")   && j["is_company"].is_boolean())    isCompany    = j["is_company"].get<bool>();
        if (j.contains("is_individual")&& j["is_individual"].is_boolean()) isIndividual = j["is_individual"].get<bool>();
        if (j.contains("is_contractor")&& j["is_contractor"].is_boolean()) isContractor = j["is_contractor"].get<bool>();
        if (j.contains("customer_rank")&& j["customer_rank"].is_number_integer()) customerRank = j["customer_rank"].get<int>();
        if (j.contains("vendor_rank")  && j["vendor_rank"].is_number_integer())   vendorRank   = j["vendor_rank"].get<int>();
        if (j.contains("active")       && j["active"].is_boolean())              active       = j["active"].get<bool>();
        if (j.contains("parent_id"))   parentId   = m2o(j["parent_id"]);
        if (j.contains("street2"))     street2    = str(j["street2"]);
        if (j.contains("type"))        addrType   = str(j["type"]);
        // commercial_partner_id, commercial_company_name and display_name are
        // intentionally NOT deserialised: the database owns them. Accepting
        // commercial_partner_id from a client would let the caller claim any
        // customer's revenue belongs to them; accepting display_name would let
        // a contact present itself under a company it does not belong to.
        if (j.contains("company_id"))  companyId  = m2o(j["company_id"]);
        if (j.contains("country_id"))  countryId  = m2o(j["country_id"]);
        if (j.contains("state_id"))    stateId    = m2o(j["state_id"]);
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Name is required");
        if (!addrType.empty() && addrType != "contact" && addrType != "invoice"
            && addrType != "delivery" && addrType != "other")
            errors.push_back("Address Type must be contact, invoice, delivery or other");
        // Self-parenting and deeper cycles are NOT checked here on purpose.
        // validate() runs only on create, where the row has no id yet, so it
        // could never see `parent_id == id`; and write() does not call it at
        // all. The guarantee therefore lives in the schema — a CHECK for the
        // self case and a BEFORE trigger for longer cycles — where it also
        // holds for SQL that never goes through this model.
        return errors;
    }
};


// ================================================================
// 2. SERVICE
// ================================================================

class PartnerService : public core::BaseService {
public:
    explicit PartnerService(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseService(std::move(db)) {}

    std::string serviceName() const override { return "partner"; }

    /**
     * Archived contacts are hidden unless the caller asks for them.
     *
     * Without this, "Archive" would change a flag and nothing else: the
     * contact would still fill the list and every picker, which is the one
     * thing archiving exists to stop. A domain that mentions `active` is left
     * exactly as it came, so a caller can still see archived rows by saying so
     * — that is how the list's "Show archived" works.
     */
    static nlohmann::json hideArchived_(const nlohmann::json& domain) {
        const std::string s = domain.is_null() ? "" : domain.dump();
        if (s.find("\"active\"") != std::string::npos) return domain;
        nlohmann::json d = domain.is_array() ? domain : nlohmann::json::array();
        d.push_back({"active", "=", true});
        return d;
    }

    nlohmann::json searchRead(const nlohmann::json& domain,
                               const std::vector<std::string>& fields = {},
                               int limit = 80, int offset = 0,
                               const std::string& order = "id ASC") {
        ResPartner p(db_);
        return p.searchRead(hideArchived_(domain), fields, limit, offset, order);
    }
    nlohmann::json read(const std::vector<int>& ids,
                        const std::vector<std::string>& fields = {}) {
        ResPartner p(db_); return p.read(ids, fields);
    }
    // docs/130 §7 and §8. Two adjustments the model itself cannot make, because
    // both need to look at ANOTHER row (the parent) before the write happens.
    int  create(const nlohmann::json& v) {
        nlohmann::json vals = v;
        applyParentDefaults_(vals);
        ResPartner p(db_); return p.create(vals);
    }
    bool write(const std::vector<int>& ids, const nlohmann::json& v) {
        nlohmann::json vals = v;
        applyParentDefaults_(vals);
        ResPartner p(db_); return p.write(ids, vals);
    }
    /**
     * What would be destroyed or orphaned by deleting these contacts?
     *
     * Returns { blockers: [{model, label, count}], detach: [...] }.
     *
     * Deleting a contact is not a schema question. Five tables that carry a
     * partner_id have no FOREIGN KEY at all — rental_contract,
     * rental_contract_line, rental_event, rental_expense and
     * account_payment_unallocated — so PostgreSQL would let the row go and
     * leave a rental contract pointing at a customer that no longer exists.
     * And where an FK does exist it either raises a constraint violation the
     * user cannot act on, or silently SET NULLs a document's customer away.
     * So the check is explicit and lives here.
     */
    nlohmann::json dependencies(const std::vector<int>& ids) {
        nlohmann::json out;
        out["blockers"] = nlohmann::json::array();
        out["detach"]   = nlohmann::json::array();
        if (ids.empty()) return out;

        // A document records something that happened. Losing its customer —
        // whether by a refused DELETE or a silent SET NULL — is not something
        // to do behind the user's back, so these refuse the delete outright.
        static const std::vector<std::tuple<const char*, const char*, const char*>> kBlocking = {
            {"account_move",                "partner_id",          "invoices and journal entries"},
            {"account_move_line",           "partner_id",          "journal items"},
            {"account_payment",             "partner_id",          "payments"},
            {"account_payment_unallocated", "partner_id",          "unallocated payments"},
            {"sale_order",                  "partner_id",          "sales orders"},
            {"sale_order",                  "partner_invoice_id",  "sales orders (invoice address)"},
            {"sale_order",                  "partner_shipping_id", "sales orders (delivery address)"},
            {"purchase_order",              "partner_id",          "purchase orders"},
            {"stock_picking",               "partner_id",          "transfers"},
            {"rental_contract",             "partner_id",          "rental contracts"},
            {"rental_contract_line",        "partner_id",          "rental lines"},
            {"rental_expense",              "partner_id",          "rental expenses"},
            {"rental_event",                "partner_id",          "rental events"},
            {"partner_rental_price",        "partner_id",          "customer rental prices"},
            {"payment_proof",               "partner_id",          "payment proofs"},
            {"project_project",             "partner_id",          "projects"},
            {"project_task",                "partner_id",          "tasks"},
            {"hr_employee",                 "address_id",          "employee records"},
            {"res_users",                   "partner_id",          "user logins"},
            {"res_company",                 "partner_id",          "companies"},
        };
        // These are links, not records of events. They are cleared, and the
        // user is told which — an unexpected detachment is still a surprise.
        static const std::vector<std::tuple<const char*, const char*, const char*>> kDetach = {
            {"res_partner",                 "parent_id",       "contacts working at this company"},
            {"mrp_bom",                     "subcontractor_id","bills of materials"},
            {"stock_warehouse_orderpoint",  "supplier_id",     "reordering rules"},
            {"account_analytic_account",    "partner_id",      "analytic accounts"},
            {"account_bank_statement_line", "partner_id",      "bank statement lines"},
        };

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // The id list is built from ints, so it cannot carry injection; table
        // and column names are literals from the tables above, never input.
        std::string idList;
        for (size_t i = 0; i < ids.size(); ++i)
            idList += (i ? "," : "") + std::to_string(ids[i]);

        auto tally = [&](const auto& spec, const char* key) {
            for (const auto& [table, column, label] : spec) {
                // A module may not be installed in every deployment.
                const auto exists = txn.exec(
                    "SELECT 1 FROM information_schema.columns "
                    " WHERE table_schema='public' AND table_name=$1 AND column_name=$2",
                    pqxx::params{table, column});
                if (exists.empty()) continue;
                const auto r = txn.exec(
                    "SELECT count(*) FROM " + std::string(table) +
                    " WHERE " + std::string(column) + " IN (" + idList + ")");
                const long n = r.empty() ? 0 : r[0][0].as<long>(0);
                if (n > 0)
                    out[key].push_back({{"model", table}, {"label", label}, {"count", n}});
            }
        };
        tally(kBlocking, "blockers");
        tally(kDetach,   "detach");
        txn.commit();
        return out;
    }

    bool unlink(const std::vector<int>& ids) {
        const auto dep = dependencies(ids);
        if (!dep["blockers"].empty()) {
            // Name what is in the way and how much of it. "Cannot delete" on
            // its own sends the user hunting; this tells them where to look.
            //
            // "label (count)" rather than "count label" so the sentence reads
            // correctly whether there is one or many — the alternative is a
            // singular form for every one of the twenty labels above.
            std::string msg = "This contact cannot be deleted — it is still used by ";
            const auto& b = dep["blockers"];
            for (size_t i = 0; i < b.size(); ++i) {
                if (i) msg += (i + 1 == b.size()) ? " and " : ", ";
                msg += b[i]["label"].get<std::string>() + " (" +
                       std::to_string(b[i]["count"].get<long>()) + ")";
            }
            msg += ". Archive it instead — it will stop appearing in lists "
                   "while its history stays intact.";
            throw infrastructure::ValidationError(msg);
        }
        ResPartner p(db_); return p.unlink(ids);
    }

    nlohmann::json fieldsGet(const std::vector<std::string>& f = {},
                              const std::vector<std::string>& a = {}) {
        ResPartner p(db_); return p.fieldsGet(f, a);
    }
    int searchCount(const nlohmann::json& d) {
        // Same rule as searchRead, or the picker's "N more…" would count rows
        // it will never show.
        ResPartner p(db_); return p.searchCount(hideArchived_(d));
    }

private:
    /// Fill in what belongs to a contact by virtue of having a company.
    ///
    /// ADDRESS (docs/130 §7, Odoo res_partner.py:344-349). A contact at Acme
    /// gets Acme's address, but only for fields the caller left EMPTY and only
    /// when the parent actually has one. Deliberately a create/write-time
    /// default, not a live sync: Odoo warns that re-homing a contact should be
    /// rare, and a background sync that overwrites a hand-typed address is worse
    /// than retyping it.
    ///
    /// company_name (docs/130 §8, Odoo :529). Once parent_id is set, the free
    /// text is redundant and can only drift out of agreement with the relation.
    /// Odoo clears it; so do we.
    void applyParentDefaults_(nlohmann::json& vals) {
        if (!vals.is_object() || !vals.contains("parent_id")) return;
        int pid = 0;
        const auto& pv = vals["parent_id"];
        if (pv.is_number_integer())                       pid = pv.get<int>();
        else if (pv.is_array() && !pv.empty() && pv[0].is_number_integer())
                                                          pid = pv[0].get<int>();
        if (pid <= 0) return;

        vals["company_name"] = false;   // the relation is now the source of truth

        static const char* kAddr[] = {"street","street2","city","zip","state_id","country_id"};
        bool anyMissing = false;
        for (const char* f : kAddr) {
            const bool given = vals.contains(f) && !vals[f].is_null()
                            && !(vals[f].is_boolean() && !vals[f].get<bool>())
                            && !(vals[f].is_string()  && vals[f].get<std::string>().empty());
            if (!given) { anyMissing = true; break; }
        }
        if (!anyMissing) return;

        try {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto r = txn.exec("SELECT COALESCE(street,''), COALESCE(street2,''), "
                              "COALESCE(city,''), COALESCE(zip,''), "
                              "COALESCE(state_id,0), COALESCE(country_id,0) "
                              "FROM res_partner WHERE id = $1",
                              pqxx::params{pid});
            txn.commit();
            if (r.empty()) return;
            const std::string ps  = r[0][0].c_str(), ps2 = r[0][1].c_str();
            const std::string pc  = r[0][2].c_str(), pz  = r[0][3].c_str();
            const int pstate = r[0][4].as<int>(), pcountry = r[0][5].as<int>();
            // Only inherit when the parent HAS an address; otherwise blanking
            // the child would be a change, not a default.
            if (ps.empty() && ps2.empty() && pc.empty() && pz.empty()
                && pstate == 0 && pcountry == 0) return;

            auto fill = [&](const char* key, const std::string& val) {
                if (val.empty()) return;
                const bool given = vals.contains(key) && !vals[key].is_null()
                                && !(vals[key].is_boolean() && !vals[key].get<bool>())
                                && !(vals[key].is_string() && vals[key].get<std::string>().empty());
                if (!given) vals[key] = val;
            };
            auto fillId = [&](const char* key, int val) {
                if (val <= 0) return;
                const bool given = vals.contains(key) && vals[key].is_number_integer()
                                && vals[key].get<int>() > 0;
                if (!given) vals[key] = val;
            };
            fill("street",  ps);  fill("street2", ps2);
            fill("city",    pc);  fill("zip",     pz);
            fillId("state_id", pstate); fillId("country_id", pcountry);
        } catch (const std::exception&) {
            // Inheriting an address is a convenience. If the lookup fails the
            // contact must still save — with its own (possibly empty) address.
        }
    }
};


// ================================================================
// 3. VIEW
// ================================================================

class PartnerListView : public core::BaseView {
public:
    std::string viewName() const override { return "res.partner.list"; }

    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"commercial_company_name\"/>"
               "<field name=\"email\"/>"
               "<field name=\"phone\"/>"
               "<field name=\"active\"/>"
               "</list>";
    }

    nlohmann::json fields() const override {
        return {
            {"name",         {{"type","char"}, {"string","Name"},         {"required",true}}},
            {"commercial_company_name", {{"type","char"}, {"string","Company"}}},
            {"email",        {{"type","char"}, {"string","Email"}}},
            {"phone",        {{"type","char"}, {"string","Phone"}}},
            // Declared so the list can offer "Show archived" — the list only
            // offers it for models that actually have the flag — and so an
            // archived row is visibly archived once shown.
            {"active",       {{"type","boolean"}, {"string","Active"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class PartnerFormView : public core::BaseView {
public:
    std::string viewName() const override { return "res.partner.form"; }

    std::string arch() const override {
        return "<form>"
               "<field name=\"name\"/>"
               "<field name=\"email\"/>"
               "<field name=\"phone\"/>"
               "<field name=\"street\"/>"
               "<field name=\"city\"/>"
               "<field name=\"zip\"/>"
               "<field name=\"country_id\"/>"
               "<field name=\"is_company\"/>"
               "<field name=\"parent_id\"/>"
               "<field name=\"type\"/>"
               "<field name=\"street2\"/>"
               "<field name=\"company_name\"/>"
               "<field name=\"job_position\"/>"
               "</form>";
    }

    nlohmann::json fields() const override {
        return {
            {"name",       {{"type","char"},    {"string","Name"},       {"required",true}}},
            {"email",      {{"type","char"},    {"string","Email"}}},
            {"phone",      {{"type","char"},    {"string","Phone"}}},
            {"street",     {{"type","char"},    {"string","Street"}}},
            {"city",       {{"type","char"},    {"string","City"}}},
            {"zip",        {{"type","char"},    {"string","ZIP"}}},
            {"is_company", {{"type","boolean"}, {"string","Is Company"}}},
            {"parent_id",  {{"type","many2one"},{"string","Company"}, {"relation","res.partner"},
                            {"domain", nlohmann::json::array({nlohmann::json::array({"is_company","=",true})})}}},
            {"company_name",{{"type","char"},   {"string","Company Name (free text)"}}},
            {"street2",     {{"type","char"},   {"string","Street 2"}}},
            {"type",        {{"type","selection"}, {"string","Address Type"},
                             {"selection", nlohmann::json::array({
                                 nlohmann::json::array({"contact",  "Contact"}),
                                 nlohmann::json::array({"invoice",  "Invoice Address"}),
                                 nlohmann::json::array({"delivery", "Delivery Address"}),
                                 nlohmann::json::array({"other",    "Other"})})}}},
            {"commercial_partner_id", {{"type","many2one"},{"string","Commercial Entity"},
                             {"relation","res.partner"}, {"readonly", true}}},
            {"job_position",{{"type","char"},   {"string","Job Position"}}},
            {"company_id", {{"type","many2one"},{"string","Owner Company"}, {"relation","res.company"}}},
            {"country_id", {{"type","many2one"},{"string","Country"}, {"relation","res.country"}}},
            {"state_id",   {{"type","many2one"},{"string","State"},   {"relation","res.country.state"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};


// ================================================================
// 4. VIEWMODELS
// ================================================================

// ----------------------------------------------------------------
// CurrencyViewModel — res.currency  (P2)
//
// res.currency used to be read-only (LookupViewModel), but the FX rate is
// now user-maintained (docs/048 §4.3), so write has to be supported. Any
// write drops the dispatcher's 60 s currency cache — otherwise a rate
// change is invisible for up to a minute, which looks like a lost edit.
// ----------------------------------------------------------------
class CurrencyViewModel : public core::GenericViewModel<ResCurrency> {
public:
    explicit CurrencyViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : core::GenericViewModel<ResCurrency>(std::move(db))
    {
        REGISTER_MUTATOR("write", handleWriteAndInvalidate)
    }

    nlohmann::json handleWriteAndInvalidate(const core::CallKwArgs& call) {
        const auto vals = call.arg(1);
        if (vals.is_object() && vals.contains("rate")) {
            if (!vals["rate"].is_number())
                throw infrastructure::ValidationError("Rate must be a number.");
            // A zero or negative rate makes every conversion nonsense and a
            // division by it undefined; reject rather than store it.
            if (vals["rate"].get<double>() <= 0.0)
                throw infrastructure::ValidationError(
                    "Rate must be greater than zero. It is how many base-currency "
                    "units equal 1 unit of this currency.");
        }
        auto result = this->handleWrite(call);
        core::CacheInvalidation::currency();
        return result;
    }
};

// Generic read-only viewmodel for lookup tables (lang, currency, country, etc.)
template<typename TModel>
class LookupViewModel : public core::BaseViewModel {
public:
    explicit LookupViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",  handleSearchRead)
        REGISTER_METHOD("read",         handleRead)
        REGISTER_METHOD("web_read",     handleRead)
        REGISTER_METHOD("fields_get",   handleFieldsGet)
        REGISTER_METHOD("search_count", handleSearchCount)
    }

    std::string modelName() const override { return TModel::MODEL_NAME; }

private:
    std::shared_ptr<infrastructure::DbConnection> db_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(),
                                call.order().empty() ? "id ASC" : call.order());
    }
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchCount(call.domain());
    }
};

class PartnerViewModel : public core::BaseViewModel {
public:
    explicit PartnerViewModel(
        std::shared_ptr<PartnerService>    service,
        std::shared_ptr<core::ViewFactory> viewFactory)
        : service_    (std::move(service))
        , viewFactory_(std::move(viewFactory))
    {
        REGISTER_METHOD("search_read",  handleSearchRead)
        REGISTER_METHOD("read",         handleRead)
        REGISTER_METHOD("web_read",     handleRead)
        REGISTER_MUTATOR("create",       handleCreate)
        REGISTER_MUTATOR("write",        handleWrite)
        REGISTER_MUTATOR("unlink",       handleUnlink)
        // Read-only: what WOULD happen if this contact were deleted. The form
        // asks before it offers the button, so the user learns why a contact
        // cannot go before pressing Delete rather than after.
        REGISTER_METHOD("check_unlink", handleCheckUnlink)
        REGISTER_METHOD("fields_get",   handleFieldsGet)
        REGISTER_METHOD("search_count", handleSearchCount)
    }

    std::string modelName() const override { return "res.partner"; }

private:
    std::shared_ptr<PartnerService>    service_;
    std::shared_ptr<core::ViewFactory> viewFactory_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        const int  limit = call.limit() > 0 ? call.limit() : 80;
        const auto order = call.order().empty() ? "id ASC" : call.order();
        return service_->searchRead(call.domain(), call.fields(),
                                    limit, call.offset(), order);
    }
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        return service_->read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        const auto newId = service_->create(v);
        return newId;
    }
    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        const auto result = service_->write(call.ids(), v);
        return result;
    }
    nlohmann::json handleUnlink(const core::CallKwArgs& call) {
        const auto ids    = call.ids();
        const auto result = service_->unlink(ids);
        return result;
    }
    nlohmann::json handleCheckUnlink(const core::CallKwArgs& call) {
        return service_->dependencies(call.ids());
    }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        const auto attrs = call.kwargs.contains("attributes") &&
                           call.kwargs["attributes"].is_array()
                           ? call.kwargs["attributes"].get<std::vector<std::string>>()
                           : std::vector<std::string>{};
        return service_->fieldsGet(call.fields(), attrs);
    }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
        return service_->searchCount(call.domain());
    }
};


// ================================================================
// 5. MODULE — method implementations
// ================================================================

BaseModule::BaseModule(core::ModelFactory&     modelFactory,
                       core::ServiceFactory&   serviceFactory,
                       core::ViewModelFactory& viewModelFactory,
                       core::ViewFactory&      viewFactory)
    : models_    (modelFactory)
    , services_  (serviceFactory)
    , viewModels_(viewModelFactory)
    , views_     (viewFactory)
{}

std::string              BaseModule::moduleName()   const { return "base"; }
std::string              BaseModule::version()      const { return "19.0.1.0.0"; }
std::vector<std::string> BaseModule::dependencies() const { return {}; }

void BaseModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("res.lang",          [db]{ return std::make_shared<ResLang>(db); });
    models_.registerCreator("res.currency",      [db]{ return std::make_shared<ResCurrency>(db); });
    models_.registerCreator("res.country",       [db]{ return std::make_shared<ResCountry>(db); });
    models_.registerCreator("res.country.state", [db]{ return std::make_shared<ResCountryState>(db); });
    models_.registerCreator("res.partner",       [db]{ return std::make_shared<ResPartner>(db); });
}

void BaseModule::registerServices() {
    auto db = services_.db();
    services_.registerCreator("partner", [db]{
        return std::make_shared<PartnerService>(db);
    });
}

void BaseModule::registerViews() {
    views_.registerView<PartnerListView>("res.partner.list");
    views_.registerView<PartnerFormView>("res.partner.form");
}

void BaseModule::registerViewModels() {
    auto& sf = services_;
    auto& vf = views_;
    auto  db = services_.db();

    viewModels_.registerCreator("res.partner", [&sf, &vf] {
        auto svc   = std::static_pointer_cast<PartnerService>(
            sf.create("partner", core::Lifetime::Singleton));
        auto vfPtr = std::shared_ptr<core::ViewFactory>(&vf, [](auto*){});
        return std::make_shared<PartnerViewModel>(svc, vfPtr);
    });
    viewModels_.registerCreator("res.lang", [db]{
        return std::make_shared<LookupViewModel<ResLang>>(db);
    });
    viewModels_.registerCreator("res.currency", [db]{
        return std::make_shared<CurrencyViewModel>(db);   // P2: writable (rate)
    });
    viewModels_.registerCreator("res.country", [db]{
        // ResCountry needs write support for the Settings → Countries tab
        struct ResCountryViewModel : public core::BaseViewModel {
            std::shared_ptr<infrastructure::DbConnection> db_;
            explicit ResCountryViewModel(std::shared_ptr<infrastructure::DbConnection> d) : db_(d) {
                REGISTER_METHOD("search_read",  handleSearchRead)
                REGISTER_METHOD("read",         handleRead)
                REGISTER_METHOD("web_read",     handleRead)
                REGISTER_METHOD("fields_get",   handleFieldsGet)
                REGISTER_METHOD("search_count", handleSearchCount)
                REGISTER_MUTATOR("write",        handleWrite)
            }
            std::string modelName() const override { return "res.country"; }
            nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
                ResCountry m(db_);
                return m.searchRead(call.domain(), call.fields(),
                                    call.limit() > 0 ? call.limit() : 300,
                                    call.offset(),
                                    call.order().empty() ? "name ASC" : call.order());
            }
            nlohmann::json handleRead(const core::CallKwArgs& call) {
                ResCountry m(db_); return m.read(call.ids(), call.fields());
            }
            nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
                ResCountry m(db_); return m.fieldsGet(call.fields());
            }
            nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
                ResCountry m(db_); return m.searchCount(call.domain());
            }
            nlohmann::json handleWrite(const core::CallKwArgs& call) {
                const auto v = call.arg(1);
                if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
                ResCountry m(db_); return m.write(call.ids(), v);
            }
        };
        return std::make_shared<ResCountryViewModel>(db);
    });
    viewModels_.registerCreator("res.country.state", [db]{
        return std::make_shared<LookupViewModel<ResCountryState>>(db);
    });
}

void BaseModule::registerRoutes() {}

// P2 (docs/047, docs/048). Registered from BaseModule because these
// migrations rewrite tables owned by account/sale/purchase/stock/product,
// and MigrationRunner applies strictly in version order regardless of which
// module registered them — so ordering is governed by the 9xx numbers, not
// by module boot sequence.
void BaseModule::registerMigrations(cerp::infrastructure::MigrationRunner& runner) {
    // P2 (docs/047, docs/048): money, price and quantity columns become BIGINT
    // micro-units. Enabled once Phases 3 and 4 were complete — the conversion
    // boundary in BaseModel plus the 22 raw-SQL sites outside it (docs/049).
    //
    // Take a pg_dump before first run. MigrationRunner applies each migration
    // in its own transaction and halts startup on failure.
    cerp::core::registerMoneyMigrations(runner);

    // docs/130 — the partner hierarchy: commercial_partner_id, the tenant
    // descending to contacts, and address types. These BACKFILL data, which is
    // why they are migrations and not ensureSchema_ ALTERs.
    registerPartnerMigrations(runner);
}

void BaseModule::initialize() {
    ensureSchema_();
    seedLang_();
    seedCurrencies_();
    seedCountries_();
}

// ----------------------------------------------------------
// Schema — all idempotent
// ----------------------------------------------------------
void BaseModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS res_lang (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            code        VARCHAR NOT NULL UNIQUE,
            iso_code    VARCHAR,
            url_code    VARCHAR NOT NULL DEFAULT '',
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            direction   VARCHAR NOT NULL DEFAULT 'ltr',
            date_format VARCHAR NOT NULL DEFAULT '%m/%d/%Y',
            time_format VARCHAR NOT NULL DEFAULT '%H:%M:%S',
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS res_currency (
            id             SERIAL PRIMARY KEY,
            name           VARCHAR(3)    NOT NULL UNIQUE,
            symbol         VARCHAR       NOT NULL,
            position       VARCHAR       NOT NULL DEFAULT 'after',
            rounding       NUMERIC(12,6) NOT NULL DEFAULT 0.01,
            decimal_places INTEGER       NOT NULL DEFAULT 2,
            active         BOOLEAN       NOT NULL DEFAULT TRUE,
            create_date    TIMESTAMP     DEFAULT now(),
            write_date     TIMESTAMP     DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS res_country (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR    NOT NULL,
            code        VARCHAR(2) NOT NULL UNIQUE,
            currency_id INTEGER    REFERENCES res_currency(id) ON DELETE SET NULL,
            phone_code  INTEGER,
            active      BOOLEAN    NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP  DEFAULT now(),
            write_date  TIMESTAMP  DEFAULT now()
        )
    )");
    txn.exec("ALTER TABLE res_country ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS res_country_state (
            id          SERIAL  PRIMARY KEY,
            country_id  INTEGER NOT NULL REFERENCES res_country(id) ON DELETE CASCADE,
            name        VARCHAR NOT NULL,
            code        VARCHAR NOT NULL,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    // Add unique constraint so ON CONFLICT (country_id, code) DO NOTHING works in seedWorldData_
    txn.exec(R"(DO $$ BEGIN
        IF NOT EXISTS (SELECT 1 FROM pg_constraint
                       WHERE conname='res_country_state_country_id_code_key') THEN
            ALTER TABLE res_country_state ADD CONSTRAINT res_country_state_country_id_code_key
                UNIQUE (country_id, code);
        END IF;
    END $$)");

    // Base partner table — also extended below with new columns
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS res_partner (
            id          SERIAL  PRIMARY KEY,
            name        VARCHAR NOT NULL,
            email       VARCHAR,
            phone       VARCHAR,
            is_company  BOOLEAN   NOT NULL DEFAULT FALSE,
            company_id  INTEGER,
            active      BOOLEAN   NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    // Address + localisation columns — idempotent on existing installs
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS street        VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS city          VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS zip           VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS lang          VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS country_id    INTEGER REFERENCES res_country(id)");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS state_id      INTEGER REFERENCES res_country_state(id)");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS mobile        VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS website       VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS comment       TEXT");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS job_position  VARCHAR");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS customer_rank INTEGER NOT NULL DEFAULT 0");
    // Rename supplier_rank → vendor_rank (idempotent: skip if already renamed)
    txn.exec(R"(DO $$ BEGIN
        IF EXISTS (SELECT 1 FROM information_schema.columns
                   WHERE table_name='res_partner' AND column_name='supplier_rank') THEN
            ALTER TABLE res_partner RENAME COLUMN supplier_rank TO vendor_rank;
        END IF;
    END $$)");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS vendor_rank INTEGER NOT NULL DEFAULT 0");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS is_contractor BOOLEAN NOT NULL DEFAULT FALSE");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS is_individual BOOLEAN NOT NULL DEFAULT FALSE");
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS company_name  VARCHAR");

    // ------------------------------------------------------------------
    // parent_id — the company a contact belongs to.
    //
    // Until this existed there was no partner->partner relation at all, so
    // "link this person to that company" had nowhere to go: the API accepted
    // parent_id, dropped it, and reported success. company_name (free text)
    // was the only company on a contact, which meant two spellings were two
    // companies and renaming one updated nobody.
    //
    // ON DELETE SET NULL, deliberately. Deleting a company must not delete the
    // people in it — they are still real customers with invoices attached.
    // CASCADE here would quietly remove them.
    // ------------------------------------------------------------------
    txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS parent_id INTEGER");
    txn.exec(R"(DO $$ BEGIN
        IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'res_partner_parent_fk') THEN
            ALTER TABLE res_partner ADD CONSTRAINT res_partner_parent_fk
                FOREIGN KEY (parent_id) REFERENCES res_partner(id) ON DELETE SET NULL;
        END IF;
        IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'res_partner_parent_not_self') THEN
            ALTER TABLE res_partner ADD CONSTRAINT res_partner_parent_not_self
                CHECK (parent_id IS NULL OR parent_id <> id);
        END IF;
    END $$;)");
    txn.exec("CREATE INDEX IF NOT EXISTS res_partner_parent_idx ON res_partner(parent_id)");

    // A cycle (Acme -> Jane -> Acme) makes any walk up the tree hang, and a
    // CHECK constraint cannot see beyond its own row. This is enforced in the
    // database rather than the model so it holds for SQL written by hand, by a
    // migration, or by a module that never goes through ResPartner.
    txn.exec(R"(CREATE OR REPLACE FUNCTION res_partner_no_cycle() RETURNS trigger AS $fn$
        DECLARE hop INTEGER := NEW.parent_id; depth INTEGER := 0;
        BEGIN
            WHILE hop IS NOT NULL AND depth < 64 LOOP
                IF hop = NEW.id THEN
                    RAISE EXCEPTION 'contact hierarchy would form a cycle at partner %', NEW.id
                        USING ERRCODE = 'check_violation';
                END IF;
                SELECT parent_id INTO hop FROM res_partner WHERE id = hop;
                depth := depth + 1;
            END LOOP;
            RETURN NEW;
        END $fn$ LANGUAGE plpgsql)");
    txn.exec("DROP TRIGGER IF EXISTS res_partner_no_cycle_trg ON res_partner");
    txn.exec(R"(CREATE TRIGGER res_partner_no_cycle_trg
        BEFORE INSERT OR UPDATE OF parent_id ON res_partner
        FOR EACH ROW WHEN (NEW.parent_id IS NOT NULL)
        EXECUTE FUNCTION res_partner_no_cycle())");

    txn.commit();
}

// ----------------------------------------------------------
// Seeds — each checks for existing rows before inserting
// ----------------------------------------------------------

void BaseModule::seedLang_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    if (txn.exec("SELECT COUNT(*) FROM res_lang")[0][0].as<int>() > 0) return;
    txn.exec(R"(
        INSERT INTO res_lang (id, name, code, iso_code, url_code, active, direction)
        VALUES (1, 'English', 'en_US', 'en', 'en', TRUE, 'ltr')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('res_lang_id_seq', 1, true)");
    txn.commit();
}

void BaseModule::seedCurrencies_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    if (txn.exec("SELECT COUNT(*) FROM res_currency")[0][0].as<int>() > 0) return;
    txn.exec(R"(
        INSERT INTO res_currency (id, name, symbol, position, rounding, decimal_places, active)
        VALUES
            (1, 'USD', '$', 'before', 0.01, 2, TRUE),
            (2, 'EUR', '€', 'after',  0.01, 2, TRUE),
            (3, 'GBP', '£', 'before', 0.01, 2, TRUE),
            (4, 'JPY', '¥', 'before', 1.00, 0, TRUE),
            (5, 'CNY', '¥', 'after',  0.01, 2, TRUE)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec("SELECT setval('res_currency_id_seq', 5, true)");
    txn.commit();
}

void BaseModule::seedCountries_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    // seedWorldData_ is idempotent: ON CONFLICT DO NOTHING on all inserts
    seedWorldData_(txn);
    txn.commit();
}

} // namespace cerp::modules::base
