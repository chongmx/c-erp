#pragma once
// =============================================================
// modules/sale/SaleModule.hpp
//
// Phase 9 — Sales module
//
// Models:  sale.order      (table: sale_order)
//          sale.order.line (table: sale_order_line)
//
// State machine: draft → sale → cancel
// ViewModels:
//   SaleOrderViewModel  — action_confirm, action_cancel,
//                         action_create_invoices
//   SaleOrderLineViewModel — recomputes line amounts on
//                            create/write; updates parent order
// Seeds: ir_act_window id=11, ir_ui_menu id=60 (Sales app),
//        id=61 (Sales Orders leaf)
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::sale {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ================================================================
// MODULE
// ================================================================

class SaleModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "sale"; }

    explicit SaleModule(ModelFactory&     modelFactory,
                        ServiceFactory&   serviceFactory,
                        ViewModelFactory& viewModelFactory,
                        ViewFactory&      viewFactory);

    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerRoutes()     override;
    void registerViews()      override;
    void registerViewModels() override;
    void initialize()         override;

private:
    ModelFactory&     models_;
    ServiceFactory&   services_;
    ViewModelFactory& viewModels_;
    ViewFactory&      views_;

    void ensureSchema_();
    void seedMenus_();
};

} // namespace odoo::modules::sale
