#pragma once
// =============================================================
// modules/hr/HrLeave.hpp — leave types, allocations and requests (docs/113 §2)
//
// Four tables and one number that has to be right: the BALANCE. Everything
// here exists to make that number impossible to corrupt.
//
//   * number_of_days counts WORKING days — weekends and public holidays are
//     excluded, server-side. A request across a weekend must not burn weekend
//     days, and a client-supplied day count is never trusted.
//   * an employee cannot hold two approved leaves over the same dates.
//   * when a type requires an allocation, approving beyond
//     (approved allocations − already approved leave) is refused, so the
//     balance can never go negative.
//   * every transition is guarded: approve only from `confirm`, no
//     double-approve, no approving something refused or cancelled, and
//     cancelling an approved leave returns the days.
//
// Slim declaration (PERF-E); classes and bodies live in the .cpp.
// =============================================================
#include <memory>
#include <string>

namespace cerp::core {
class ModelFactory;
class ViewModelFactory;
}
namespace cerp::infrastructure { class DbConnection; }
namespace pqxx { class transaction_base; }

namespace cerp::modules::hr {

class HrLeave {
public:
    /// Tables, constraints and indexes. Idempotent.
    static void ensureSchema(pqxx::transaction_base& txn);

    /// The four default leave types. Public holidays are deliberately NOT
    /// seeded — see docs/113.
    static void seedDefaults(pqxx::transaction_base& txn);

    static void registerModels(core::ModelFactory& models,
                               std::shared_ptr<infrastructure::DbConnection> db);
    static void registerViewModels(core::ViewModelFactory& viewModels,
                                   std::shared_ptr<infrastructure::DbConnection> db);

    /// Menus 405-408, actions 119-122. Idempotent.
    static void seedMenus(pqxx::transaction_base& txn);

    /**
     * Working days between two dates inclusive, excluding Sat/Sun and any
     * hr_public_holiday row for the company.
     *
     * Exposed for tests: the day counter is the single most consequential
     * calculation in this module, and it deserves to be assertable without
     * going through a request's whole lifecycle.
     */
    static double workingDays(pqxx::transaction_base& txn,
                              const std::string& dateFrom,
                              const std::string& dateTo,
                              int companyId);
};

} // namespace cerp::modules::hr
