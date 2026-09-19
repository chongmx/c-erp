#pragma once
// =============================================================
// modules/api/ApiModule.hpp — API keys and the REST API (/api/v1).
//
// A script, a CI job or an AI agent cannot log in with a password, and should
// not hold one. It holds an API KEY instead: created by a signed-in user under
// Settings → Users & Access → API Keys, shown once, stored only as a SHA-256
// hash, and limited by SCOPES (what it may do) and optionally by PROJECT
// (where). Everything done with a key is done AS its owner — record rules, the
// company boundary and comment authorship behave exactly as they do for that
// person in the browser, because the REST routes call the same view models
// the screens do.
//
// A key authenticates /api/v1/* only. It is never accepted by the JSON-RPC
// endpoint, so it can reach nothing the scopes do not name — and it can never
// mint another key: keys are created and revoked over JSON-RPC, which needs a
// real session.
//
// The first area is the issue tracker (docs/reference/api-v1.md). Other areas
// add their own scopes to kScopes and their own routes here.
//
// Migrations 1200-1299.  Menu 87 / action 129 (Settings → Users & Access).
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::api {

class ApiModule : public core::IModule {
public:
    explicit ApiModule(core::ModelFactory&     models,
                       core::ServiceFactory&   services,
                       core::ViewModelFactory& viewModels,
                       core::ViewFactory&      views);

    static constexpr const char* staticModuleName() { return "api"; }
    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViewModels() override;
    void registerViews()      override;
    void registerRoutes()     override;
    void initialize()         override;
    void registerMigrations(cerp::infrastructure::MigrationRunner& runner) override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void seedMenus_();
};

} // namespace cerp::modules::api
