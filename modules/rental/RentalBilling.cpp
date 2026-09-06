// =============================================================
// modules/rental/RentalBilling.cpp
// =============================================================
#include "RentalBilling.hpp"
#include "RentalEvents.hpp"

#include "DbConnection.hpp"
#include "IrCron.hpp"
#include "IrSequence.hpp"
#include "Money.hpp"
#include "TaxHelpers.hpp"
#include "PaymentAllocation.hpp"
#include "DecimalPrecision.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <map>
#include <tuple>
#include <sstream>
#include <string>
#include <vector>

namespace cerp::modules::rental {

using namespace cerp::infrastructure;
using namespace cerp::core;

namespace {

/// One tenancy line due for billing.
struct DueLine {
    int         id          = 0;
    int         unitId      = 0;
    int         contractId  = 0;
    std::string unitCode;
    std::string unitName;
    long long   unitPrice   = 0;    ///< micro-units
    long long   discountPct = 0;    ///< micro-units
    std::string taxIdsJson  = "[]";
    int         anchorDay   = 1;
    int         months      = 1;
    int         interval    = 1;          ///< the X in "every X <unit>"
    std::string unit        = "month";    ///< day | week | month | year
};

struct DueGroup {
    int         partnerId  = 0;
    std::string periodStart;
    int         companyId  = 1;
    int         currencyId = 0;
    int         journalId  = 0;
    std::vector<DueLine> lines;
};

std::string today_() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return std::string(buf);
}

} // namespace

