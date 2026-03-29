#pragma once
// =============================================================
// modules/base/BaseModule.hpp  — declaration only
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::base {

// ================================================================
// 5. MODULE
// ================================================================
class BaseModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "base"; }

    explicit BaseModule(core::ModelFactory&     modelFactory,
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

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_();
    void seedLang_();
    void seedCurrencies_();
    void seedCountries_();
};

} // namespace odoo::modules::base
