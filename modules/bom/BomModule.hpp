#pragma once
// =============================================================
// modules/bom/BomModule.hpp
//
// docs/107 — the BOM editor and its importer.
//
// Extends mrp.bom / mrp.bom.line with what a PCBA needs (docs/105 Phase 1) and
// adds a staged importer in front of them.
//
// Models / ViewModels:
//   bom.editor  — one BOM's lines, with per-line status
//   bom.import  — parse → resolve → review → commit, staged in mrp_bom_import_line
//
// Menus:  id=150 BOM Editor (under Manufacturing)
// Actions: id=116 BOM Editor
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::bom {

class BomModule : public core::IModule {
public:
    explicit BomModule(core::ModelFactory&, core::ServiceFactory&,
                       core::ViewModelFactory&, core::ViewFactory&);

    static constexpr const char* staticModuleName() { return "bom"; }
    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

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

} // namespace cerp::modules::bom