BillingResult RentalBilling::run(std::shared_ptr<DbConnection> db,
                                 const std::string& asOfIn,
                                 int contractId) {
    BillingResult out;
    const std::string asOf = asOfIn.empty() ? today_() : asOfIn;

    // ------------------------------------------------------------------
    // 1. Find what is due, in a read-only transaction.
    //
    // Due means: the period starts within billing_lead_days of asOf.
    // Billing IN ADVANCE is the whole point — the invoice goes out before
    // the period begins, so `next_period_start - lead <= asOf`.
    // ------------------------------------------------------------------
    // Key is (partner, period, company, currency) — NOT just
    // (partner, period).
    //
    // Grouping on partner+period alone let the group's company, currency
    // and journal be overwritten by whichever line was read last, so two
    // units billed together silently took the last one's values. Two
    // units in different currencies would have produced one invoice in
    // whichever currency happened to sort last — a wrong invoice with no
    // error anywhere. Making them part of the key means a mismatch
    // produces two correct invoices instead of one wrong one.
    struct GroupKey {
        int         partnerId;
        std::string periodStart;
        int         companyId;
        int         currencyId;
        bool operator<(const GroupKey& o) const {
            return std::tie(partnerId, periodStart, companyId, currencyId)
                 < std::tie(o.partnerId, o.periodStart, o.companyId, o.currencyId);
        }
    };
    std::map<GroupKey, DueGroup> groups;
    {
        auto conn = db->acquire();
        pqxx::work txn{conn.get()};

        std::string sql =
            "SELECT l.id, l.partner_id, l.unit_id, l.contract_id, "
            "       l.unit_price, l.discount_pct, l.tax_ids_json, "
            "       l.billing_anchor_day, l.billing_months, l.company_id, "
            // The line's own period wins; NULL means it follows the contract
            // (migration 816). Resolving it here rather than in C++ keeps
            // "what cadence is this line on" answerable in one SQL query.
            "       COALESCE(l.billing_interval, c.billing_interval, 1) AS billing_interval, "
            "       COALESCE(l.billing_unit,     c.billing_unit, 'month') AS billing_unit, "
            "       to_char(COALESCE(l.next_period_start, l.date_start), 'YYYY-MM-DD') AS period_start, "
            "       COALESCE(u.code, '')  AS unit_code, "
            "       COALESCE(u.name, '')  AS unit_name, "
            "       c.currency_id, c.journal_id "
            "  FROM rental_contract_line l "
            "  LEFT JOIN rental_unit     u ON u.id = l.unit_id "
            "  LEFT JOIN rental_contract c ON c.id = l.contract_id "
            " WHERE l.state = 'active' ";

        // The scheduled run bills only what is SCHEDULED. Asking for one
        // contract is a person saying "bill this now", which is the only thing
        // that may reach a oneoff or on-demand line — see RentalBilling.hpp.
        if (contractId > 0) {
            // Every billing mode, including 'manual' — which is the column
            // DEFAULT, so it is what a line gets when nobody chose. "Manual"
            // means "invoiced by hand", and this button IS the hand; excluding
            // it left those lines billable by nothing at all, which is what a
            // real contract hit: monthly, active, due, and "nothing is due".
            sql += "   AND l.contract_id = $2 ";
        } else {
            sql += "   AND l.billing_mode = 'recurring' "
                   "   AND l.next_period_start IS NOT NULL "
                   // A one-off or on-demand CONTRACT is never scheduled,
                   // whatever its lines say. Without this the COALESCE above
                   // would quietly fall back to monthly and invoice a contract
                   // the user marked as billed only when asked.
                   "   AND (c.id IS NULL OR c.billing_period NOT IN ('oneoff','ondemand')) ";
        }

        sql +=
            // The period gate holds either way: nothing is invoiced before its
            // lead days, however it was asked for.
            "   AND COALESCE(l.next_period_start, l.date_start) IS NOT NULL "
            "   AND COALESCE(l.next_period_start, l.date_start) - l.billing_lead_days "
            "       <= $1::date "
            // A line that has been ended stops billing even if its period start
            // is still in the past.
            "   AND (l.date_end IS NULL "
            "        OR COALESCE(l.next_period_start, l.date_start) <= l.date_end) "
            " ORDER BY l.partner_id, COALESCE(l.next_period_start, l.date_start), l.id "
            " LIMIT 1000";                       // PERF-F

        pqxx::params params;
        params.append(asOf);
        if (contractId > 0) params.append(contractId);
        auto rows = txn.exec(sql, params);

        for (const auto& r : rows) {
            GroupKey key;
            key.partnerId   = r["partner_id"].as<int>(0);
            key.periodStart = r["period_start"].c_str();
            key.companyId   = r["company_id"].as<int>(1);
            key.currencyId  = r["currency_id"].is_null() ? 0 : r["currency_id"].as<int>();

            auto& g = groups[key];
            g.partnerId   = key.partnerId;
            g.periodStart = key.periodStart;
            g.companyId   = key.companyId;
            g.currencyId  = key.currencyId;
            // The journal is not part of the key: it is a routing choice
            // rather than an accounting attribute of the invoice, and
            // falls back to the company's sale journal when unset.
            if (!r["journal_id"].is_null())  g.journalId  = r["journal_id"].as<int>();

            DueLine dl;
            dl.id          = r["id"].as<int>();
            dl.unitId      = r["unit_id"].is_null() ? 0 : r["unit_id"].as<int>();
            dl.contractId  = r["contract_id"].is_null() ? 0 : r["contract_id"].as<int>();
            dl.unitCode    = r["unit_code"].c_str();
            dl.unitName    = r["unit_name"].c_str();
            dl.unitPrice   = r["unit_price"].as<long long>(0);
            dl.discountPct = r["discount_pct"].as<long long>(0);
            dl.taxIdsJson  = r["tax_ids_json"].is_null() ? "[]" : r["tax_ids_json"].c_str();
            dl.anchorDay   = r["billing_anchor_day"].as<int>(1);
            dl.months      = r["billing_months"].as<int>(1);
            dl.interval     = r["billing_interval"].as<int>(1);
            dl.unit         = r["billing_unit"].is_null() ? std::string("month")
                                                          : r["billing_unit"].c_str();
            g.lines.push_back(std::move(dl));
        }
        txn.commit();
    }

    if (groups.empty()) return out;

    // ------------------------------------------------------------------
    // 2. One transaction PER GROUP.
    //
    // A failure on one customer must not half-bill another, and must not
    // abort the run — so each group commits or rolls back alone, and the
    // loop continues past a failure with the error recorded.
    // ------------------------------------------------------------------
    for (auto& [key, g] : groups) {
        try {
            auto conn = db->acquire();
            pqxx::work txn{conn.get()};

            // Journal: the contract's, else the first sale journal.
            int journalId = g.journalId;
            if (journalId == 0) {
                auto j = txn.exec(
                    "SELECT id FROM account_journal "
                    " WHERE type = 'sale' AND company_id = $1 ORDER BY id LIMIT 1",
                    pqxx::params{g.companyId});
                if (j.empty()) throw std::runtime_error("no sale journal configured");
                journalId = j[0][0].as<int>();
            }

            auto acc = txn.exec(
                "SELECT id FROM account_account "
                " WHERE code = '1200' AND company_id = $1 LIMIT 1",
                pqxx::params{g.companyId});
            if (acc.empty()) throw std::runtime_error("receivable account 1200 missing");
            const int arAccount = acc[0][0].as<int>();

            auto rev = txn.exec(
                "SELECT id FROM account_account "
                " WHERE code = '4000' AND company_id = $1 LIMIT 1",
                pqxx::params{g.companyId});
            if (rev.empty()) throw std::runtime_error("revenue account 4000 missing");
            const int revAccount = rev[0][0].as<int>();

            // period_end is the day before the NEXT period starts, using
            // the same anchor arithmetic the advance below uses — so the
            // printed period and the next due date can never disagree.
            auto pe = txn.exec(
                "SELECT to_char(rental_next_period($1::date, $2, $3, $4) - 1, 'YYYY-MM-DD')",
                pqxx::params{g.periodStart, g.lines[0].anchorDay,
                             g.lines[0].interval, g.lines[0].unit});
            const std::string periodEnd = pe[0][0].c_str();

            // Invoice number from ir.sequence inside this transaction —
            // never COUNT(*)+1, which P4 removed from three other places.
            //
            // A rental invoice is an out_invoice, so it draws from the SAME
            // customer-invoice series as a hand-posted one:
            // `account.move.INV` — prefix "INV", padding 6, seeded by
            // migration 1020. A rental invoice and a manual invoice must
            // share one continuous series; two series in the customer-
            // invoice space is a numbering gap waiting to be explained to
            // an auditor.
            const std::string invName =
                IrSequence::instance().nextByCode(txn, "account.move.INV");

            // Origin, exactly as a sale-generated invoice carries it: the
            // contract name when there is one, else a label naming the
            // period. `invoice_origin` is a registered field on
            // account.move, so this reaches the API and the customer
            // portal with no further wiring.
            //
            // Walk-ins genuinely have no contract, so the origin names the
            // period instead of inventing a document that does not exist.
            int contractId = 0;
            for (const auto& dl : g.lines)
                if (dl.contractId > 0) { contractId = dl.contractId; break; }

            std::string origin;
            if (contractId > 0) {
                auto cn = txn.exec("SELECT name FROM rental_contract WHERE id = $1",
                                   pqxx::params{contractId});
                if (!cn.empty() && !cn[0][0].is_null()) origin = cn[0][0].c_str();
            }
            if (origin.empty()) origin = "Rental " + g.periodStart;

            pqxx::params mp;
            mp.append(invName);
            mp.append(asOf);                 // invoice_date
            mp.append(g.periodStart);        // due on the day the period starts
            mp.append(journalId);
            mp.append(g.partnerId);
            mp.append(g.companyId);
            if (g.currencyId > 0) mp.append(g.currencyId); else mp.append(nullptr);
            mp.append("Rental " + g.periodStart + " to " + periodEnd);
            mp.append(origin);
            if (contractId > 0) mp.append(contractId); else mp.append(nullptr);

            // The column is `due_date`, not the reference ERP's `invoice_date_due`.
            auto mv = txn.exec(
                "INSERT INTO account_move "
                "(name, move_type, state, date, invoice_date, due_date, "
                " journal_id, partner_id, company_id, currency_id, narration, "
                " invoice_origin, rental_contract_id, "
                " amount_untaxed, amount_tax, amount_total, amount_residual, payment_state) "
                "VALUES ($1,'out_invoice','posted',$2,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
                "        0,0,0,0,'not_paid') "
                "RETURNING id", mp);
            const int moveId = mv[0][0].as<int>();

            const int dp = DecimalPrecision::ready()
                         ? DecimalPrecision::instance().digits(DecimalPrecision::kAccount, 2)
                         : 2;

            long long untaxedTotal = 0;

            for (const auto& dl : g.lines) {
                // Compute through TaxEngine — the same path sale and
                // purchase use. Rent is taxable, and reimplementing the
                // arithmetic here is how the two modules drifted before.
                nlohmann::json vals;
                vals["quantity"]     = 1.0;
                vals["price_unit"]   = Money::fromMicros(dl.unitPrice).toJson();
                vals["discount"]     = Money::fromMicros(dl.discountPct).toJson();
                vals["tax_ids_json"] = dl.taxIdsJson;
                core::applyLineTaxes(txn, vals, "quantity", dp);

                const long long subtotal =
                    Money::fromJson(vals.value("price_subtotal", 0.0)).toDb();

                const std::string label =
                    (dl.unitCode.empty() ? std::string("Rental") : "Unit " + dl.unitCode)
                    + (dl.unitName.empty() ? "" : " (" + dl.unitName + ")")
                    + " — " + g.periodStart + " to " + periodEnd;

                // account_move_line has no `discount` column, so the NET
                // rate is written as price_unit rather than the standard
                // rate. With quantity 1 that makes price_unit == credit,
                // so the line is internally consistent and the customer is
                // charged exactly what the line shows.
                //
                // The alternative — standard rate in price_unit, discount
                // applied invisibly — prints RM 120.00 on a line that
                // charges RM 102.00, with nothing on the invoice
                // explaining the gap. Showing "RM 120.00 less 15%" needs a
                // discount column, and belongs with the committed-use
                // pricing work rather than smuggled in here.
                pqxx::params lp;
                lp.append(moveId); lp.append(revAccount); lp.append(journalId);
                lp.append(g.companyId); lp.append(asOf); lp.append(label);
                lp.append(g.partnerId);
                lp.append(subtotal);              // credit
                lp.append(subtotal);              // price_unit == net, qty is 1
                lp.append(dl.taxIdsJson);
                txn.exec(
                    "INSERT INTO account_move_line "
                    "(move_id, account_id, journal_id, company_id, date, name, partner_id, "
                    " debit, credit, quantity, price_unit, tax_ids_json, display_type) "
                    "VALUES ($1,$2,$3,$4,$5,$6,$7,0,$8,1000000,$9,$10,'')", lp);

                untaxedTotal += subtotal;

                // THE guard. A second run of this period hits the UNIQUE
                // and rolls the whole group back — no invoice, no advance.
                // ir.cron is at-least-once, so this is what makes
                // double-billing impossible, not the schedule.
                pqxx::params ip;
                ip.append(moveId); ip.append(dl.contractId > 0 ? dl.contractId : 0);
                ip.append(dl.id); ip.append(g.periodStart); ip.append(periodEnd);
                ip.append(subtotal); ip.append(g.companyId);
                txn.exec(
                    "INSERT INTO rental_invoice_link "
                    "(move_id, contract_id, contract_line_id, period_start, period_end, "
                    " amount, company_id) "
                    "VALUES ($1, NULLIF($2,0), $3, $4, $5, $6, $7)", ip);

                // Advance to the next period, by the same function that
                // produced period_end.
                txn.exec(
                    "UPDATE rental_contract_line "
                    // The 4-argument form advances by (interval, unit) so a
                    // weekly or daily tenancy moves by the right amount; the
                    // 3-argument form it replaces could only step whole months.
                    // COALESCE, because only a recurring line HAS a
                    // next_period_start (migration 819 sets it for those
                    // alone). Without it rental_next_period(NULL, …) is NULL,
                    // the period never moves, and a monthly contract billed by
                    // hand could be invoiced exactly once, ever.
                    //
                    // A one-off is the exception and keeps its NULL: it is
                    // billed once by definition, and UNIQUE (contract_line_id,
                    // period_start) is what refuses the second attempt.
                    "   SET next_period_start = CASE WHEN billing_mode = 'oneoff' "
                    "                                THEN next_period_start "
                    "                                ELSE rental_next_period( "
                    "                                       COALESCE(next_period_start, date_start), "
                    "                                       $2, $3, $4) END, "
                    "       invoiced_through  = $5::date, "
                    "       write_date = now() "
                    " WHERE id = $1",
                    pqxx::params{dl.id, dl.anchorDay, dl.interval, dl.unit, periodEnd});
            }

            // One tax line per tax, accumulated across the units on this
            // invoice — which is what an invoice is expected to show.
            // Rounding is per line then summed (docs/048 option A), so the
            // printed column always foots to the printed total.
            long long taxTotal = 0;
            {
                std::map<int, long long> byTax;
                for (const auto& dl : g.lines) {
                    const auto taxes = core::loadTaxes(txn, dl.taxIdsJson);
                    if (taxes.empty()) continue;
                    const auto res = TaxEngine::compute(
                        Money::fromMicros(dl.unitPrice),
                        Money::parse("1"),
                        Money::fromMicros(dl.discountPct),
                        taxes, dp);
                    for (const auto& c : res.components)
                        byTax[c.taxId] += c.amount.toDb();
                }
                for (const auto& [taxId, amount] : byTax) {
                    if (amount == 0) continue;
                    auto ta = txn.exec(
                        "SELECT COALESCE(t.account_id, "
                        "  (SELECT id FROM account_account "
                        "    WHERE code='2200' AND company_id=$2 LIMIT 1)), t.name "
                        " FROM account_tax t WHERE t.id = $1",
                        pqxx::params{taxId, g.companyId});
                    if (ta.empty() || ta[0][0].is_null()) continue;
                    pqxx::params tp;
                    tp.append(moveId); tp.append(ta[0][0].as<int>()); tp.append(journalId);
                    tp.append(g.companyId); tp.append(asOf);
                    tp.append(std::string(ta[0][1].c_str())); tp.append(taxId);
                    tp.append(amount);
                    txn.exec(
                        "INSERT INTO account_move_line "
                        "(move_id, account_id, journal_id, company_id, date, name, "
                        " tax_line_id, credit, debit, display_type) "
                        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,0,'')", tp);
                    taxTotal += amount;
                }
            }

            const long long total = untaxedTotal + taxTotal;

            // The receivable leg, so the entry balances.
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, name, partner_id, "
                " debit, credit, display_type) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,0,'')",
                pqxx::params{moveId, arAccount, journalId, g.companyId, asOf,
                             std::string("Rental receivable"), g.partnerId, total});

            txn.exec(
                "UPDATE account_move "
                "   SET amount_untaxed=$2, amount_tax=$3, amount_total=$4, "
                "       amount_residual=$4, write_date=now() "
                " WHERE id=$1",
                pqxx::params{moveId, untaxedTotal, taxTotal, total});

            // Consume any unallocated credit the customer is carrying.
            // This is the advance-payment behaviour: someone who paid six
            // months up front has it drawn down one period at a time, on
            // their own billing date.
            try {
                auto pmts = txn.exec(
                    "SELECT payment_id FROM account_payment_unallocated "
                    " WHERE partner_id = $1 AND amount_unallocated > 0 "
                    " ORDER BY payment_id",
                    pqxx::params{g.partnerId});
                for (const auto& p : pmts) {
                    core::PaymentAllocation::allocate(
                        txn, p[0].as<int>(), core::Money::zero(), {moveId});
                    auto resid = txn.exec(
                        "SELECT amount_residual FROM account_move WHERE id=$1",
                        pqxx::params{moveId});
                    if (!resid.empty() && resid[0][0].as<long long>(0) <= 0) break;
                }
            } catch (const std::exception& ex) {
                // An allocation problem must not lose the invoice — the
                // invoice is the thing that must exist.
                LOG_WARN << "[rental/billing] advance allocation skipped: " << ex.what();
            }

            EventCtx ctx;
            ctx.partnerId  = g.partnerId;
            ctx.companyId  = g.companyId;
            ctx.refModel   = "account.move";
            ctx.refId      = moveId;
            if (!g.lines.empty()) {
                ctx.lineId     = g.lines[0].id;
                ctx.unitId     = g.lines[0].unitId;
                ctx.contractId = g.lines[0].contractId;
            }
            RentalEvents::emit(
                txn, evt::kInvoiceGenerated, ctx,
                "Invoice " + invName + " for " + g.periodStart + " to " + periodEnd +
                    " (" + std::to_string(g.lines.size()) + " unit(s))",
                nlohmann::json{{"move_id", moveId},
                               {"period_start", g.periodStart},
                               {"period_end", periodEnd},
                               {"total", Money::fromMicros(total).toJson()}});

            txn.commit();

            out.invoicesCreated++;
            out.linesBilled += static_cast<int>(g.lines.size());
            out.moveIds.push_back(moveId);

        } catch (const std::exception& ex) {
            const std::string what = ex.what();
            // A duplicate is not a failure — it is the guard doing its
            // job on a second run. Counted separately so a re-run reports
            // "skipped", not "failed".
            if (what.find("rental_invoice_link_uniq") != std::string::npos ||
                what.find("duplicate key") != std::string::npos) {
                out.groupsSkipped++;
            } else {
                out.groupsFailed++;
                out.errors.push_back(what);
                LOG_ERROR << "[rental/billing] partner " << g.partnerId
                          << " period " << g.periodStart << ": " << what;
            }
        }
    }

    return out;
}

void RentalBilling::registerCron(std::shared_ptr<DbConnection> db) {
    if (!IrCron::ready()) return;
    IrCron::instance().registerJob("rental.billing", [db] {
        const auto r = run(db);
        LOG_INFO << "[rental/billing] " << r.invoicesCreated << " invoice(s), "
                 << r.linesBilled << " line(s), " << r.groupsSkipped << " skipped, "
                 << r.groupsFailed << " failed";
    });
}

} // namespace cerp::modules::rental
