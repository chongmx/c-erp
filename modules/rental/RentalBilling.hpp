#pragma once
// =============================================================
// modules/rental/RentalBilling.hpp — recurring billing (docs/054 phase 5)
//
// Bills IN ADVANCE: the invoice for a period is raised
// `billing_lead_days` before the period starts, so the tenant has time to
// pay before occupying. That is also why cancellation needs no proration
// — they have already paid for the period they are sitting in.
//
// ONE code path for the cron and the manual "Generate invoices now"
// button, differing only by the as-of date. A manual path that drifts
// from the scheduled one is how double-billing gets discovered in
// production.
//
// Idempotency is the database's job, not the scheduler's. ir.cron is
// at-least-once by design, so `UNIQUE (contract_line_id, period_start)`
// on rental_invoice_link is what makes a second run a no-op — see
// verify_rental_billing.sh, which proves it by dropping the constraint
// and watching the double-bill happen.
// =============================================================
#include <memory>
#include <string>
#include <vector>

namespace cerp::infrastructure { class DbConnection; }

namespace cerp::modules::rental {

struct BillingResult {
    int invoicesCreated = 0;
    int linesBilled     = 0;
    int groupsSkipped   = 0;   ///< already billed for that period
    int groupsFailed    = 0;   ///< errored; the run continued
    std::vector<int> moveIds;
    std::vector<std::string> errors;
};

class RentalBilling {
public:
    /**
     * Bill everything due as of a date.
     *
     * @param asOf  billing date; empty means today. The manual action
     *              passes a date, the cron passes none — same function.
     *
     * Each (partner, period) group runs in its OWN transaction, so a
     * failure on one customer cannot half-bill another or abort the run.
     */
    /**
     * @param contractId 0 = everything due (the cron, and the "Generate
     *        invoices now" button). Non-zero = ONE contract, because an
     *        operator asked for it on that contract's form.
     *
     * Asking for one contract also RELAXES two filters, and it is the only
     * thing that may:
     *
     *   * a contract billed `oneoff` or `ondemand` is skipped by the scheduled
     *     run on purpose — "on demand" means nothing happens until somebody
     *     demands it. This IS that demand, so those contracts bill here.
     *   * a line whose billing_mode is oneoff/ondemand likewise. Without this a
     *     dated booking made on the calendar could never be invoiced by
     *     anything at all: it is written billing_mode='oneoff' so the recurring
     *     engine leaves it alone, and nothing else billed it.
     *
     * What it does NOT relax is the period gate or idempotency. A period still
     * has to be within its lead days, and UNIQUE (contract_line_id,
     * period_start) still makes a second press a no-op.
     */
    static BillingResult run(std::shared_ptr<infrastructure::DbConnection> db,
                             const std::string& asOf = "",
                             int contractId = 0);

    /// Register the cron handler and activate the job.
    static void registerCron(std::shared_ptr<infrastructure::DbConnection> db);
};

} // namespace cerp::modules::rental
