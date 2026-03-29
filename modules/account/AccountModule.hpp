#pragma once
// =============================================================
// modules/account/AccountModule.hpp  — declaration only
//
// Phase 6 — minimal double-entry bookkeeping module.
//
// Models:  account.account, account.journal, account.tax,
//          account.move, account.move.line,
//          account.payment, account.payment.term
//
// initialize() creates tables, seeds a minimal chart of accounts,
// four journals (SAL/PUR/BNK/CSH), two taxes, two payment terms,
// and four ir_act_window / ir_ui_menu entries (idempotent).
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::account {

// ================================================================
// 3. MODULE
// ================================================================

class AccountModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "account"; }

    explicit AccountModule(core::ModelFactory&     modelFactory,
                           core::ServiceFactory&   serviceFactory,
                           core::ViewModelFactory& viewModelFactory,
                           core::ViewFactory&      viewFactory);

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
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_();
    void seedChartOfAccounts_();
    void seedJournals_();
    void seedTaxes_();
    void seedPaymentTerms_();
    void seedMenus_();
};

} // namespace odoo::modules::account
