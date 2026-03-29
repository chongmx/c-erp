#pragma once
// =============================================================
// modules/uom/UomModule.hpp
//
// Phase 7 — Units of Measure
//
// Model:  uom.uom  (table: uom_uom)
// Seeds:  15 standard UOM rows (Units, kg, L, Hours, m, …)
// Menus:  Creates "Products" app tile (id=50) with a
//         Configuration section (id=52) → Units of Measure leaf (id=53)
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::uom {

// ================================================================
// MODULE
// ================================================================

class UomModule : public core::IModule {
public:
    explicit UomModule(core::ModelFactory&     models,
                       core::ServiceFactory&   services,
                       core::ViewModelFactory& viewModels,
                       core::ViewFactory&      views);

    static constexpr const char* staticModuleName() { return "uom"; }

    std::string moduleName() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViewModels() override;
    void registerViews()      override;
    void registerRoutes()     override;
    void initialize()         override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_();
    void seedUom_();
    void seedMenus_();
};

} // namespace odoo::modules::uom
