#pragma once
// =============================================================
// modules/mrp/MrpModule.hpp
//
// Phase 26 — Manufacturing / Bill of Materials
//
// Models:
//   mrp.bom       (table: mrp_bom)       — BOM header
//   mrp.bom.line  (table: mrp_bom_line)  — BOM component lines
//
// ViewModels:
//   MrpBomViewModel     — CRUD + getBomLines
//   MrpBomLineViewModel — CRUD
//
// Menus:
//   id=100  Manufacturing app tile
//   id=101  Products section (under Manufacturing)
//   id=102  Bills of Materials leaf
//
// Actions:
//   id=34  Bills of Materials  (mrp.bom,  list,form)
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::mrp {

class MrpModule : public core::IModule {
public:
    explicit MrpModule(core::ModelFactory&     models,
                       core::ServiceFactory&   services,
                       core::ViewModelFactory& viewModels,
                       core::ViewFactory&      views);

    static constexpr const char* staticModuleName() { return "mrp"; }
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
    void seedMenus_();
};

} // namespace odoo::modules::mrp
