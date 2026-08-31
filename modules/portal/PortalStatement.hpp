#pragma once
// =============================================================
// modules/portal/PortalStatement.hpp — statement of account (docs/114 W4)
//
// "What do I owe you, and how did we get there."
//
// Built from the RECEIVABLE LEDGER — account_move_line rows on an
// asset_receivable account — not by adding up invoices and subtracting
// payments. That distinction is the whole design:
//
//   * a statement assembled from invoices and payments is a second opinion
//     about the customer's balance, and second opinions drift. One built from
//     the receivable account IS the ledger, so it agrees with the trial
//     balance by construction and cannot quietly disagree with the books.
//   * it picks up everything that touches the receivable — credit notes,
//     manual journal entries, write-offs, FX adjustments — without anyone
//     remembering to add a case for each.
//
// Only POSTED moves are included. A draft invoice is not a debt.
// =============================================================
#include <nlohmann/json.hpp>
#include <string>

namespace pqxx { class transaction_base; }

namespace cerp::modules::portal {

class PortalStatement {
public:
    /**
     * The statement as data.
     *
     * @param partnerId whose statement — always supplied by the caller from
     *                  the session, never from the request.
     * @param dateFrom  "YYYY-MM-DD"; empty means "since the beginning".
     * @param dateTo    "YYYY-MM-DD"; empty means today.
     *
     * Returns opening balance, the movements in the window with a running
     * balance, the closing balance, and an ageing breakdown of what is still
     * outstanding.
     */
    static nlohmann::json build(pqxx::transaction_base& txn,
                                int partnerId,
                                const std::string& dateFrom,
                                const std::string& dateTo);

    /// The same statement as a printable HTML document.
    static std::string renderHtml(pqxx::transaction_base& txn,
                                  int partnerId,
                                  const std::string& dateFrom,
                                  const std::string& dateTo);

    /// "YYYY-MM-DD" or empty. Anything else is rejected rather than passed to
    /// PostgreSQL to interpret — the dates arrive from a query string.
    static bool isIsoDate(const std::string& s);
};

} // namespace cerp::modules::portal
