// =============================================================
// modules/rental/RentalCalendar.cpp
// =============================================================
#include "RentalCalendar.hpp"

#include "DbConnection.hpp"
#include "Errors.hpp"
#include "Money.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace cerp::modules::rental {

using namespace cerp::infrastructure;
using namespace cerp::core;

namespace {

/// Today as YYYY-MM-DD, in local time — the same clock the billing run uses.
std::string today_() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

/// "YYYY-MM" -> the first of that month, or the current month when the input
/// is not a month at all. Deliberately lenient: a bad ?month= is a broken
/// link, not a reason to refuse to draw a calendar.
std::string monthStart_(const std::string& ym) {
    if (ym.size() == 7 && ym[4] == '-' &&
        std::all_of(ym.begin(), ym.begin() + 4, ::isdigit) &&
        std::isdigit(static_cast<unsigned char>(ym[5])) &&
        std::isdigit(static_cast<unsigned char>(ym[6]))) {
        const int mm = (ym[5] - '0') * 10 + (ym[6] - '0');
        if (mm >= 1 && mm <= 12) return ym + "-01";
    }
    return today_().substr(0, 7) + "-01";
}

} // namespace

nlohmann::json RentalCalendar::month(std::shared_ptr<DbConnection> db,
                                     const std::string& ym,
                                     int companyId,
                                     int typeId) {
    const std::string from = monthStart_(ym);

    auto conn = db->acquire();
    pqxx::work txn{conn.get()};

    // PostgreSQL owns the calendar arithmetic. Month lengths and leap years
    // are not worth reimplementing, and doing it here means the bounds are
    // the same ones the overlap test uses.
    auto bounds = txn.exec(
        "SELECT to_char($1::date, 'YYYY-MM')                       AS ym, "
        "       to_char($1::date, 'YYYY-MM-DD')                    AS d_from, "
        "       to_char(($1::date + INTERVAL '1 month - 1 day')::date,"
        "               'YYYY-MM-DD')                              AS d_to, "
        "       EXTRACT(DAY FROM ($1::date + INTERVAL '1 month - 1 day'))::int AS n_days",
        pqxx::params{from});

    const std::string mLabel = bounds[0]["ym"].c_str();
    const std::string dFrom  = bounds[0]["d_from"].c_str();
    const std::string dTo    = bounds[0]["d_to"].c_str();
    const int nDays          = bounds[0]["n_days"].as<int>(30);

    // One query: every lettable unit, LEFT JOINed to the live lines that
    // touch this month. A unit with no bookings still comes back, with nulls
    // — which is the row that matters most on a booking screen.
    std::string sql =
        "SELECT u.id            AS unit_id, "
        "       u.code          AS unit_code, "
        "       COALESCE(u.name,'')  AS unit_name, "
        "       u.state         AS unit_state, "
        "       COALESCE(u.type_id,0) AS type_id, "
        "       COALESCE(t.name,'Unclassified') AS type_name, "
        "       COALESCE(t.code,'')  AS type_code, "
        "       l.id            AS line_id, "
        "       l.contract_id   AS contract_id, "
        "       to_char(l.date_start,'YYYY-MM-DD') AS d_start, "
        "       to_char(l.date_end,  'YYYY-MM-DD') AS d_end, "
        "       l.state         AS line_state, "
        "       l.partner_id    AS partner_id, "
        "       COALESCE(p.display_name, p.name, '') AS partner_name, "
        "       COALESCE(c.name,'') AS contract_ref "
        "  FROM rental_unit u "
        "  LEFT JOIN rental_unit_type t ON t.id = u.type_id "
        "  LEFT JOIN rental_contract_line l "
        "         ON l.unit_id = u.id "
        "        AND l.state IN ('pending','active') "
        "        AND l.date_start <= $2::date "
        "        AND (l.date_end IS NULL OR l.date_end >= $1::date) "
        "  LEFT JOIN res_partner    p ON p.id = l.partner_id "
        "  LEFT JOIN rental_contract c ON c.id = l.contract_id "
        // A retired unit is not lettable stock. Counting it would understate
        // occupancy for ever — the same rule the dashboard and grid apply.
        " WHERE u.active AND u.state <> 'retired' ";
    if (companyId > 0) sql += " AND (u.company_id = $3 OR u.company_id IS NULL) ";
    if (typeId    > 0) sql += " AND u.type_id = $" + std::to_string(companyId > 0 ? 4 : 3) + " ";
    sql += " ORDER BY type_name, u.code, l.date_start";

    pqxx::params params;
    params.append(dFrom);
    params.append(dTo);
    if (companyId > 0) params.append(companyId);
    if (typeId    > 0) params.append(typeId);
    auto rows = txn.exec(sql, params);

    // ---- fold the lines into per-unit day arrays -------------------------
    struct UnitAcc {
        int              id = 0;
        std::string      code, name, state, typeName, typeCode;
        int              typeId = 0;
        std::vector<int> days;          ///< 0 free, 1 let
        nlohmann::json   bookings = nlohmann::json::array();
    };
    std::vector<UnitAcc>      units;
    std::map<int, std::size_t> byUnit;   ///< unit id -> index in `units`

    for (const auto& r : rows) {
        const int uid = r["unit_id"].as<int>(0);
        auto it = byUnit.find(uid);
        if (it == byUnit.end()) {
            UnitAcc a;
            a.id       = uid;
            a.code     = r["unit_code"].c_str();
            a.name     = r["unit_name"].c_str();
            a.state    = r["unit_state"].c_str();
            a.typeId   = r["type_id"].as<int>(0);
            a.typeName = r["type_name"].c_str();
            a.typeCode = r["type_code"].c_str();
            a.days.assign(static_cast<std::size_t>(nDays), 0);
            byUnit[uid] = units.size();
            units.push_back(std::move(a));
            it = byUnit.find(uid);
        }
        if (r["line_id"].is_null()) continue;      // a unit with no bookings

        UnitAcc& acc = units[it->second];
        const std::string s = r["d_start"].c_str();
        const std::string e = r["d_end"].is_null() ? std::string() : r["d_end"].c_str();

        // Clamp the line to this month, then fill. Both ends inclusive.
        const int firstDay = (s > dFrom) ? std::stoi(s.substr(8, 2)) : 1;
        const int lastDay  = (!e.empty() && e < dTo) ? std::stoi(e.substr(8, 2)) : nDays;
        for (int d = firstDay; d <= lastDay && d <= nDays; ++d)
            acc.days[static_cast<std::size_t>(d - 1)] = 1;

        acc.bookings.push_back({
            {"line_id",     r["line_id"].as<int>(0)},
            {"contract_id", r["contract_id"].is_null() ? 0 : r["contract_id"].as<int>(0)},
            {"contract",    std::string(r["contract_ref"].c_str())},
            {"partner_id",  r["partner_id"].is_null() ? 0 : r["partner_id"].as<int>(0)},
            {"partner",     std::string(r["partner_name"].c_str())},
            {"from",        s},
            {"to",          e.empty() ? nlohmann::json(nullptr) : nlohmann::json(e)},
            {"state",       std::string(r["line_state"].c_str())},
        });
    }

    // ---- shape the reply -------------------------------------------------
    struct TypeAcc { std::string name, code; int units = 0; long long let = 0; };
    std::map<int, TypeAcc> types;

    nlohmann::json unitsJson = nlohmann::json::array();
    long long grandLet = 0;

    for (const auto& u : units) {
        long long let = 0;
        for (int d : u.days) let += d;
        grandLet += let;

        auto& t = types[u.typeId];
        t.name = u.typeName;
        t.code = u.typeCode;
        t.units += 1;
        t.let   += let;

        unitsJson.push_back({
            {"id",        u.id},
            {"code",      u.code},
            {"name",      u.name},
            {"state",     u.state},
            {"type_id",   u.typeId},
            {"type_name", u.typeName},
            {"type_code", u.typeCode},
            {"days",      u.days},
            {"let_days",  let},
            {"pct",       nDays > 0 ? (100.0 * static_cast<double>(let) / nDays) : 0.0},
            {"bookings",  u.bookings},
        });
    }

    nlohmann::json typesJson = nlohmann::json::array();
    for (const auto& [tid, t] : types) {
        const long long possible = static_cast<long long>(t.units) * nDays;
        typesJson.push_back({
            {"id",        tid},
            {"name",      t.name},
            {"code",      t.code},
            {"units",     t.units},
            {"let_days",  t.let},
            {"possible",  possible},
            {"pct",       possible > 0 ? (100.0 * static_cast<double>(t.let) / possible) : 0.0},
        });
    }

    const long long possible = static_cast<long long>(units.size()) * nDays;
    return {
        {"month",  mLabel},
        {"from",   dFrom},
        {"to",     dTo},
        {"days",   nDays},
        {"today",  today_()},
        {"types",  typesJson},
        {"units",  unitsJson},
        {"totals", {{"units",    static_cast<long long>(units.size())},
                    {"let_days", grandLet},
                    {"possible", possible},
                    {"pct",      possible > 0
                                     ? (100.0 * static_cast<double>(grandLet) / possible)
                                     : 0.0}}},
    };
}

