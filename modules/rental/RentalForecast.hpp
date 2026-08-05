#pragma once
// =============================================================
// modules/rental/RentalForecast.hpp — cashflow forecast
//
// Month-by-month expected cash in and out.
//
// NOTHING here is stored. A forecast is derived on every request from
// the tenancies, the recurring-expense templates and the open invoices.
// Storing it would mean a number that was right when it was written and
// silently wrong from the next rate change onward — and a stale forecast
// is worse than none, because it is trusted.
//
// Income has two distinct sources, and keeping them apart is what stops
// double counting:
//
//   receivable  already invoiced, not yet paid -> counted in the month it
//               falls DUE
//   projected   not yet invoiced -> counted from next_period_start, which
//               already points past everything invoiced
//
// Because billing is IN ADVANCE, next_period_start is always the first
// unbilled period, so the two sets cannot overlap.
// =============================================================
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace odoo::infrastructure { class DbConnection; }

namespace odoo::modules::rental {

class RentalForecast {
public:
    /**
     * @param months how far ahead, clamped to 1..36
     * @param from   first month, empty means the current one
     */
    static nlohmann::json cashflow(std::shared_ptr<infrastructure::DbConnection> db,
                                   int months = 12,
                                   const std::string& from = "");
};

} // namespace odoo::modules::rental
