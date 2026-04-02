#pragma once
// =============================================================
// modules/ir/IrModule.hpp  — declaration only
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::infrastructure { class MigrationRunner; }

namespace odoo::modules::ir {

class IrModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "ir"; }

    explicit IrModule(core::ModelFactory&     modelFactory,
                      core::ServiceFactory&   serviceFactory,
                      core::ViewModelFactory& viewModelFactory,
                      core::ViewFactory&      viewFactory);

    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViews()      override;
    void registerViewModels() override;
    void registerRoutes()     override;
    void initialize()         override;
    void registerMigrations(odoo::infrastructure::MigrationRunner& runner) override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;

    void ensureSchema_();
    void seedActions_();
    void seedMenus_();
    void seedConfigParams_();
    void seedRules_();

    // CSV import/export helpers
    static std::string         buildExportFilename_(const std::string& model);
    static std::vector<std::string> splitFields_(const std::string& csv);
};

} // namespace odoo::modules::ir
