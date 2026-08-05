// =============================================================
// modules/rental/RentalDashboard.cpp
// =============================================================
#include "RentalDashboard.hpp"
#include "RentalForecast.hpp"

#include "DbConnection.hpp"
#include "Money.hpp"
#include "TtlCache.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <string>

namespace odoo::modules::rental {

using namespace odoo::infrastructure;
using namespace odoo::core;

namespace {

// 60 s. Long enough that a page refresh is free, short enough that a
// rate change shows up while the operator is still looking for it.
TtlCache<std::string, nlohmann::json>& cache_() {
    static TtlCache<std::string, nlohmann::json> c;
    return c;
}

} // namespace

nlohmann::json RentalDashboard::build(std::shared_ptr<DbConnection> db,
                                      int months, bool fresh) {
    months = std::clamp(months, 1, 36);
    const std::string key = "dashboard:" + std::to_string(months);

    if (!fresh) {
        if (auto hit = cache_().get(key)) return *hit;
    }

    nlohmann::json out;

    {
        auto conn = db->acquire();
        pqxx::work txn{conn.get()};

        // ---------------- occupancy ----------------
        // Retired units are excluded from the denominator: they are not
        // lettable stock, and counting them would understate occupancy
        // permanently. Same rule the unit grid applies.
        auto occ = txn.exec(
            "SELECT state, count(*) AS n FROM rental_unit "
            " WHERE active GROUP BY state");
        nlohmann::json byState = nlohmann::json::object();
        long long lettable = 0, occupied = 0, total = 0;
        for (const auto& r : occ) {
            const std::string s = r["state"].c_str();
            const long long n = r["n"].as<long long>(0);
            byState[s] = n;
            total += n;
            if (s != "retired") lettable += n;
            if (s == "occupied") occupied = n;
        }
        for (const char* s : {"occupied", "available", "reserved",
                              "maintenance", "retired"})
            if (!byState.contains(s)) byState[s] = 0;

        out["occupancy"] = {
            {"by_state", byState},
            {"total",    total},
            {"lettable", lettable},
            {"occupied", occupied},
            {"pct",      lettable > 0
                             ? static_cast<int>((occupied * 100 + lettable / 2) / lettable)
                             : 0},
        };

        // Occupancy per unit type — the stacked bar in docs/046 §2.
        auto byType = txn.exec(
            "SELECT COALESCE(t.name,'(no type)') AS type_name, u.state, count(*) AS n "
            "  FROM rental_unit u "
            "  LEFT JOIN rental_unit_type t ON t.id = u.type_id "
            " WHERE u.active "
            " GROUP BY 1, 2 ORDER BY 1");
        nlohmann::json types = nlohmann::json::object();
        for (const auto& r : byType) {
            const std::string tn = r["type_name"].c_str();
            if (!types.contains(tn)) types[tn] = nlohmann::json::object();
            types[tn][r["state"].c_str()] = r["n"].as<long long>(0);
        }
        out["occupancy"]["by_type"] = types;

        // ---------------- MRR ----------------
        // Monthly recurring revenue: recurring tenancies only, normalised
        // to a month so a quarterly line does not read as three times its
        // worth. Walk-ins are excluded — they are not recurring, and
        // counting them would inflate the one number most likely to be
        // quoted.
        auto mrr = txn.exec(
            "SELECT COALESCE(SUM( "
            "  GREATEST(l.unit_price - (l.unit_price * l.discount_pct) / 100000000, 0) "
            "  / GREATEST(l.billing_months, 1) ), 0) AS mrr "
            "  FROM rental_contract_line l "
            " WHERE l.state = 'active' AND l.billing_mode = 'recurring'");
        const long long mrrMicros = mrr.empty() ? 0 : mrr[0][0].as<long long>(0);
        out["mrr"] = Money::fromMicros(mrrMicros).toJson();

        // ---------------- receivables ----------------
        auto rec = txn.exec(
            "SELECT COALESCE(SUM(amount_residual),0) AS outstanding, "
            "       COALESCE(SUM(CASE WHEN due_date < CURRENT_DATE "
            "                         THEN amount_residual ELSE 0 END),0) AS overdue "
            "  FROM account_move "
            " WHERE move_type='out_invoice' AND state='posted' AND amount_residual > 0");
        const long long outstanding = rec.empty() ? 0 : rec[0]["outstanding"].as<long long>(0);
        const long long overdue     = rec.empty() ? 0 : rec[0]["overdue"].as<long long>(0);
        out["receivables"] = {
            {"outstanding", Money::fromMicros(outstanding).toJson()},
            {"overdue",     Money::fromMicros(overdue).toJson()},
        };

        // Ageing buckets — computed here, never stored (docs/040 §3.3).
        // An ORDERED magnitude of badness, which is why the UI renders it
        // as a one-hue ordinal ramp and not four categorical colours.
        auto age = txn.exec(
            "SELECT CASE "
            "         WHEN due_date IS NULL OR due_date >= CURRENT_DATE THEN 'current' "
            "         WHEN CURRENT_DATE - due_date <= 30  THEN 'd0_30' "
            "         WHEN CURRENT_DATE - due_date <= 60  THEN 'd31_60' "
            "         WHEN CURRENT_DATE - due_date <= 90  THEN 'd61_90' "
            "         ELSE 'd90_plus' END AS bucket, "
            "       COALESCE(SUM(amount_residual),0) AS amount "
            "  FROM account_move "
            " WHERE move_type='out_invoice' AND state='posted' AND amount_residual > 0 "
            " GROUP BY 1");
        nlohmann::json buckets = nlohmann::json::object();
        for (const char* b : {"current", "d0_30", "d31_60", "d61_90", "d90_plus"})
            buckets[b] = 0.0;
        for (const auto& r : age)
            buckets[r["bucket"].c_str()] =
                Money::fromMicros(r["amount"].as<long long>(0)).toJson();
        out["ageing"] = buckets;

        // ---------------- needs attention ----------------
        // A table, not a chart: mixed classes that each carry meaning.
        auto att = txn.exec(
            "SELECT "
            "  (SELECT count(*) FROM account_move "
            "    WHERE move_type='out_invoice' AND state='posted' "
            "      AND amount_residual > 0 AND due_date < CURRENT_DATE - 60) AS overdue_60, "
            "  (SELECT count(*) FROM rental_unit WHERE state='maintenance') AS maintenance, "
            "  (SELECT count(*) FROM rental_unit WHERE state='available' AND active) AS vacant, "
            "  (SELECT count(*) FROM rental_contract_line "
            "    WHERE state='active' AND billing_mode='manual') AS walk_ins, "
            "  (SELECT count(*) FROM account_payment_unallocated "
            "    WHERE amount_unallocated > 0) AS unallocated");
        out["attention"] = {
            {"overdue_60d",          att[0]["overdue_60"].as<long long>(0)},
            {"units_in_maintenance", att[0]["maintenance"].as<long long>(0)},
            {"units_vacant",         att[0]["vacant"].as<long long>(0)},
            {"walk_in_tenancies",    att[0]["walk_ins"].as<long long>(0)},
            {"unallocated_payments", att[0]["unallocated"].as<long long>(0)},
        };

        // ---------------- activity feed ----------------
        auto ev = txn.exec(
            "SELECT to_char(occurred_at,'YYYY-MM-DD HH24:MI') AS at, "
            "       event_type, COALESCE(summary,'') AS summary "
            "  FROM rental_event ORDER BY occurred_at DESC, id DESC LIMIT 20");
        nlohmann::json feed = nlohmann::json::array();
        for (const auto& r : ev)
            feed.push_back({{"at",      std::string(r["at"].c_str())},
                            {"type",    std::string(r["event_type"].c_str())},
                            {"summary", std::string(r["summary"].c_str())}});
        out["activity"] = feed;

        txn.commit();
    }

    // ---------------- cashflow ----------------
    // Delegated, not reimplemented: the dashboard and the standalone
    // /rental/cashflow endpoint must never disagree about the projection.
    out["cashflow"] = RentalForecast::cashflow(db, months);

    // NOI for the current month, taken from the cashflow series so the
    // KPI tile and the chart cannot tell different stories.
    double noi = 0.0;
    if (out["cashflow"].contains("series") && !out["cashflow"]["series"].empty())
        noi = out["cashflow"]["series"][0].value("net", 0.0);
    out["noi_month"] = noi;

    out["cached_seconds"] = 60;
    cache_().set(key, out, 60);
    return out;
}

} // namespace odoo::modules::rental
