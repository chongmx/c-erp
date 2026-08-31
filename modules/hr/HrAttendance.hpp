#pragma once
// =============================================================
// modules/hr/HrAttendance.hpp — who is at work, and for how long (docs/113 §1)
//
// Attendance is a pair of timestamps and one derived number, which makes it
// look trivial. It is not, because the interesting part is what must be
// IMPOSSIBLE rather than what is computed:
//
//   * an employee must never have two open attendances — two taps on a kiosk
//     half a second apart must produce one record, not two. That is a partial
//     unique index, not a handler check: a handler is one code path and a
//     constraint is all of them, including the race between two requests.
//   * worked_hours is derived from the stored timestamps on the server. A
//     client that lies about the duration changes nothing.
//   * a closed interval must not overlap another closed interval for the same
//     employee — a range comparison a unique index cannot express, so it is
//     checked on the way out.
//
// The declaration stays slim (PERF-E): the model and viewmodel classes, the
// heavy includes and every method body live in the .cpp.
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

class HrAttendance {
public:
    /// CREATE TABLE + the three guards. Idempotent; called from HrModule::initialize().
    static void ensureSchema(pqxx::transaction_base& txn);

    static void registerModels(core::ModelFactory& models,
                               std::shared_ptr<infrastructure::DbConnection> db);
    static void registerViewModels(core::ViewModelFactory& viewModels,
                                   std::shared_ptr<infrastructure::DbConnection> db);

    /// Menus + actions (ids 404/405, actions 118/119). Idempotent.
    static void seedMenus(pqxx::transaction_base& txn);
};

} // namespace cerp::modules::hr