nlohmann::json RentalCalendar::book(std::shared_ptr<DbConnection> db,
                                    const BookRequest& req) {
    if (req.unitId    <= 0) throw ValidationError("Choose a unit to book.");
    if (req.partnerId <= 0) throw ValidationError("Choose a customer for this booking.");
    if (req.dateStart.empty()) throw ValidationError("A start date is required.");
    if (!req.dateEnd.empty() && req.dateEnd < req.dateStart)
        throw ValidationError("The end date is before the start date.");

    auto conn = db->acquire();
    pqxx::work txn{conn.get()};

    // ---- the unit has to be lettable -------------------------------------
    auto u = txn.exec(
        "SELECT code, state, COALESCE(type_id,0) AS type_id, COALESCE(company_id,1) AS company_id "
        "  FROM rental_unit WHERE id = $1 AND active",
        pqxx::params{req.unitId});
    if (u.empty()) throw ValidationError("That unit no longer exists.");
    const std::string unitCode  = u[0]["code"].c_str();
    const std::string unitState = u[0]["state"].c_str();
    const int unitType          = u[0]["type_id"].as<int>(0);
    const int unitCompany       = u[0]["company_id"].as<int>(1);
    if (unitState == "retired")
        throw ValidationError("Unit " + unitCode + " is retired and cannot be let.");
    if (unitState == "maintenance")
        throw ValidationError("Unit " + unitCode + " is under maintenance. Take it out of "
                              "maintenance before booking it.");

    // ---- overlap, in words -----------------------------------------------
    // The database enforces this too (migration 820). This copy exists to name
    // the dates: a constraint violation tells the operator that something is
    // wrong, not which booking is in the way.
    auto clash = txn.exec(
        "SELECT to_char(l.date_start,'YYYY-MM-DD') AS d_start, "
        "       to_char(l.date_end,  'YYYY-MM-DD') AS d_end, "
        "       COALESCE(p.display_name, p.name, '') AS partner "
        "  FROM rental_contract_line l "
        "  LEFT JOIN res_partner p ON p.id = l.partner_id "
        " WHERE l.unit_id = $1 AND l.state IN ('pending','active') "
        "   AND daterange(l.date_start, COALESCE(l.date_end,'infinity'::date), '[]') "
        "    && daterange($2::date, COALESCE(NULLIF($3,'')::date,'infinity'::date), '[]') "
        " ORDER BY l.date_start LIMIT 1",
        pqxx::params{req.unitId, req.dateStart, req.dateEnd});
    if (!clash.empty()) {
        const std::string cs = clash[0]["d_start"].c_str();
        const std::string ce = clash[0]["d_end"].is_null()
                                   ? std::string("open-ended")
                                   : std::string(clash[0]["d_end"].c_str());
        const std::string who = clash[0]["partner"].c_str();
        throw ValidationError(
            "Unit " + unitCode + " is already let " +
            (who.empty() ? std::string() : "to " + who + " ") +
            "from " + cs + " to " + ce + ". Pick dates that do not overlap.");
    }

    // ---- the rate ---------------------------------------------------------
    long long priceMicros = 0;
    if (req.unitPrice >= 0.0) {
        priceMicros = Money::fromJson(req.unitPrice).micros();
    } else if (unitType > 0) {
        // Falling back to the type's default rate is the reason unit types
        // carry one. Nothing else in the module reads it yet.
        auto t = txn.exec("SELECT COALESCE(default_rate,0) FROM rental_unit_type WHERE id=$1",
                          pqxx::params{unitType});
        if (!t.empty()) priceMicros = t[0][0].as<long long>(0);
    }

    // ---- the contract -----------------------------------------------------
    int contractId = req.contractId;
    if (contractId > 0) {
        auto c = txn.exec("SELECT partner_id FROM rental_contract WHERE id=$1",
                          pqxx::params{contractId});
        if (c.empty()) throw ValidationError("That contract no longer exists.");
        if (c[0][0].as<int>(0) != req.partnerId)
            throw ValidationError("That contract belongs to a different customer.");
    } else {
        // A booking with no contract has nowhere to keep its billing terms,
        // so one is opened. It starts ACTIVE rather than draft: the operator
        // has just let a unit, and a draft contract would leave the unit
        // reserved with no way to invoice it.
        auto ins = txn.exec(
            "INSERT INTO rental_contract (name, partner_id, state, date_start, "
            "                             billing_period, billing_unit, billing_interval, "
            "                             company_id) "
            "VALUES ($1, $2, 'active', $3::date, $4, $5, 1, $6) RETURNING id",
            pqxx::params{
                std::string("BK-") + unitCode + "-" + req.dateStart,
                req.partnerId,
                req.dateStart,
                req.dateEnd.empty() ? std::string("monthly") : std::string("oneoff"),
                std::string("month"),
                req.companyId > 0 ? req.companyId : unitCompany});
        contractId = ins[0][0].as<int>();
    }

    // ---- the line ---------------------------------------------------------
    // A dated booking bills ONCE; an open-ended one is a tenancy and recurs.
    // Getting this wrong in the other direction would have the billing run
    // invoicing a three-day locker hire every month for ever.
    const std::string mode = !req.billingMode.empty()
                                 ? req.billingMode
                                 : (req.dateEnd.empty() ? "recurring" : "oneoff");
    // pending until it starts, active once it has. The unit-state trigger
    // turns that into reserved / occupied on its own.
    const std::string state = (req.dateStart <= today_()) ? "active" : "pending";

    auto line = txn.exec(
        "INSERT INTO rental_contract_line "
        "  (contract_id, unit_id, partner_id, date_start, date_end, unit_price, "
        "   billing_mode, state, company_id) "
        "VALUES ($1,$2,$3,$4::date, NULLIF($5,'')::date, $6, $7, $8, $9) RETURNING id",
        pqxx::params{contractId, req.unitId, req.partnerId, req.dateStart,
                     req.dateEnd, priceMicros, mode, state,
                     req.companyId > 0 ? req.companyId : unitCompany});
    const int lineId = line[0][0].as<int>();

    txn.commit();

    return {
        {"ok",          true},
        {"line_id",     lineId},
        {"contract_id", contractId},
        {"unit",        unitCode},
        {"from",        req.dateStart},
        {"to",          req.dateEnd.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(req.dateEnd)},
        {"state",       state},
        {"billing_mode", mode},
    };
}

} // namespace cerp::modules::rental
