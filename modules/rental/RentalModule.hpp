#pragma once
// =============================================================
// modules/rental/RentalModule.hpp
//
// Warehouse storage rental — lockers and rooms let to customers,
// billed monthly IN ADVANCE, open-ended until cancellation.
// (docs/054; data model from docs/040 §3)
//
// Models
//   rental.unit.type        Small Locker / Room / Pallet Space …
//   rental.unit             the physical lettable space
//   rental.contract         the customer agreement
//   rental.contract.line    one per rented unit — per-unit dates live HERE
//   rental.invoice.link     what a generated invoice covers + idempotency
//   rental.expense.category
//   rental.expense          one-off and recurring
//   rental.event            domain event log (separate from audit_log)
//
// Schema lives in RentalMigrations.cpp (versions 800–810), not in an
// ensureSchema_() here: these tables carry constraints that must be
// created exactly once, in order, and be recorded as applied.
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::infrastructure { class MigrationRunner; }

namespace cerp::modules::rental {

class RentalModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "rental"; }

    explicit RentalModule(core::ModelFactory&     models,
                          core::ServiceFactory&   services,
                          core::ViewModelFactory& viewModels,
                          core::ViewFactory&      views);

    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViewModels() override;
    void registerViews()      override;
    void registerRoutes()     override;
    void registerMigrations(cerp::infrastructure::MigrationRunner& runner) override;
    void initialize()         override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void seedActions_();
    void seedMenus_();
};

} // namespace cerp::modules::rental
