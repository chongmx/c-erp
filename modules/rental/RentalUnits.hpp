#pragma once
// =============================================================
// modules/rental/RentalUnits.hpp — unit state (docs/054 phase 3)
//
// The STATE itself is derived by a database trigger (migration 811), not
// here. That is deliberate: derived state must not depend on which code
// path wrote the contract line, and a trigger cannot be forgotten by a
// future caller the way a helper function can.
//
// What lives here is what the database cannot do:
//
//   * emit the domain events, which need the acting user
//   * expose a repair/reconcile entry point for maintenance and tests
//   * the maintenance open/close actions, which are operator facts rather
//     than consequences of a contract
// =============================================================
#include <string>
#include <vector>

namespace pqxx { class transaction_base; }

namespace odoo::modules::rental {

struct EventCtx;

class RentalUnits {
public:
    /// Current state of one unit, or "" if it does not exist.
    static std::string stateOf(pqxx::transaction_base& txn, int unitId);

    /**
     * Ask the database to recompute one unit's state.
     *
     * Normally unnecessary — the trigger has already run. Used by repair
     * tooling and by tests that want to assert the derivation in
     * isolation from a line write.
     */
    static void recompute(pqxx::transaction_base& txn, int unitId);

    /// Recompute every unit. Returns how many changed.
    static int recomputeAll(pqxx::transaction_base& txn);

    /**
     * Emit unit_assigned / unit_released for a state transition.
     *
     * Called by the contract lifecycle after a line write, because the
     * trigger has the state but not the user. Emits nothing when the
     * state did not actually change, so a no-op edit does not litter the
     * activity feed.
     */
    static void emitTransition(pqxx::transaction_base& txn,
                               int                     unitId,
                               const std::string&      before,
                               const std::string&      after,
                               const EventCtx&         ctx);

    /**
     * Take a unit out of service, or return it.
     *
     * An operator fact: the trigger never overwrites maintenance or
     * retired, so this is the only way in or out of those states.
     * Returning to service re-derives from the contract lines rather than
     * assuming "available" — the unit may have been let in the meantime.
     */
    static void openMaintenance (pqxx::transaction_base& txn, int unitId,
                                 const std::string& reason, const EventCtx& ctx);
    static void closeMaintenance(pqxx::transaction_base& txn, int unitId,
                                 const EventCtx& ctx);
};

} // namespace odoo::modules::rental
