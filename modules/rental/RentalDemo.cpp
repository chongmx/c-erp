// =============================================================
// modules/rental/RentalDemo.cpp
// =============================================================
#include "RentalDemo.hpp"
#include "RentalEvents.hpp"

#include "DbConnection.hpp"
#include "Money.hpp"

#include <drogon/drogon.h>
#include <pqxx/pqxx>

#include <string>
#include <vector>

namespace cerp::modules::rental {

using namespace cerp::infrastructure;
using namespace cerp::core;

namespace {

// The ONLY definition of what demo data is. Both seed and clear read
// these, so the two can never disagree about what they own.
constexpr const char* kSite         = "Demo Warehouse";
constexpr const char* kContractName = "DEMO/1";

// Named so a human recognises them in a list, and fixed so clear() can
// match on them exactly rather than guessing by category.
const std::vector<std::string> kExpenseNames = {
    "Wifi / broadband", "Electricity", "Security monitoring",
    "Cleaning contract", "Facility maintenance", "Insurance premium",
    "Warehouse lease",
};

struct ZoneDef { const char* prefix; int count; const char* zone; const char* typeCode; double area; };
const std::vector<ZoneDef> kZones = {
    {"A", 16, "Zone A \xE2\x80\x94 Small Lockers",  "SL",  1.5},
    {"B", 10, "Zone B \xE2\x80\x94 Medium Lockers", "ML",  3.0},
    {"C",  8, "Zone C \xE2\x80\x94 Large Lockers",  "LL",  6.0},
    {"R",  5, "Zone D \xE2\x80\x94 Storage Rooms",  "RM", 18.0},
    {"P",  6, "Zone E \xE2\x80\x94 Pallet Spaces",  "PS",  2.4},
};

// Which units are let, and how. Zero-padded to two digits — `seq -w`
// pads to the width of the largest value, so the shell version once
// produced A01 but C1 and every later reference to C01 matched nothing.
const std::vector<std::string> kActive = {
    "A01","A02","A04","A07","A08","A11","A12","A15",
    "B01","B02","B05","B09","C01","C03","R01","R02","P01","P03",
};
const std::vector<std::string> kPending     = {"A03","B07","C05","R04"};
const std::vector<std::string> kMaintenance = {"A09","C08"};

// {name, monthly micro-units, interval}
struct ExpDef { const char* name; long long amount; const char* interval; const char* category; };
const std::vector<ExpDef> kExpenses = {
    {"Wifi / broadband",     200LL * 1000000, "monthly",   "Utilities"},
    {"Electricity",          850LL * 1000000, "monthly",   "Utilities"},
    {"Security monitoring",  450LL * 1000000, "monthly",   "Security"},
    {"Cleaning contract",    300LL * 1000000, "monthly",   "Cleaning"},
    {"Facility maintenance",1200LL * 1000000, "quarterly", "Maintenance"},
    {"Insurance premium",   4800LL * 1000000, "yearly",    "Insurance"},
    {"Warehouse lease",     6500LL * 1000000, "monthly",   "Rent / Lease"},
};

std::string expenseNameList(pqxx::work& txn) {
    std::string out;
    for (const auto& n : kExpenseNames) {
        if (!out.empty()) out += ",";
        out += txn.quote(n);
    }
    return out;
}

long long scalar(pqxx::work& txn, const std::string& sql) {
    auto r = txn.exec(sql);
    return (r.empty() || r[0][0].is_null()) ? 0 : r[0][0].as<long long>(0);
}

} // namespace

nlohmann::json RentalDemo::status(std::shared_ptr<DbConnection> db) {
    auto conn = db->acquire();
    pqxx::work txn{conn.get()};
    const std::string names = expenseNameList(txn);

    const long long units = scalar(txn,
        "SELECT count(*) FROM rental_unit WHERE site = " + txn.quote(kSite));
    const long long contracts = scalar(txn,
        "SELECT count(*) FROM rental_contract WHERE name LIKE 'DEMO/%'");
    const long long tenancies = scalar(txn,
        "SELECT count(*) FROM rental_contract_line WHERE contract_id IN "
        "(SELECT id FROM rental_contract WHERE name LIKE 'DEMO/%')");
    const long long templates = scalar(txn,
        "SELECT count(*) FROM rental_expense WHERE is_recurring AND name IN (" + names + ")");
    const long long children = scalar(txn,
        "SELECT count(*) FROM rental_expense WHERE recurrence_parent_id IN "
        "(SELECT id FROM rental_expense WHERE name IN (" + names + "))");
    const long long invoices = scalar(txn,
        "SELECT count(DISTINCT ril.move_id) FROM rental_invoice_link ril "
        " JOIN rental_contract_line l ON l.id = ril.contract_line_id "
        " WHERE l.contract_id IN (SELECT id FROM rental_contract WHERE name LIKE 'DEMO/%')");
    txn.commit();

    return {
        {"present",           units > 0 || contracts > 0 || templates > 0},
        {"units",             units},
        {"contracts",         contracts},
        {"tenancies",         tenancies},
        {"expense_templates", templates},
        {"expense_entries",   children},
        {"invoices",          invoices},
        {"site",              kSite},
    };
}

nlohmann::json RentalDemo::seed(std::shared_ptr<DbConnection> db) {
    auto conn = db->acquire();
    pqxx::work txn{conn.get()};

    // A partner to be the tenant. The demo is about the facility, not
    // about inventing customers, so an existing one is reused.
    auto pr = txn.exec("SELECT id FROM res_partner ORDER BY id LIMIT 1");
    if (pr.empty())
        throw std::runtime_error("no partner exists to act as the demo tenant");
    const int partnerId = pr[0][0].as<int>();

    int unitsMade = 0;
    for (const auto& z : kZones) {
        auto ty = txn.exec("SELECT id FROM rental_unit_type WHERE code = $1 LIMIT 1",
                           pqxx::params{std::string(z.typeCode)});
        for (int i = 1; i <= z.count; ++i) {
            char code[8];
            std::snprintf(code, sizeof(code), "%s%02d", z.prefix, i);
            pqxx::params p;
            p.append(std::string(code));
            if (ty.empty()) p.append(nullptr); else p.append(ty[0][0].as<int>());
            p.append(std::string(kSite));
            p.append(std::string(z.zone));
            p.append(z.area);
            auto r = txn.exec(
                "INSERT INTO rental_unit (code, name, type_id, site, zone, state, "
                "                         area_sqm, company_id) "
                "VALUES ($1,'',$2,$3,$4,'available',$5,1) "
                "ON CONFLICT (code, company_id) DO NOTHING RETURNING id", p);
            if (!r.empty()) ++unitsMade;
        }
    }

    // The contract. Its presence is what makes these tenancies recurring
    // rather than walk-ins.
    auto cr = txn.exec("SELECT id FROM rental_contract WHERE name = $1",
                       pqxx::params{std::string(kContractName)});
    int contractId;
    if (cr.empty()) {
        auto ins = txn.exec(
            "INSERT INTO rental_contract (name, partner_id, state, date_start, "
            "                             billing_period, company_id) "
            "VALUES ($1,$2,'active',CURRENT_DATE - 90,'monthly',1) RETURNING id",
            pqxx::params{std::string(kContractName), partnerId});
        contractId = ins[0][0].as<int>();
    } else {
        contractId = cr[0][0].as<int>();
    }

    int letMade = 0;
    auto letUnit = [&](const std::string& code, const char* state, int offsetDays) {
        auto u = txn.exec(
            "SELECT u.id, COALESCE(t.default_rate, 120000000) "
            "  FROM rental_unit u LEFT JOIN rental_unit_type t ON t.id = u.type_id "
            " WHERE u.code = $1 AND u.site = $2",
            pqxx::params{code, std::string(kSite)});
        if (u.empty()) return;                 // unit absent: nothing to let
        const int unitId = u[0][0].as<int>();
        // The partial unique index already forbids a second live line on
        // a unit; checking first keeps a re-run quiet instead of noisy.
        auto live = txn.exec(
            "SELECT 1 FROM rental_contract_line "
            " WHERE unit_id = $1 AND state IN ('pending','active')",
            pqxx::params{unitId});
        if (!live.empty()) return;

        // $4::int is required, not decorative. A bound parameter arrives
        // untyped, so `CURRENT_DATE + $4` is ambiguous — PostgreSQL can
        // read it as date+integer or date+interval and refuses to guess:
        // "operator is not unique: date + unknown".
        txn.exec(
            "INSERT INTO rental_contract_line "
            "(contract_id, partner_id, unit_id, date_start, unit_price, state, "
            " billing_mode, billing_anchor_day, billing_months, billing_lead_days, "
            " next_period_start, company_id) "
            "VALUES ($1,$2,$3,CURRENT_DATE + $4::int,$5,$6,'recurring',1,1,7,"
            "        date_trunc('month', CURRENT_DATE + $4::int + interval '1 month')::date,1)",
            pqxx::params{contractId, partnerId, unitId, offsetDays,
                         u[0][1].as<long long>(120000000), std::string(state)});
        ++letMade;
    };

    for (const auto& c : kActive)  letUnit(c, "active",  -60);
    for (const auto& c : kPending) letUnit(c, "pending",  21);

    // Operator facts, which the derivation trigger must never overwrite.
    for (const auto& c : kMaintenance)
        txn.exec("UPDATE rental_unit SET state='maintenance' "
                 " WHERE code = $1 AND site = $2 AND state <> 'maintenance'",
                 pqxx::params{c, std::string(kSite)});

    int expMade = 0;
    for (const auto& e : kExpenses) {
        auto cat = txn.exec("SELECT id FROM rental_expense_category WHERE name = $1 LIMIT 1",
                            pqxx::params{std::string(e.category)});
        pqxx::params p;
        p.append(std::string(e.name));
        if (cat.empty()) p.append(nullptr); else p.append(cat[0][0].as<int>());
        p.append(e.amount);
        p.append(std::string(e.interval));
        auto r = txn.exec(
            "INSERT INTO rental_expense "
            "(date, name, category_id, amount, is_recurring, recurrence_interval, "
            " recurrence_next_date, company_id, state) "
            "SELECT date_trunc('month', CURRENT_DATE)::date, $1, $2, $3, TRUE, $4, "
            "       date_trunc('month', CURRENT_DATE)::date, 1, 'draft' "
            " WHERE NOT EXISTS (SELECT 1 FROM rental_expense WHERE name = $1) "
            "RETURNING id", p);
        if (!r.empty()) ++expMade;
    }

    EventCtx ctx;
    ctx.partnerId = partnerId;
    ctx.contractId = contractId;
    RentalEvents::emit(txn, "demo_seeded", ctx,
        "Demo facility created: " + std::to_string(unitsMade) + " unit(s), " +
        std::to_string(letMade) + " tenancy(ies), " +
        std::to_string(expMade) + " expense budget(s)",
        nlohmann::json{{"units", unitsMade}, {"tenancies", letMade},
                       {"expenses", expMade}});

    txn.commit();

    auto out = status(db);
    out["created"] = {{"units", unitsMade}, {"tenancies", letMade},
                      {"expense_templates", expMade}};
    return out;
}

nlohmann::json RentalDemo::clear(std::shared_ptr<DbConnection> db) {
    auto conn = db->acquire();
    pqxx::work txn{conn.get()};
    const std::string names = expenseNameList(txn);
    const std::string site  = txn.quote(kSite);

    nlohmann::json removed;

    // Order matters: children before parents, and invoice links before
    // the tenancies they reference.
    //
    // The generated INVOICES are deliberately left alone. They are posted
    // accounting documents with sequence numbers drawn from ir.sequence;
    // deleting them would punch a hole in the invoice series, and a gap
    // in a numbered series is exactly what an auditor asks about. The
    // link rows go, so nothing dangles.
    removed["invoice_links"] = txn.exec(
        "DELETE FROM rental_invoice_link WHERE contract_line_id IN "
        "(SELECT id FROM rental_contract_line WHERE contract_id IN "
        " (SELECT id FROM rental_contract WHERE name LIKE 'DEMO/%'))").affected_rows();

    removed["tenancies"] = txn.exec(
        "DELETE FROM rental_contract_line WHERE contract_id IN "
        "(SELECT id FROM rental_contract WHERE name LIKE 'DEMO/%')").affected_rows();

    removed["events"] = txn.exec(
        "DELETE FROM rental_event WHERE unit_id IN "
        "(SELECT id FROM rental_unit WHERE site = " + site + ") "
        "   OR contract_id IN (SELECT id FROM rental_contract WHERE name LIKE 'DEMO/%') "
        "   OR event_type = 'demo_seeded'").affected_rows();

    // Detach the kept invoices before the contract goes.
    //
    // Migration 813's FK (account_move.rental_contract_id -> rental_contract)
    // otherwise refuses the delete, and it is right to: a contract with
    // invoices against it should not silently vanish. For real data that
    // refusal is the feature. Here the invoices are deliberately kept, so
    // the link is cleared explicitly rather than the FK weakened to
    // ON DELETE SET NULL — which would let a real contract be deleted out
    // from under its invoices too.
    //
    // invoice_origin still carries the contract NAME as text, so the
    // invoice remains self-describing after the contract row is gone.
    removed["invoices_detached"] = txn.exec(
        "UPDATE account_move SET rental_contract_id = NULL "
        " WHERE rental_contract_id IN "
        "(SELECT id FROM rental_contract WHERE name LIKE 'DEMO/%')").affected_rows();

    removed["contracts"] = txn.exec(
        "DELETE FROM rental_contract WHERE name LIKE 'DEMO/%'").affected_rows();

    removed["units"] = txn.exec(
        "DELETE FROM rental_unit WHERE site = " + site).affected_rows();

    removed["expense_entries"] = txn.exec(
        "DELETE FROM rental_expense WHERE recurrence_parent_id IN "
        "(SELECT id FROM rental_expense WHERE name IN (" + names + "))").affected_rows();

    removed["expense_templates"] = txn.exec(
        "DELETE FROM rental_expense WHERE name IN (" + names + ")").affected_rows();

    txn.commit();

    auto out = status(db);
    out["removed"] = removed;
    return out;
}

} // namespace cerp::modules::rental
