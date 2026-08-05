// ============================================================
// core/PaymentAllocation.cpp — see PaymentAllocation.hpp
// ============================================================
#include "PaymentAllocation.hpp"
#include "infrastructure/Errors.hpp"

#include <pqxx/pqxx>
#include <trantor/utils/Logger.h>

namespace odoo::core {

using infrastructure::ValidationError;

Money PaymentAllocation::residualOf(pqxx::transaction_base& txn, int moveId) {
    auto r = txn.exec(
        "SELECT m.amount_total - COALESCE(SUM(a.amount), 0) "
        "  FROM account_move m "
        "  LEFT JOIN account_partial_reconcile a ON a.move_id = m.id "
        " WHERE m.id = $1 "
        " GROUP BY m.amount_total",
        pqxx::params{moveId});
    if (r.empty()) return Money::zero();
    return Money::fromMicros(r[0][0].as<long long>(0));
}

void PaymentAllocation::refreshResidual(pqxx::transaction_base& txn, int moveId) {
    const Money residual = residualOf(txn, moveId);
    auto tot = txn.exec("SELECT amount_total FROM account_move WHERE id = $1",
                        pqxx::params{moveId});
    if (tot.empty()) return;
    const Money total = Money::fromMicros(tot[0][0].as<long long>(0));

    // isZero() rather than an epsilon: with int64 money "fully paid" is an
    // exact test, which is the point of P2. The old code compared a double
    // residual against 0.001.
    std::string state = "partial";
    if (residual.isZero())               state = "paid";
    else if (residual == total)          state = "not_paid";

    txn.exec(
        "UPDATE account_move SET amount_residual = $1, payment_state = $2, "
        "       write_date = now() WHERE id = $3",
        pqxx::params{residual.toDb(), state, moveId});
}

AllocationResult PaymentAllocation::allocate(pqxx::transaction_base& txn,
                                             int paymentId,
                                             const Money& receivedBase,
                                             const std::vector<int>& moveIds) {
    AllocationResult out;

    auto p = txn.exec(
        "SELECT partner_id, company_id, currency_id, amount, "
        "       COALESCE(currency_rate, 1000000) AS rate, date, "
        "       COALESCE(payment_type, 'inbound') AS payment_type "
        "  FROM account_payment WHERE id = $1 AND state = 'posted'",
        pqxx::params{paymentId});
    if (p.empty())
        throw ValidationError("Payment not found, or not posted.");

    const int partnerId = p[0]["partner_id"].is_null() ? 0 : p[0]["partner_id"].as<int>();
    const int companyId = p[0]["company_id"].is_null() ? 1 : p[0]["company_id"].as<int>();
    const std::string date = p[0]["date"].is_null() ? "" : p[0]["date"].c_str();
    const Money payAmount  = Money::fromMicros(p[0]["amount"].as<long long>(0));

    if (partnerId == 0)
        throw ValidationError("Payment has no partner, so it cannot be allocated.");

    // Effective settlement rate. When the bank converted, the caller passes
    // what actually landed and the rate is DERIVED — the bank's spread makes
    // any quoted rate wrong (docs/048 §4.6).
    int64_t settlementRate = p[0]["rate"].as<long long>(Money::ONE);
    if (!receivedBase.isZero() && !payAmount.isZero())
        settlementRate = payAmount.impliedRate(receivedBase);

    // Which document family this payment can settle. An OUTBOUND payment
    // settles vendor bills, not customer invoices — hard-coding
    // ('out_invoice','out_refund') meant a supplier payment matched nothing,
    // so the bill stayed open forever and the money sat as a permanent
    // unallocated credit on the vendor.
    const std::string payType = p[0]["payment_type"].c_str();
    const bool inbound = (payType != "outbound");
    const char* moveTypes = inbound ? "'out_invoice','out_refund'"
                                    : "'in_invoice','in_refund'";

    // Open documents for this partner, oldest first. Restricting to moveIds
    // covers "the customer told us which invoice this is for".
    //
    // moveTypes is a compile-time literal chosen by a bool — never a value
    // from the request — so it is not an injection surface.
    std::string sql =
        "SELECT m.id, m.amount_total, COALESCE(m.currency_rate, 1000000) AS booked_rate, "
        "       COALESCE(SUM(a.amount), 0) AS applied "
        "  FROM account_move m "
        "  LEFT JOIN account_partial_reconcile a ON a.move_id = m.id "
        " WHERE m.partner_id = $1 AND m.state = 'posted' "
        "   AND m.move_type IN (" + std::string(moveTypes) + ") ";
    if (!moveIds.empty()) {
        sql += "   AND m.id = ANY($2::int[]) ";
    }
    sql += " GROUP BY m.id, m.amount_total, m.currency_rate "
           "HAVING m.amount_total - COALESCE(SUM(a.amount), 0) <> 0 "
           " ORDER BY m.date, m.id";

    pqxx::params qp;
    qp.append(partnerId);
    if (!moveIds.empty()) {
        std::string arr = "{";
        for (std::size_t i = 0; i < moveIds.size(); ++i) {
            if (i) arr += ",";
            arr += std::to_string(moveIds[i]);
        }
        arr += "}";
        qp.append(arr);
    }
    auto invoices = txn.exec(sql, qp);

    Money remaining = payAmount;
    for (const auto& inv : invoices) {
        if (remaining.isZero()) break;

        const int   moveId     = inv["id"].as<int>();
        const Money total      = Money::fromMicros(inv["amount_total"].as<long long>(0));
        const Money applied    = Money::fromMicros(inv["applied"].as<long long>(0));
        const Money residual   = total - applied;
        const int64_t bookedRate = inv["booked_rate"].as<long long>(Money::ONE);

        if (residual.isZero()) continue;

        // Never apply more than the invoice still owes.
        const Money amount = (remaining.micros() < residual.micros())
                           ? remaining : residual;

        // Realised FX: what this slice is worth at the settlement rate versus
        // what it was booked at. Zero when both rates match, which is the
        // same-currency case.
        const Money atSettlement = amount.convertTo(0, settlementRate);
        const Money atBooked     = amount.convertTo(0, bookedRate);
        const Money fxDiff       = atSettlement - atBooked;

        pqxx::params ap;
        ap.append(paymentId); ap.append(moveId);
        ap.append(amount.toDb()); ap.append(atSettlement.toDb());
        ap.append(fxDiff.toDb()); ap.append(date); ap.append(companyId);
        txn.exec(
            "INSERT INTO account_partial_reconcile "
            "(payment_id, move_id, amount, amount_base, fx_diff, date, company_id) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7)", ap);

        refreshResidual(txn, moveId);

        out.applied.push_back({moveId, amount, atSettlement, fxDiff});
        out.totalApplied += amount;
        out.totalFxDiff  += fxDiff;
        remaining        -= amount;
    }

    // Whatever is left is a credit on the customer — the advance-payment
    // case. It is not an error and is not written anywhere: the view
    // account_payment_unallocated derives it from the allocations.
    out.unallocated = remaining;
    return out;
}

void PaymentAllocation::unallocate(pqxx::transaction_base& txn, int paymentId) {
    auto affected = txn.exec(
        "SELECT DISTINCT move_id FROM account_partial_reconcile WHERE payment_id = $1",
        pqxx::params{paymentId});

    txn.exec("DELETE FROM account_partial_reconcile WHERE payment_id = $1",
             pqxx::params{paymentId});

    // Residuals must be recomputed AFTER the delete, or they are derived from
    // allocations that no longer exist.
    for (const auto& r : affected)
        refreshResidual(txn, r[0].as<int>());
}

} // namespace odoo::core
