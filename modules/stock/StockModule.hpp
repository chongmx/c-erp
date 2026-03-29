#pragma once
// =============================================================
// modules/stock/StockModule.hpp  — declaration only
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::stock {

class StockModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "stock"; }

    explicit StockModule(core::ModelFactory&     modelFactory,
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
    void seedLocations_();
    void seedPickingTypes_();
    void seedWarehouses_();
    void seedMenus_();
};

} // namespace odoo::modules::stock
