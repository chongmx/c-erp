#pragma once
// ============================================================
// core/PaymentAllocation.hpp
//
// Open-item payment allocation + realised FX. (P1 — docs/045, docs/048 §4.6)
//
// WHAT IT REPLACES
//   One payment settled one invoice by decrementing a scalar
//   amount_residual. That model cannot represent:
//     * one payment across several invoices — the normal rental case,
//       where a tenant pays one transfer for three lockers;
//     * an unallocated credit — paying two months up front, when no
//       invoice exists yet to decrement;
//     * reversing a misapplied payment.
//   Allocations are now rows, and residual is DERIVED from them, so the
//   two cannot disagree.
//
// FX, THE WAY THE BANK ACTUALLY WORKS (docs/048 §4.6)
//   The bank converts incoming foreign currency before it reaches the
//   account, so a USD invoice is settled in MYR. The user therefore does
//   NOT enter a rate — the rate includes the bank's spread and is not a
//   number they know. They enter the MYR that landed, and the effective
//   rate is derived:
//
//     invoice 100 USD booked at 4.70   -> 470.00 MYR receivable
//     bank credits 448.50 MYR          -> effective rate 4.485
//     realised FX loss                 =  -21.50 MYR  -> account 7900
//
//   Computed PER ALLOCATION, so an invoice paid in two instalments at
//   different rates gets the right difference each time.
// ============================================================
#include "Money.hpp"

#include <string>
#include <vector>

namespace pqxx { class transaction_base; }

namespace odoo::core {

/// One application of a payment to an invoice.
struct Allocation {
    int   moveId     = 0;
    Money amount;        ///< in the invoice's currency
    Money amountBase;    ///< same amount in base currency, at settlement rate
    Money fxDiff;        ///< realised FX on this allocation (base currency)
};

struct AllocationResult {
    std::vector<Allocation> applied;
    Money totalApplied;      ///< in payment currency
    Money unallocated;       ///< credit left on the customer
    Money totalFxDiff;       ///< base currency; posts to 7900
};

class PaymentAllocation {
public:
    /**
     * @brief Apply a payment to open invoices, oldest due first.
     *
     * @param paymentId    account_payment row (already posted)
     * @param receivedBase what actually landed in the bank, in BASE currency.
     *                     Zero means "same currency, no conversion" and the
     *                     payment amount is used as-is.
     * @param moveIds      restrict to these invoices; empty = all open for
     *                     the payment's partner.
     *
     * Oldest-due-first is the convention; an explicit moveIds list overrides
     * it for the case where a customer says which invoice they are paying.
     */
    static AllocationResult allocate(pqxx::transaction_base& txn,
                                     int paymentId,
                                     const Money& receivedBase,
                                     const std::vector<int>& moveIds = {});

    /**
     * @brief Undo an allocation.
     *
     * Deletes the reconcile rows and recomputes the affected invoices'
     * residuals. Reversing the FX difference is the caller's job — it has
     * already been posted to the ledger and must be reversed by a journal
     * entry, not by deleting history.
     */
    static void unallocate(pqxx::transaction_base& txn, int paymentId);

    /**
     * @brief Residual for one invoice, derived from its allocations.
     *
     * amount_total minus everything applied. Derived rather than stored so
     * the residual cannot drift from the allocation rows.
     */
    static Money residualOf(pqxx::transaction_base& txn, int moveId);

    /// Recompute and store amount_residual + payment_state for one invoice.
    static void refreshResidual(pqxx::transaction_base& txn, int moveId);
};

} // namespace odoo::core
