// =============================================================
// modules/rental/RentalForecast.cpp
// =============================================================
#include "RentalForecast.hpp"

#include "DbConnection.hpp"
#include "Money.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <map>
#include <string>

namespace cerp::modules::rental {

using namespace cerp::infrastructure;
using namespace cerp::core;

nlohmann::json RentalForecast::cashflow(std::shared_ptr<DbConnection> db,
                                        int months,
                                        const std::string& from) {
    months = std::clamp(months, 1, 36);

    auto conn = db->acquire();
    pqxx::work txn{conn.get()};

    // Anchor month. Everything below is expressed relative to it, so one
    // clamp here fixes the whole query.
    auto anchorRow = txn.exec(
        "SELECT to_char(date_trunc('month', COALESCE(NULLIF($1,'')::date, CURRENT_DATE)),"
        "                'YYYY-MM-DD')",
        pqxx::params{from});
    const std::string anchor = anchorRow[0][0].c_str();

    // ------------------------------------------------------------------
    // A line bills in month M when the whole number of months between its
    // next_period_start and M is a non-negative multiple of its billing
    // interval. Expressed in month ordinals so no date arithmetic is
    // needed per row.
    // ------------------------------------------------------------------
    const char* kSql = R"SQL(
WITH m AS (
    SELECT generate_series($1::date, ($1::date + (($2 - 1) || ' months')::interval),
                           interval '1 month')::date AS month
),
-- Income already invoiced: counted in the month it falls DUE, not the
-- month it was raised, because that is when the cash is expected.
recv AS (
    SELECT date_trunc('month', COALESCE(mv.due_date, mv.invoice_date, mv.date))::date AS month,
           SUM(mv.amount_residual) AS amount
      FROM account_move mv
     WHERE mv.move_type = 'out_invoice'
       AND mv.state = 'posted'
       AND mv.amount_residual > 0
     GROUP BY 1
),
-- Income not yet invoiced, projected from the tenancies. Only recurring
-- lines: a walk-in is invoiced by hand and cannot be forecast.
proj AS (
    SELECT m.month,
           SUM(GREATEST(l.unit_price - (l.unit_price * l.discount_pct) / 100000000, 0)) AS amount
      FROM m
      JOIN rental_contract_line l
        ON l.state = 'active'
       AND l.billing_mode = 'recurring'
       AND l.next_period_start IS NOT NULL
       AND (l.date_end IS NULL OR m.month <= l.date_end)
       AND ((EXTRACT(YEAR FROM m.month)::int * 12 + EXTRACT(MONTH FROM m.month)::int)
          - (EXTRACT(YEAR FROM l.next_period_start)::int * 12
             + EXTRACT(MONTH FROM l.next_period_start)::int)) >= 0
       AND ((EXTRACT(YEAR FROM m.month)::int * 12 + EXTRACT(MONTH FROM m.month)::int)
          - (EXTRACT(YEAR FROM l.next_period_start)::int * 12
             + EXTRACT(MONTH FROM l.next_period_start)::int))
           % GREATEST(l.billing_months, 1) = 0
     GROUP BY m.month
),
-- Budgeted recurring expenses, projected from the templates the same way.
exp_rec AS (
    SELECT m.month, SUM(e.amount) AS amount
      FROM m
      JOIN rental_expense e
        ON e.is_recurring
       AND e.state <> 'cancelled'
       AND e.recurrence_next_date IS NOT NULL
       AND (e.recurrence_end_date IS NULL OR m.month <= e.recurrence_end_date)
       AND ((EXTRACT(YEAR FROM m.month)::int * 12 + EXTRACT(MONTH FROM m.month)::int)
          - (EXTRACT(YEAR FROM e.recurrence_next_date)::int * 12
             + EXTRACT(MONTH FROM e.recurrence_next_date)::int)) >= 0
       AND ((EXTRACT(YEAR FROM m.month)::int * 12 + EXTRACT(MONTH FROM m.month)::int)
          - (EXTRACT(YEAR FROM e.recurrence_next_date)::int * 12
             + EXTRACT(MONTH FROM e.recurrence_next_date)::int))
           % CASE e.recurrence_interval
                 WHEN 'quarterly' THEN 3
                 WHEN 'yearly'    THEN 12
                 ELSE 1
             END = 0
     GROUP BY m.month
),
-- Expenses already entered with a date: one-offs, and children already
-- generated from a template. Recurring TEMPLATES are excluded here or
-- they would be counted twice, once as a template and once as a child.
exp_actual AS (
    SELECT date_trunc('month', e.date)::date AS month, SUM(e.amount) AS amount
      FROM rental_expense e
     WHERE NOT e.is_recurring
       AND e.state <> 'cancelled'
     GROUP BY 1
)
SELECT to_char(m.month, 'YYYY-MM')          AS month,
       COALESCE(recv.amount, 0)             AS receivable,
       COALESCE(proj.amount, 0)             AS projected_income,
       COALESCE(exp_rec.amount, 0)          AS budgeted_expense,
       COALESCE(exp_actual.amount, 0)       AS committed_expense
  FROM m
  LEFT JOIN recv       ON recv.month       = m.month
  LEFT JOIN proj       ON proj.month       = m.month
  LEFT JOIN exp_rec    ON exp_rec.month    = m.month
  LEFT JOIN exp_actual ON exp_actual.month = m.month
 ORDER BY m.month
)SQL";

    auto rows = txn.exec(kSql, pqxx::params{anchor, months});

    nlohmann::json out;
    out["from"]   = anchor;
    out["months"] = months;

    nlohmann::json series = nlohmann::json::array();
    long long cumulative = 0;
    long long totalIn = 0, totalOut = 0;

    for (const auto& r : rows) {
        const long long recv = r["receivable"].as<long long>(0);
        const long long proj = r["projected_income"].as<long long>(0);
        const long long bexp = r["budgeted_expense"].as<long long>(0);
        const long long cexp = r["committed_expense"].as<long long>(0);

        const long long in  = recv + proj;
        const long long outM = bexp + cexp;
        const long long net = in - outM;
        cumulative += net;
        totalIn  += in;
        totalOut += outM;

        series.push_back({
            {"month",             std::string(r["month"].c_str())},
            {"receivable",        Money::fromMicros(recv).toJson()},
            {"projected_income",  Money::fromMicros(proj).toJson()},
            {"income",            Money::fromMicros(in).toJson()},
            {"budgeted_expense",  Money::fromMicros(bexp).toJson()},
            {"committed_expense", Money::fromMicros(cexp).toJson()},
            {"expense",           Money::fromMicros(outM).toJson()},
            {"net",               Money::fromMicros(net).toJson()},
            {"cumulative",        Money::fromMicros(cumulative).toJson()},
        });
    }

    out["series"] = series;
    out["totals"] = {
        {"income",  Money::fromMicros(totalIn).toJson()},
        {"expense", Money::fromMicros(totalOut).toJson()},
        {"net",     Money::fromMicros(totalIn - totalOut).toJson()},
    };

    // Stated rather than implied. A forecast whose assumptions are not
    // visible gets read as a prediction.
    out["assumptions"] = nlohmann::json::array({
        "Recurring tenancies only — walk-ins are invoiced by hand and cannot be projected.",
        "Open invoices are counted in the month they fall due, at their unpaid residual.",
        "Recurring expenses use the BUDGETED amount from the template; generated "
        "occurrences use their actual amount.",
        "No churn assumption: every active tenancy is assumed to continue.",
        "Tax is excluded from projected income — it is not yours to keep.",
    });

    txn.commit();
    return out;
}

} // namespace cerp::modules::rental
