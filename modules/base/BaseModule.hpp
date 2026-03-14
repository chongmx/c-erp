#pragma once
// =============================================================
// modules/base/BaseModule.hpp
//
// All includes use flat paths matching the user's project layout:
//   core/services/   → infrastructure (DbConnection, etc.)
//   modules/base/    → base classes + ORM types (flat, no subdirs)
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
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::base {

// ================================================================
// 1. MODEL — ResPartner
// ================================================================
class ResPartner : public core::BaseModel<ResPartner> {
public:
    ODOO_MODEL("res.partner", "res_partner")

    std::string name;
    std::string email;
    std::string phone;
    bool        isCompany = false;
    int         companyId = 0;

    explicit ResPartner(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResPartner>(std::move(db)) {}

    // PUBLIC — required for CRTP static_cast from BaseModel<ResPartner>
    void registerFields() override {
        fieldRegistry_.add({ "name",       core::FieldType::Char,    "Name",       true });
        fieldRegistry_.add({ "email",      core::FieldType::Char,    "Email" });
        fieldRegistry_.add({ "phone",      core::FieldType::Char,    "Phone" });
        fieldRegistry_.add({ "is_company", core::FieldType::Boolean, "Is Company" });
        fieldRegistry_.add({ "company_id", core::FieldType::Many2one,"Company",
                             false, false, true, false, "res.company" });
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["email"]      = email;
        j["phone"]      = phone;
        j["is_company"] = isCompany;
        j["company_id"] = companyId > 0
                          ? nlohmann::json{companyId, "Company"}
                          : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")       && j["name"].is_string())
            name      = j["name"].get<std::string>();
        if (j.contains("email")      && j["email"].is_string())
            email     = j["email"].get<std::string>();
        if (j.contains("phone")      && j["phone"].is_string())
            phone     = j["phone"].get<std::string>();
        if (j.contains("is_company") && j["is_company"].is_boolean())
            isCompany = j["is_company"].get<bool>();
        if (j.contains("company_id") && j["company_id"].is_number_integer())
            companyId = j["company_id"].get<int>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Name is required");
        return errors;
    }
};


// ================================================================
// 2. SERVICE — PartnerService
// ================================================================
class PartnerService : public core::BaseService {
public:
    explicit PartnerService(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseService(std::move(db)) {}

    std::string serviceName() const override { return "partner"; }

    nlohmann::json searchRead(const nlohmann::json&           domain,
                               const std::vector<std::string>& fields = {},
                               int limit = 80, int offset = 0,
                               const std::string& order = "id ASC") {
        ResPartner proto(db_);
        return proto.searchRead(domain, fields, limit, offset, order);
    }

    nlohmann::json read(const std::vector<int>&         ids,
                        const std::vector<std::string>& fields = {}) {
        ResPartner proto(db_);
        return proto.read(ids, fields);
    }

    int create(const nlohmann::json& values) {
        ResPartner rec(db_);
        return rec.create(values);
    }

    bool write(const std::vector<int>& ids, const nlohmann::json& values) {
        ResPartner proto(db_);
        return proto.write(ids, values);
    }

    bool unlink(const std::vector<int>& ids) {
        ResPartner proto(db_);
        return proto.unlink(ids);
    }

    nlohmann::json fieldsGet(const std::vector<std::string>& fields     = {},
                              const std::vector<std::string>& attributes = {}) {
        ResPartner proto(db_);
        return proto.fieldsGet(fields, attributes);
    }

    int searchCount(const nlohmann::json& domain) {
        ResPartner proto(db_);
        return proto.searchCount(domain);
    }
};


// ================================================================
// 3. VIEW — PartnerFormView
// ================================================================
class PartnerFormView : public core::BaseView {
public:
    std::string viewName() const override { return "res.partner.form"; }

    std::string arch() const override {
        return "<form>"
               "<field name=\"name\"/>"
               "<field name=\"email\"/>"
               "<field name=\"phone\"/>"
               "<field name=\"is_company\"/>"
               "</form>";
    }

    nlohmann::json fields() const override {
        return {
            {"name",       {{"type","char"},    {"string","Name"},       {"required",true}}},
            {"email",      {{"type","char"},    {"string","Email"}}},
            {"phone",      {{"type","char"},    {"string","Phone"}}},
            {"is_company", {{"type","boolean"}, {"string","Is Company"}}},
            {"company_id", {{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {
            {"arch",   arch()},
            {"fields", fields()},
            {"record", record},
        };
    }
};


// ================================================================
// 4. VIEWMODEL — PartnerViewModel
// ================================================================
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
        const int limit  = call.limit() > 0 ? call.limit() : 80;
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
        const auto values = call.arg(0);
        if (!values.is_object())
            throw std::runtime_error("create: args[0] must be a dict");
        return service_->create(values);
    }

    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        const auto values = call.arg(1);
        if (!values.is_object())
            throw std::runtime_error("write: args[1] must be a dict");
        return service_->write(call.ids(), values);
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
// 5. MODULE — BaseModule
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
    std::string              version()      const override { return "17.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {}; }

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("res.partner", [db] {
            return std::make_shared<ResPartner>(db);
        });
    }

    void registerServices() override {
        auto db = services_.db();
        services_.registerCreator("partner", [db] {
            return std::make_shared<PartnerService>(db);
        });
    }

    void registerViews() override {
        views_.registerView<PartnerFormView>("res.partner.form");
    }

    void registerViewModels() override {
        auto& sf = services_;
        auto& vf = views_;
        viewModels_.registerCreator("res.partner", [&sf, &vf] {
            auto svc = std::static_pointer_cast<PartnerService>(
                sf.create("partner", core::Lifetime::Singleton));
            auto vfPtr = std::shared_ptr<core::ViewFactory>(&vf, [](auto*){});
            return std::make_shared<PartnerViewModel>(svc, vfPtr);
        });
    }

    void registerRoutes() override {}

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;
};

} // namespace odoo::modules::base