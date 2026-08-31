// =============================================================
// modules/rental/RentalUnits.cpp
// =============================================================
#include "RentalUnits.hpp"
#include "RentalEvents.hpp"

#include <pqxx/pqxx>

namespace cerp::modules::rental {

std::string RentalUnits::stateOf(pqxx::transaction_base& txn, int unitId) {
    auto r = txn.exec("SELECT state FROM rental_unit WHERE id = $1",
                      pqxx::params{unitId});
    if (r.empty() || r[0][0].is_null()) return "";
    return r[0][0].c_str();
}

void RentalUnits::recompute(pqxx::transaction_base& txn, int unitId) {
    if (unitId <= 0) return;
    txn.exec("SELECT rental_unit_derive_state($1)", pqxx::params{unitId});
}

int RentalUnits::recomputeAll(pqxx::transaction_base& txn) {
    // Snapshot, recompute, then count what moved. Comparing before and
    // after is the point: a repair run that reports "0 changed" is the
    // evidence that the trigger has been keeping up, which is the thing
    // worth knowing.
    txn.exec("CREATE TEMP TABLE IF NOT EXISTS _ru_before "
             "(id INTEGER PRIMARY KEY, state TEXT) ON COMMIT DROP");
    txn.exec("DELETE FROM _ru_before");
    txn.exec("INSERT INTO _ru_before SELECT id, state FROM rental_unit");
    txn.exec("SELECT rental_unit_derive_state(id) FROM rental_unit");
    auto r = txn.exec(
        "SELECT count(*) FROM rental_unit u JOIN _ru_before b ON b.id = u.id "
        " WHERE u.state IS DISTINCT FROM b.state");
    return r.empty() ? 0 : r[0][0].as<int>(0);
}

void RentalUnits::emitTransition(pqxx::transaction_base& txn,
                                 int                     unitId,
                                 const std::string&      before,
                                 const std::string&      after,
                                 const EventCtx&         ctx) {
    if (unitId <= 0 || before == after) return;

    EventCtx c = ctx;
    c.unitId = unitId;

    auto code = [&]() -> std::string {
        auto r = txn.exec("SELECT code FROM rental_unit WHERE id = $1",
                          pqxx::params{unitId});
        return (r.empty() || r[0][0].is_null()) ? std::to_string(unitId) : r[0][0].c_str();
    }();

    const nlohmann::json detail = {{"from", before}, {"to", after}};

    // "Assigned" and "released" are about occupancy, not about every
    // state change: available -> reserved is an assignment, and anything
    // -> available is a release. A move between occupied and reserved is
    // neither, and gets no event rather than a misleading one.
    if (after == "occupied" || after == "reserved") {
        RentalEvents::emit(txn, evt::kUnitAssigned, c,
                           "Unit " + code + " " + after + " (was " + before + ")", detail);
    } else if (after == "available") {
        RentalEvents::emit(txn, evt::kUnitReleased, c,
                           "Unit " + code + " released (was " + before + ")", detail);
    }
}

void RentalUnits::openMaintenance(pqxx::transaction_base& txn, int unitId,
                                  const std::string& reason, const EventCtx& ctx) {
    const std::string before = stateOf(txn, unitId);
    if (before.empty()) return;

    txn.exec("UPDATE rental_unit SET state = 'maintenance', write_date = now() "
             " WHERE id = $1", pqxx::params{unitId});

    EventCtx c = ctx;
    c.unitId = unitId;
    RentalEvents::emit(txn, evt::kMaintenanceOpened, c,
                       "Unit taken out of service" + (reason.empty() ? "" : ": " + reason),
                       nlohmann::json{{"from", before}, {"reason", reason}});
}

void RentalUnits::closeMaintenance(pqxx::transaction_base& txn, int unitId,
                                   const EventCtx& ctx) {
    const std::string before = stateOf(txn, unitId);
    if (before != "maintenance") return;

    // Drop to 'available' first so the trigger's guard ("never overwrite
    // maintenance or retired") no longer applies, then re-derive. Setting
    // it straight to 'available' would be wrong: the unit may have been
    // let while it was out of service, and would come back claiming to be
    // free.
    txn.exec("UPDATE rental_unit SET state = 'available', write_date = now() "
             " WHERE id = $1", pqxx::params{unitId});
    recompute(txn, unitId);

    const std::string after = stateOf(txn, unitId);
    EventCtx c = ctx;
    c.unitId = unitId;
    RentalEvents::emit(txn, evt::kMaintenanceClosed, c,
                       "Unit returned to service as " + after,
                       nlohmann::json{{"to", after}});
}

} // namespace cerp::modules::rental
