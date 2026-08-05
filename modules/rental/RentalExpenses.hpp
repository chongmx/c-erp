#pragma once
// =============================================================
// modules/rental/RentalExpenses.hpp — recurring expenses (docs/054 phase 7)
//
// A recurring expense is a TEMPLATE row (is_recurring = TRUE) carrying
// the BUDGETED amount — wifi RM 200/month, maintenance RM 500/quarter.
// The cron clones it into dated child rows, which are the ACTUALS and
// are editable when the real bill arrives.
//
// That split is what makes the forecast honest: past months use what was
// actually spent, future months use what was budgeted. Storing one number
// for both would make the forecast quietly rewrite history every time a
// bill came in higher than expected.
//
// Idempotency is the same discipline as billing:
// UNIQUE (recurrence_parent_id, date) means a second cron run cannot
// post the electricity bill twice.
// =============================================================
#include <memory>
#include <string>
#include <vector>

namespace odoo::infrastructure { class DbConnection; }

namespace odoo::modules::rental {

struct ExpenseGenResult {
    int generated = 0;
    int skipped   = 0;   ///< already existed for that date
    int failed    = 0;
    std::vector<std::string> errors;
};

class RentalExpenses {
public:
    /**
     * Generate the dated children due on or before `asOf`.
     *
     * Catches up: a template whose next date is three months in the past
     * produces three children, not one. A cron that was down for a week
     * must not silently lose a month's expense.
     */
    static ExpenseGenResult generate(std::shared_ptr<infrastructure::DbConnection> db,
                                     const std::string& asOf = "");

    /// Register the cron handler and activate the job.
    static void registerCron(std::shared_ptr<infrastructure::DbConnection> db);
};

} // namespace odoo::modules::rental
