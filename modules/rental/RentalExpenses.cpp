// =============================================================
// modules/rental/RentalExpenses.cpp
// =============================================================
#include "RentalExpenses.hpp"
#include "RentalEvents.hpp"

#include "DbConnection.hpp"
#include "IrCron.hpp"
#include "Money.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <string>
#include <vector>

namespace odoo::modules::rental {

using namespace odoo::infrastructure;
using namespace odoo::core;

namespace {

std::string today_() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return std::string(buf);
}

int monthsFor_(const std::string& interval) {
    if (interval == "quarterly") return 3;
    if (interval == "yearly")    return 12;
    return 1;   // monthly, and the safe default
}

} // namespace

ExpenseGenResult RentalExpenses::generate(std::shared_ptr<DbConnection> db,
                                          const std::string& asOfIn) {
    ExpenseGenResult out;
    const std::string asOf = asOfIn.empty() ? today_() : asOfIn;

    std::vector<int> templateIds;
    {
        auto conn = db->acquire();
        pqxx::work txn{conn.get()};
        auto rows = txn.exec(
            "SELECT id FROM rental_expense "
            " WHERE is_recurring "
            "   AND state <> 'cancelled' "
            "   AND recurrence_next_date IS NOT NULL "
            "   AND recurrence_next_date <= $1::date "
            "   AND (recurrence_end_date IS NULL OR recurrence_next_date <= recurrence_end_date) "
            " ORDER BY id LIMIT 1000",              // PERF-F
            pqxx::params{asOf});
        for (const auto& r : rows) templateIds.push_back(r[0].as<int>());
        txn.commit();
    }

    for (int tid : templateIds) {
        try {
            auto conn = db->acquire();
            pqxx::work txn{conn.get()};

            // Lock the template so two overlapping runs cannot both read
            // the same recurrence_next_date and both generate from it.
            auto t = txn.exec(
                "SELECT name, category_id, amount, partner_id, unit_id, contract_id, "
                "       account_id, company_id, recurrence_interval, "
                "       to_char(recurrence_next_date,'YYYY-MM-DD') AS next_date, "
                "       to_char(recurrence_end_date,'YYYY-MM-DD')  AS end_date "
                "  FROM rental_expense WHERE id = $1 FOR UPDATE",
                pqxx::params{tid});
            if (t.empty()) { txn.commit(); continue; }

            const int months = monthsFor_(t[0]["recurrence_interval"].is_null()
                                          ? "monthly" : t[0]["recurrence_interval"].c_str());
            std::string nextDate = t[0]["next_date"].c_str();
            const std::string endDate =
                t[0]["end_date"].is_null() ? "" : t[0]["end_date"].c_str();

            int madeHere = 0;
            // Catch up rather than generate one. A cron that was down for
            // a week must not silently lose a month of expense — the
            // forecast would then be wrong in the direction that flatters.
            // Bounded so a template with a nonsense date cannot spin.
            for (int guard = 0; guard < 60; ++guard) {
                if (nextDate > asOf) break;
                if (!endDate.empty() && nextDate > endDate) break;

                pqxx::params p;
                p.append(nextDate);
                p.append(std::string(t[0]["name"].c_str()));
                if (t[0]["category_id"].is_null()) p.append(nullptr);
                else p.append(t[0]["category_id"].as<int>());
                p.append(t[0]["amount"].as<long long>(0));
                if (t[0]["partner_id"].is_null()) p.append(nullptr);
                else p.append(t[0]["partner_id"].as<int>());
                if (t[0]["unit_id"].is_null()) p.append(nullptr);
                else p.append(t[0]["unit_id"].as<int>());
                if (t[0]["contract_id"].is_null()) p.append(nullptr);
                else p.append(t[0]["contract_id"].as<int>());
                if (t[0]["account_id"].is_null()) p.append(nullptr);
                else p.append(t[0]["account_id"].as<int>());
                p.append(tid);
                p.append(t[0]["company_id"].as<int>(1));

                // ON CONFLICT DO NOTHING against the unique index from
                // migration 806, so a second run for the same date is a
                // no-op rather than an error and catching up stays simple.
                //
                // The index is PARTIAL (WHERE recurrence_parent_id IS NOT
                // NULL), and inferring a partial index requires repeating
                // its predicate here. Without it PostgreSQL cannot match
                // the index and raises "no unique or exclusion constraint
                // matching the ON CONFLICT specification" — which the
                // per-template catch reported as a failed template.
                auto ins = txn.exec(
                    "INSERT INTO rental_expense "
                    "(date, name, category_id, amount, partner_id, unit_id, contract_id, "
                    " account_id, recurrence_parent_id, company_id, state, is_recurring) "
                    "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,'draft',FALSE) "
                    "ON CONFLICT (recurrence_parent_id, date) "
                    "  WHERE recurrence_parent_id IS NOT NULL DO NOTHING "
                    "RETURNING id", p);

                if (ins.empty()) out.skipped++;
                else { out.generated++; madeHere++; }

                auto nd = txn.exec(
                    "SELECT to_char(($1::date + ($2 || ' months')::interval)::date,"
                    "                'YYYY-MM-DD')",
                    pqxx::params{nextDate, months});
                nextDate = nd[0][0].c_str();
            }

            txn.exec("UPDATE rental_expense "
                     "   SET recurrence_next_date = $2::date, write_date = now() "
                     " WHERE id = $1",
                     pqxx::params{tid, nextDate});

            if (madeHere > 0) {
                EventCtx ctx;
                ctx.companyId = t[0]["company_id"].as<int>(1);
                if (!t[0]["unit_id"].is_null())    ctx.unitId    = t[0]["unit_id"].as<int>();
                if (!t[0]["contract_id"].is_null())ctx.contractId= t[0]["contract_id"].as<int>();
                if (!t[0]["partner_id"].is_null()) ctx.partnerId = t[0]["partner_id"].as<int>();
                ctx.refModel = "rental.expense";
                ctx.refId    = tid;
                RentalEvents::emit(
                    txn, evt::kExpenseGenerated, ctx,
                    std::string(t[0]["name"].c_str()) + " — " +
                        std::to_string(madeHere) + " occurrence(s) generated",
                    nlohmann::json{{"template_id", tid}, {"count", madeHere},
                                   {"next_date", nextDate}});
            }

            txn.commit();
        } catch (const std::exception& ex) {
            out.failed++;
            out.errors.push_back(ex.what());
            LOG_ERROR << "[rental/expenses] template " << tid << ": " << ex.what();
        }
    }

    return out;
}

void RentalExpenses::registerCron(std::shared_ptr<DbConnection> db) {
    if (!IrCron::ready()) return;
    IrCron::instance().registerJob("rental.expenses", [db] {
        const auto r = generate(db);
        LOG_INFO << "[rental/expenses] " << r.generated << " generated, "
                 << r.skipped << " skipped, " << r.failed << " failed";
    });
}

} // namespace odoo::modules::rental
