#pragma once
// =============================================================
// modules/rental/RentalEvents.hpp — the domain event log (docs/054 phase 2)
//
// Deliberately SEPARATE from audit_log. audit_log answers "who wrote
// which row" — CRUD forensics. This answers "what happened to the
// business" — contract activated, unit released, invoice generated,
// deposit refunded. Conflating them yields a log that is bad at both,
// and the dashboard's activity feed reads this one.
//
// Built now rather than last, even though its UI comes last, because
// every phase after this emits into it. Threading emit() calls back
// through finished code is where events get missed — the person adding
// them is no longer the person who knew which branches mattered.
//
// emit() takes the CALLER'S transaction, never its own connection, so an
// event cannot commit when the thing it describes rolled back.
// =============================================================
#include <nlohmann/json.hpp>
#include <string>

namespace pqxx { class transaction_base; }

namespace cerp::modules::rental {

/// Optional context for an event. Zero means "not applicable".
struct EventCtx {
    int contractId = 0;
    int lineId     = 0;
    int unitId     = 0;
    int partnerId  = 0;
    int userId     = 0;
    int companyId  = 1;
    std::string refModel;
    int refId      = 0;
};

// The vocabulary. String literals rather than an enum because the column
// is TEXT and the dashboard filters on it; a constant keeps the spelling
// honest at the call sites.
namespace evt {
inline constexpr const char* kContractCreated    = "contract_created";
inline constexpr const char* kContractActivated  = "contract_activated";
inline constexpr const char* kContractCancelled  = "contract_cancelled";
inline constexpr const char* kLineAdded          = "line_added";
inline constexpr const char* kLineStarted        = "line_started";
inline constexpr const char* kLineEnded          = "line_ended";
inline constexpr const char* kUnitAssigned       = "unit_assigned";
inline constexpr const char* kUnitReleased       = "unit_released";
inline constexpr const char* kRateChanged        = "rate_changed";
inline constexpr const char* kInvoiceGenerated   = "invoice_generated";
inline constexpr const char* kPaymentReceived    = "payment_received";
inline constexpr const char* kPaymentApplied     = "payment_applied";
inline constexpr const char* kInvoiceOverdue     = "invoice_overdue";
inline constexpr const char* kDepositHeld        = "deposit_held";
inline constexpr const char* kDepositRefunded    = "deposit_refunded";
inline constexpr const char* kDepositForfeited   = "deposit_forfeited";
inline constexpr const char* kMaintenanceOpened  = "maintenance_opened";
inline constexpr const char* kMaintenanceClosed  = "maintenance_closed";
inline constexpr const char* kExpenseGenerated   = "expense_generated";
} // namespace evt

class RentalEvents {
public:
    /**
     * Append one event, inside the caller's transaction.
     *
     * @param txn     the caller's transaction — NOT a fresh connection
     * @param type    one of evt::k*
     * @param summary human-readable one-liner for the activity feed
     * @param detail  structured payload; null writes SQL NULL
     */
    static void emit(pqxx::transaction_base& txn,
                     const std::string&      type,
                     const EventCtx&         ctx,
                     const std::string&      summary,
                     const nlohmann::json&   detail = nullptr);
};

} // namespace cerp::modules::rental
