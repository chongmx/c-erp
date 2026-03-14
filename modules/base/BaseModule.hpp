#pragma once
// =============================================================
// Example: res.partner — shows how a full module is wired up
// =============================================================
// File layout:
//   modules/base/models/ResPartner.hpp
//   modules/base/services/PartnerService.hpp
//   modules/base/viewmodels/PartnerViewModel.hpp
//   modules/base/views/PartnerFormView.hpp
//   modules/base/BaseModule.hpp
// =============================================================

#include "BaseModel.hpp"
#include "BaseService.hpp"
#include "BaseViewModel.hpp"
#include "BaseView.hpp"
#include "IModule.hpp"
#include "Container.hpp"
#include <string>

namespace odoo::modules::base {

// -------------------------------------------------------
// 1. MODEL
// -------------------------------------------------------
class ResPartner : public core::BaseModel<ResPartner> {
    ODOO_MODEL("res.partner", "res_partner")
public:
    std::string name;
    std::string email;
    std::string phone;
    bool        isCompany = false;
    int         companyId = 0;

    explicit ResPartner(std::shared_ptr<core::DbConnection> db)
        : core::BaseModel<ResPartner>(db)
    {
        modelName_   = MODEL_NAME;
        tableName_   = TABLE_NAME;
        displayName_ = name;
        registerFields();
    }

    void registerFields() override {
        using namespace odoo::core;
        fieldRegistry_.add({ "name",       FieldType::Char,    "Name",       true });
        fieldRegistry_.add({ "email",      FieldType::Char,    "Email" });
        fieldRegistry_.add({ "phone",      FieldType::Char,    "Phone" });
        fieldRegistry_.add({ "is_company", FieldType::Boolean, "Is Company" });
        fieldRegistry_.add({ "company_id", FieldType::Many2one,"Company",
                             false, false, true, false, "res.company" });
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Name is required");
        return errors;
    }

protected:
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
        if (j.contains("name"))       name      = j["name"].get<std::string>();
        if (j.contains("email"))      email     = j["email"].get<std::string>();
        if (j.contains("phone"))      phone     = j["phone"].get<std::string>();
        if (j.contains("is_company")) isCompany = j["is_company"].get<bool>();
    }
};


// -------------------------------------------------------
// 2. SERVICE
// -------------------------------------------------------
class PartnerService : public core::BaseService {
public:
    explicit PartnerService(std::shared_ptr<core::DbConnection> db)
        : core::BaseService(db) {}

    std::string serviceName() const override { return "partner"; }

    // Example business method
    std::shared_ptr<ResPartner> findByEmail(const std::string& email) {
        // auto rs = ResPartner::search(db_, {{"email","=",email}}, 1);
        // return rs.empty() ? nullptr : rs[0];
        return nullptr; // stub
    }

    std::vector<std::shared_ptr<ResPartner>> searchPartners(
        const core::Domain& domain, int limit = 80, int offset = 0
    ) {
        // return ResPartner::search(db_, domain, limit, offset).records;
        return {}; // stub
    }
};


// -------------------------------------------------------
// 3. VIEW
// -------------------------------------------------------
class PartnerFormView : public core::BaseView<ResPartner> {
public:
    std::string viewName() const override { return "res.partner.form"; }

    nlohmann::json render(const ResPartner& p) const override {
        return {
            {"id",         p.getId()},
            {"name",       p.name},
            {"email",      p.email},
            {"phone",      p.phone},
            {"is_company", p.isCompany},
            {"display_name", p.name}
        };
    }
};


// -------------------------------------------------------
// 4. VIEWMODEL
// -------------------------------------------------------
class PartnerViewModel : public core::BaseViewModel {
public:
    explicit PartnerViewModel(
        std::shared_ptr<PartnerService> service,
        std::shared_ptr<core::ViewFactory> viewFactory
    )
        : service_(std::move(service))
        , viewFactory_(std::move(viewFactory))
    {
        // Register all handled JSON-RPC methods
        REGISTER_METHOD("search_read", handleSearchRead)
        REGISTER_METHOD("read",        handleRead)
        REGISTER_METHOD("create",      handleCreate)
        REGISTER_METHOD("write",       handleWrite)
        REGISTER_METHOD("unlink",      handleUnlink)
        REGISTER_METHOD("fields_get",  handleFieldsGet)
    }

    std::string viewModelName() const override { return "res.partner"; }

private:
    std::shared_ptr<PartnerService>    service_;
    std::shared_ptr<core::ViewFactory> viewFactory_;

    nlohmann::json handleSearchRead(
        const nlohmann::json& args,
        const nlohmann::json& kwargs,
        const core::RpcContext& ctx
    ) {
        auto domain  = core::domainFromJson(
            args.size() > 0 ? args[0] : nlohmann::json::array());
        int  limit   = kwargs.value("limit",  80);
        int  offset  = kwargs.value("offset", 0);

        auto records = service_->searchPartners(domain, limit, offset);
        auto view    = viewFactory_->getView("res.partner", "form");

        nlohmann::json result = nlohmann::json::array();
        for (const auto& r : records)
            result.push_back(view->renderJson(r->toJson()));
        return result;
    }

    nlohmann::json handleRead(
        const nlohmann::json& args,
        const nlohmann::json& kwargs,
        const core::RpcContext&
    ) {
        // args[0] = list of IDs
        // stub
        return nlohmann::json::array();
    }

    nlohmann::json handleCreate(
        const nlohmann::json& args,
        const nlohmann::json& kwargs,
        const core::RpcContext& ctx
    ) {
        // args[0] = vals dict — stub
        return 0;
    }

    nlohmann::json handleWrite(
        const nlohmann::json& args,
        const nlohmann::json& kwargs,
        const core::RpcContext&
    ) {
        return true;
    }

    nlohmann::json handleUnlink(
        const nlohmann::json& args,
        const nlohmann::json& kwargs,
        const core::RpcContext&
    ) {
        return true;
    }

    nlohmann::json handleFieldsGet(
        const nlohmann::json&,
        const nlohmann::json&,
        const core::RpcContext&
    ) {
        ResPartner stub(nullptr);
        return stub.fieldRegistry_.toJson();
    }
};


// -------------------------------------------------------
// 5. MODULE — wires everything into the Container
// -------------------------------------------------------
class BaseModule : public core::IModule {
public:
    explicit BaseModule(std::shared_ptr<odoo::Container> container)
        : container_(std::move(container)) {}

    std::string              moduleName()   const override { return "base"; }
    std::string              version()      const override { return "17.0.1"; }
    std::vector<std::string> dependencies() const override { return {}; }

    void registerModels() override {
        container_->modelFactory->registerCreator("res.partner", [this] {
            return std::make_shared<ResPartner>(container_->db);
        });
    }

    void registerServices() override {
        container_->serviceFactory->registerCreator("partner", [this] {
            return std::make_shared<PartnerService>(container_->db);
        });
    }

    void registerViews() override {
        container_->viewFactory->registerView<ResPartner, PartnerFormView>(
            "res.partner.form",
            [] { return std::make_shared<PartnerFormView>(); }
        );
    }

    void registerViewModels() override {
        container_->viewModelFactory->registerCreator("res.partner", [this] {
            auto svc  = std::static_pointer_cast<PartnerService>(
                container_->serviceFactory->create("partner", core::Lifetime::Singleton)
            );
            return std::make_shared<PartnerViewModel>(svc, container_->viewFactory);
        });
    }

    void registerRoutes() override {
        // Base module has no extra HTTP routes beyond JSON-RPC
    }

private:
    std::shared_ptr<odoo::Container> container_;
};

} // namespace odoo::modules::base
