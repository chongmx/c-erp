#pragma once
// =============================================================
// modules/base/BaseModule.hpp
//
// Provides the foundation models every other module depends on:
//   res.partner, res.lang, res.currency, res.country,
//   res.country.state
//
// initialize() creates all tables and seeds minimal reference
// data (one language, five currencies, fifteen countries).
// Uses ALTER TABLE … ADD COLUMN IF NOT EXISTS for idempotence.
// =============================================================
#include "BaseModel.hpp"
#include "BaseService.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "IModule.hpp"
#include "Factories.hpp"
#include "Domain.hpp"
#include "FieldRegistry.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::base {

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
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]           = name;
        j["symbol"]         = symbol;
        j["position"]       = position;
        j["rounding"]       = rounding;
        j["decimal_places"] = decimalPlaces;
        j["active"]         = active;
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

    explicit ResCountry(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResCountry>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        core::FieldType::Char,    "Country",  true});
        fieldRegistry_.add({"code",        core::FieldType::Char,    "ISO Code", true});
        fieldRegistry_.add({"currency_id", core::FieldType::Many2one,"Currency",
                             false, false, true, false, "res.currency"});
        fieldRegistry_.add({"phone_code",  core::FieldType::Integer, "Phone Code"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]        = name;
        j["code"]        = code;
        j["currency_id"] = currencyId > 0
                           ? nlohmann::json{currencyId, "Currency"}
                           : nlohmann::json(false);
        j["phone_code"]  = phoneCode;
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

    std::string name, email, phone, street, city, zip, lang;
    bool        isCompany = false;
    int         companyId = 0;
    int         countryId = 0;
    int         stateId   = 0;

    explicit ResPartner(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResPartner>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char,    "Name",       true});
        fieldRegistry_.add({"email",      core::FieldType::Char,    "Email"});
        fieldRegistry_.add({"phone",      core::FieldType::Char,    "Phone"});
        fieldRegistry_.add({"street",     core::FieldType::Char,    "Street"});
        fieldRegistry_.add({"city",       core::FieldType::Char,    "City"});
        fieldRegistry_.add({"zip",        core::FieldType::Char,    "ZIP"});
        fieldRegistry_.add({"lang",       core::FieldType::Char,    "Language"});
        fieldRegistry_.add({"is_company", core::FieldType::Boolean, "Is Company"});
        fieldRegistry_.add({"company_id", core::FieldType::Many2one,"Company",
                             false, false, true, false, "res.company"});
        fieldRegistry_.add({"country_id", core::FieldType::Many2one,"Country",
                             false, false, true, false, "res.country"});
        fieldRegistry_.add({"state_id",   core::FieldType::Many2one,"State",
                             false, false, true, false, "res.country.state"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["email"]      = email;
        j["phone"]      = phone;
        j["street"]     = street;
        j["city"]       = city;
        j["zip"]        = zip;
        j["lang"]       = lang;
        j["is_company"] = isCompany;
        j["company_id"] = companyId > 0
                          ? nlohmann::json{companyId, "Company"}
                          : nlohmann::json(false);
        j["country_id"] = countryId > 0
                          ? nlohmann::json{countryId, "Country"}
                          : nlohmann::json(false);
        j["state_id"]   = stateId > 0
                          ? nlohmann::json{stateId, "State"}
                          : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")       && j["name"].is_string())
            name       = j["name"].get<std::string>();
        if (j.contains("email")      && j["email"].is_string())
            email      = j["email"].get<std::string>();
        if (j.contains("phone")      && j["phone"].is_string())
            phone      = j["phone"].get<std::string>();
        if (j.contains("street")     && j["street"].is_string())
            street     = j["street"].get<std::string>();
        if (j.contains("city")       && j["city"].is_string())
            city       = j["city"].get<std::string>();
        if (j.contains("zip")        && j["zip"].is_string())
            zip        = j["zip"].get<std::string>();
        if (j.contains("lang")       && j["lang"].is_string())
            lang       = j["lang"].get<std::string>();
        if (j.contains("is_company") && j["is_company"].is_boolean())
            isCompany  = j["is_company"].get<bool>();
        if (j.contains("company_id") && j["company_id"].is_number_integer())
            companyId  = j["company_id"].get<int>();
        if (j.contains("country_id") && j["country_id"].is_number_integer())
            countryId  = j["country_id"].get<int>();
        if (j.contains("state_id")   && j["state_id"].is_number_integer())
            stateId    = j["state_id"].get<int>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Name is required");
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

    nlohmann::json searchRead(const nlohmann::json& domain,
                               const std::vector<std::string>& fields = {},
                               int limit = 80, int offset = 0,
                               const std::string& order = "id ASC") {
        ResPartner p(db_); return p.searchRead(domain, fields, limit, offset, order);
    }
    nlohmann::json read(const std::vector<int>& ids,
                        const std::vector<std::string>& fields = {}) {
        ResPartner p(db_); return p.read(ids, fields);
    }
    int  create(const nlohmann::json& v) { ResPartner p(db_); return p.create(v); }
    bool write(const std::vector<int>& ids, const nlohmann::json& v) {
        ResPartner p(db_); return p.write(ids, v);
    }
    bool unlink(const std::vector<int>& ids) { ResPartner p(db_); return p.unlink(ids); }
    nlohmann::json fieldsGet(const std::vector<std::string>& f = {},
                              const std::vector<std::string>& a = {}) {
        ResPartner p(db_); return p.fieldsGet(f, a);
    }
    int searchCount(const nlohmann::json& d) { ResPartner p(db_); return p.searchCount(d); }
};


// ================================================================
// 3. VIEW
// ================================================================

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
            {"company_id", {{"type","many2one"},{"string","Company"}, {"relation","res.company"}}},
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
        REGISTER_METHOD("create",       handleCreate)
        REGISTER_METHOD("write",        handleWrite)
        REGISTER_METHOD("unlink",       handleUnlink)
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
        auto records = service_->searchRead(call.domain(), call.fields(),
                                            limit, call.offset(), order);
        auto view = viewFactory_->getView("res.partner", "form");
        nlohmann::json result = nlohmann::json::array();
        for (const auto& rec : records) result.push_back(view->render(rec));
        return result;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        auto records = service_->read(call.ids(), call.fields());
        auto view = viewFactory_->getView("res.partner", "form");
        nlohmann::json result = nlohmann::json::array();
        for (const auto& rec : records) result.push_back(view->render(rec));
        return result;
    }
    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        return service_->create(v);
    }
    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        return service_->write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const core::CallKwArgs& call) {
        return service_->unlink(call.ids());
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
// 5. MODULE
// ================================================================
class BaseModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "base"; }

    explicit BaseModule(core::ModelFactory&     modelFactory,
                        core::ServiceFactory&   serviceFactory,
                        core::ViewModelFactory& viewModelFactory,
                        core::ViewFactory&      viewFactory)
        : models_    (modelFactory)
        , services_  (serviceFactory)
        , viewModels_(viewModelFactory)
        , views_     (viewFactory)
    {}

    std::string              moduleName()   const override { return "base"; }
    std::string              version()      const override { return "19.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {}; }

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("res.lang",          [db]{ return std::make_shared<ResLang>(db); });
        models_.registerCreator("res.currency",      [db]{ return std::make_shared<ResCurrency>(db); });
        models_.registerCreator("res.country",       [db]{ return std::make_shared<ResCountry>(db); });
        models_.registerCreator("res.country.state", [db]{ return std::make_shared<ResCountryState>(db); });
        models_.registerCreator("res.partner",       [db]{ return std::make_shared<ResPartner>(db); });
    }

    void registerServices() override {
        auto db = services_.db();
        services_.registerCreator("partner", [db]{
            return std::make_shared<PartnerService>(db);
        });
    }

    void registerViews() override {
        views_.registerView<PartnerFormView>("res.partner.form");
    }

    void registerViewModels() override {
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
            return std::make_shared<LookupViewModel<ResCurrency>>(db);
        });
        viewModels_.registerCreator("res.country", [db]{
            return std::make_shared<LookupViewModel<ResCountry>>(db);
        });
        viewModels_.registerCreator("res.country.state", [db]{
            return std::make_shared<LookupViewModel<ResCountryState>>(db);
        });
    }

    void registerRoutes() override {}

    void initialize() override {
        ensureSchema_();
        seedLang_();
        seedCurrencies_();
        seedCountries_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    // ----------------------------------------------------------
    // Schema — all idempotent
    // ----------------------------------------------------------
    void ensureSchema_() {
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
                create_date TIMESTAMP  DEFAULT now(),
                write_date  TIMESTAMP  DEFAULT now()
            )
        )");

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
        txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS street     VARCHAR");
        txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS city       VARCHAR");
        txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS zip        VARCHAR");
        txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS lang       VARCHAR");
        txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS country_id INTEGER REFERENCES res_country(id)");
        txn.exec("ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS state_id   INTEGER REFERENCES res_country_state(id)");

        txn.commit();
    }

    // ----------------------------------------------------------
    // Seeds — each checks for existing rows before inserting
    // ----------------------------------------------------------

    void seedLang_() {
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

    void seedCurrencies_() {
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

    void seedCountries_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        if (txn.exec("SELECT COUNT(*) FROM res_country")[0][0].as<int>() > 0) return;
        txn.exec(R"(
            INSERT INTO res_country (id, name, code, currency_id, phone_code) VALUES
                ( 1, 'United States',   'US',  1,    1),
                ( 2, 'United Kingdom',  'GB',  3,   44),
                ( 3, 'Germany',         'DE',  2,   49),
                ( 4, 'France',          'FR',  2,   33),
                ( 5, 'Japan',           'JP',  4,   81),
                ( 6, 'China',           'CN',  5,   86),
                ( 7, 'Canada',          'CA',  1,    1),
                ( 8, 'Australia',       'AU',  1,   61),
                ( 9, 'Netherlands',     'NL',  2,   31),
                (10, 'Singapore',       'SG',  1,   65),
                (11, 'Switzerland',     'CH',  3,   41),
                (12, 'Sweden',          'SE',  2,   46),
                (13, 'Spain',           'ES',  2,   34),
                (14, 'Italy',           'IT',  2,   39),
                (15, 'Brazil',          'BR',  NULL, 55)
            ON CONFLICT (id) DO NOTHING
        )");
        txn.exec("SELECT setval('res_country_id_seq', 15, true)");
        txn.commit();
    }
};

} // namespace odoo::modules::base
