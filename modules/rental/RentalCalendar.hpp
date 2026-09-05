#pragma once
// =============================================================
// modules/rental/RentalCalendar.hpp — day-level occupancy, and booking it
//
// The Booking screen asks two questions and this answers both:
//
//   "which days is each unit let?"   -> month()
//   "let this unit for these days"   -> book()
//
// NO NEW TABLE. Occupancy is not stored anywhere; it is derived from the
// contract lines that already exist:
//
//     a unit is occupied on day D  <=>  a live line (pending|active) exists
//                                       with date_start <= D
//                                       and (date_end IS NULL or D <= date_end)
//
// Storing it as well would create a second source of truth that drifts from
// billing the first time someone edits a line — and billing already reads
// these same dates (RentalBilling.cpp), so anything the calendar shows is
// exactly what will be invoiced.
//
// date_end is INCLUSIVE: it is the last day of the let, which is how the
// billing run treats it, so the next booking starts the day after.
//
// The whole month comes back in ONE query and is folded into day arrays in
// C++, rather than a generate_series cross join per unit. A facility with 400
// units and a 31-day month is 12,400 cells; the join returns one row per
// overlapping LINE, which is a far smaller number.
// =============================================================
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace cerp::infrastructure { class DbConnection; }

namespace cerp::modules::rental {

class RentalCalendar {
public:
    /**
     * One month of occupancy, by unit and by type.
     *
     * @param ym        "YYYY-MM". Anything unparseable falls back to today's
     *                  month rather than erroring — a calendar that refuses to
     *                  draw is worse than one showing the wrong month.
     * @param companyId the active company (docs/094). 0 means no scoping,
     *                  which is only reachable from an internal caller.
     * @param typeId    optional filter to one unit type; 0 for all.
     *
     * Returns { month, days, from, to, totals, types[], units[] } where each
     * unit carries a `days` array of 0/1 the length of the month, its let-day
     * count, and the bookings that produced them.
     */
    static nlohmann::json month(std::shared_ptr<infrastructure::DbConnection> db,
                                const std::string& ym,
                                int companyId,
                                int typeId = 0);

    /// What book() was asked to do. Kept as a struct so the route stays a
    /// parameter-shuffling exercise and the rules live here.
    struct BookRequest {
        int         unitId    = 0;
        int         partnerId = 0;
        std::string dateStart;              ///< YYYY-MM-DD, required
        std::string dateEnd;                ///< YYYY-MM-DD, empty = open-ended
        double      unitPrice = -1.0;       ///< <0 = take the unit type's rate
        int         contractId = 0;         ///< 0 = create a contract
        std::string billingMode;            ///< empty = oneoff when dated
        int         companyId = 0;
        int         uid       = 0;
    };

    /**
     * Let a unit for a period, as one contract line.
     *
     * Reuses the contract when one is given and belongs to the same customer;
     * otherwise opens a new one, because a booking with no contract has
     * nowhere to hang its billing terms.
     *
     * Overlap is checked HERE as well as in the database (migration 820), so
     * the operator is told which dates clash instead of meeting a constraint.
     * The database check is the one that survives a race; this one is the one
     * that is readable.
     *
     * @return { ok, line_id, contract_id, unit, from, to } on success.
     * @throws ValidationError with a message meant for the screen.
     */
    static nlohmann::json book(std::shared_ptr<infrastructure::DbConnection> db,
                               const BookRequest& req);
};

} // namespace cerp::modules::rental
