// =============================================================
// modules/portal/PortalStatement.cpp — implementation (docs/114 W4)
// =============================================================
#include "PortalStatement.hpp"
#include "Money.hpp"
#include <pqxx/pqxx>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

namespace cerp::modules::portal {

namespace {

double micros(const pqxx::field& f) {
    return f.is_null() ? 0.0
                       : cerp::core::Money::fromMicros(f.as<long long>(0)).toJson();
}

std::string fmt2(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << v;
    return o.str();
}

// Minimal HTML escaping. Customer names, references and memos land in a page,
// and at least one of them is customer-supplied (the payment memo, via the
// portal request route).
std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&#39;";  break;
            default:   o += c;
        }
    }
    return o;
}

std::string sOrEmpty(const pqxx::field& f) {
    return f.is_null() ? std::string{} : std::string(f.c_str());
}

} // anonymous namespace

bool PortalStatement::isIsoDate(const std::string& s) {
    if (s.empty()) return true;              // empty = "unbounded", allowed
    if (s.size() != 10) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) { if (s[i] != '-') return false; }
        else if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

nlohmann::json PortalStatement::build(pqxx::transaction_base& txn,
                                      int partnerId,
                                      const std::string& dateFrom,
                                      const std::string& dateTo)
{
    // Dates are validated by the caller; belt and braces here because this is
    // the function that puts them into SQL. They are bound as parameters, so
    // this is about rejecting nonsense early rather than about injection.
    const std::string from = isIsoDate(dateFrom) ? dateFrom : std::string{};
    const std::string to   = isIsoDate(dateTo)   ? dateTo   : std::string{};

    nlohmann::json out;
    out["date_from"] = from.empty() ? nlohmann::json(false) : nlohmann::json(from);
    out["date_to"]   = to.empty()   ? nlohmann::json(false) : nlohmann::json(to);

    auto p = txn.exec("SELECT name FROM res_partner WHERE id=$1", pqxx::params{partnerId});
    out["partner"] = p.empty() ? "" : std::string(p[0][0].c_str());

    // ---- opening balance: everything on the receivable BEFORE the window ----
    auto ob = txn.exec(
        "SELECT COALESCE(SUM(l.debit - l.credit),0)::bigint "
        "  FROM account_move_line l "
        "  JOIN account_move m  ON m.id = l.move_id "
        "  JOIN account_account a ON a.id = l.account_id "
        " WHERE l.partner_id = $1 AND m.state = 'posted' "
        "   AND a.account_type = 'asset_receivable' "
        // NOT `($2 = '' OR l.date < $2)`. With no lower bound that reads as
        // "every row qualifies", and the opening balance swallows the whole
        // ledger — then the statement lists the same movements again below it
        // and closes at double. With no date_from there is nothing *before*
        // the window, so the opening balance is zero by definition.
        "   AND $2 <> '' AND l.date < $2::date",
        pqxx::params{partnerId, from});
    const double opening = ob.empty() ? 0.0 : micros(ob[0][0]);
    out["opening_balance"] = opening;

    // ---- the movements in the window ----
    auto rows = txn.exec(
        "SELECT to_char(l.date,'YYYY-MM-DD') AS d, "
        "       COALESCE(m.name,'')          AS doc, "
        "       m.move_type                  AS mtype, "
        "       COALESCE(NULLIF(l.name,''), NULLIF(m.ref,''), COALESCE(m.invoice_origin,'')) AS descr, "
        "       l.debit::bigint  AS debit, "
        "       l.credit::bigint AS credit, "
        "       to_char(m.due_date,'YYYY-MM-DD') AS due "
        "  FROM account_move_line l "
        "  JOIN account_move m  ON m.id = l.move_id "
        "  JOIN account_account a ON a.id = l.account_id "
        " WHERE l.partner_id = $1 AND m.state = 'posted' "
        "   AND a.account_type = 'asset_receivable' "
        "   AND ($2 = '' OR l.date >= $2::date) "
        "   AND ($3 = '' OR l.date <= $3::date) "
        " ORDER BY l.date, m.id, l.id",
        pqxx::params{partnerId, from, to});

    nlohmann::json lines = nlohmann::json::array();
    double running = opening, totDr = 0, totCr = 0;
    for (const auto& r : rows) {
        const double dr = micros(r["debit"]);
        const double cr = micros(r["credit"]);
        running += dr - cr;
        totDr += dr; totCr += cr;

        // Say what the row IS in the customer's language, not ours.
        const std::string mt = sOrEmpty(r["mtype"]);
        std::string kind = "Entry";
        if      (mt == "out_invoice") kind = "Invoice";
        else if (mt == "out_refund")  kind = "Credit Note";
        else if (mt == "entry")       kind = (cr > 0 ? "Payment" : "Adjustment");

        lines.push_back({
            {"date",        sOrEmpty(r["d"])},
            {"document",    sOrEmpty(r["doc"])},
            {"kind",        kind},
            {"description", sOrEmpty(r["descr"])},
            {"due_date",    r["due"].is_null() ? nlohmann::json(false)
                                               : nlohmann::json(sOrEmpty(r["due"]))},
            {"debit",       dr},
            {"credit",      cr},
            {"balance",     running},
        });
    }
    out["lines"]           = lines;
    out["total_debit"]     = totDr;
    out["total_credit"]    = totCr;
    out["closing_balance"] = running;

    // ---- ageing of what is STILL OUTSTANDING as of date_to ----
    //
    // Ageing is about unpaid invoices, so it comes from amount_residual on the
    // moves rather than from the ledger lines above: a partly-paid invoice
    // should age by what is left, not by what it was.
    auto ag = txn.exec(
        "WITH ref AS (SELECT CASE WHEN $2 = '' THEN CURRENT_DATE ELSE $2::date END AS d) "
        "SELECT "
        "  COALESCE(SUM(CASE WHEN COALESCE(m.due_date, m.invoice_date) >= (SELECT d FROM ref) "
        "                    THEN m.amount_residual ELSE 0 END),0)::bigint AS b_cur, "
        "  COALESCE(SUM(CASE WHEN (SELECT d FROM ref) - COALESCE(m.due_date, m.invoice_date) BETWEEN 1 AND 30 "
        "                    THEN m.amount_residual ELSE 0 END),0)::bigint AS b30, "
        "  COALESCE(SUM(CASE WHEN (SELECT d FROM ref) - COALESCE(m.due_date, m.invoice_date) BETWEEN 31 AND 60 "
        "                    THEN m.amount_residual ELSE 0 END),0)::bigint AS b60, "
        "  COALESCE(SUM(CASE WHEN (SELECT d FROM ref) - COALESCE(m.due_date, m.invoice_date) BETWEEN 61 AND 90 "
        "                    THEN m.amount_residual ELSE 0 END),0)::bigint AS b90, "
        "  COALESCE(SUM(CASE WHEN (SELECT d FROM ref) - COALESCE(m.due_date, m.invoice_date) > 90 "
        "                    THEN m.amount_residual ELSE 0 END),0)::bigint AS b120 "
        "  FROM account_move m "
        " WHERE m.partner_id = $1 AND m.state = 'posted' "
        "   AND m.move_type = 'out_invoice' AND COALESCE(m.amount_residual,0) <> 0 "
        "   AND ($2 = '' OR m.invoice_date <= $2::date)",
        pqxx::params{partnerId, to});

    nlohmann::json aging = {
        {"current", 0.0}, {"d1_30", 0.0}, {"d31_60", 0.0},
        {"d61_90", 0.0},  {"d90_plus", 0.0}
    };
    if (!ag.empty()) {
        aging["current"]  = micros(ag[0]["b_cur"]);
        aging["d1_30"]    = micros(ag[0]["b30"]);
        aging["d31_60"]   = micros(ag[0]["b60"]);
        aging["d61_90"]   = micros(ag[0]["b90"]);
        aging["d90_plus"] = micros(ag[0]["b120"]);
    }
    out["aging"] = aging;

    // The figure the customer actually wants: what is owed right now.
    auto owed = txn.exec(
        "SELECT COALESCE(SUM(amount_residual),0)::bigint FROM account_move "
        " WHERE partner_id=$1 AND state='posted' AND move_type='out_invoice'",
        pqxx::params{partnerId});
    out["amount_due"] = owed.empty() ? 0.0 : micros(owed[0][0]);

    return out;
}

std::string PortalStatement::renderHtml(pqxx::transaction_base& txn,
                                        int partnerId,
                                        const std::string& dateFrom,
                                        const std::string& dateTo)
{
    const nlohmann::json d = build(txn, partnerId, dateFrom, dateTo);

    // Company identity for the letterhead — the customer's own company, so a
    // multi-company install letterheads each statement correctly.
    std::string coName = "";
    auto co = txn.exec(
        "SELECT c.name FROM res_company c "
        "  JOIN res_partner p ON COALESCE(p.company_id,1) = c.id "
        " WHERE p.id = $1", pqxx::params{partnerId});
    if (!co.empty()) coName = co[0][0].c_str();

    const std::string period =
        (d["date_from"].is_string() ? d["date_from"].get<std::string>() : "the beginning")
        + " to " +
        (d["date_to"].is_string() ? d["date_to"].get<std::string>() : "today");

    std::ostringstream h;
    h << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      << "<title>Statement of Account</title><style>"
         "body{font-family:Helvetica,Arial,sans-serif;font-size:12px;color:#1a1a1a;margin:28px}"
         "h1{font-size:18px;margin:0 0 2px}"
         ".sub{color:#666;margin:0 0 18px;font-size:11px}"
         ".who{margin:0 0 16px}"
         ".who b{font-size:13px}"
         "table{width:100%;border-collapse:collapse;margin-top:8px}"
         "th{text-align:left;border-bottom:1.5px solid #333;padding:6px 8px;font-size:11px;"
         "text-transform:uppercase;letter-spacing:.04em}"
         "td{padding:5px 8px;border-bottom:1px solid #e4e4e4}"
         ".num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}"
         "tr.tot td{border-top:1.5px solid #333;border-bottom:none;font-weight:bold}"
         ".due{margin-top:18px;padding:10px 12px;background:#f4f6f8;border:1px solid #dde3e8}"
         ".due .amt{font-size:16px;font-weight:bold}"
         ".aging{margin-top:16px}"
         ".aging td,.aging th{text-align:right}"
         ".aging td:first-child,.aging th:first-child{text-align:left}"
      << "</style></head><body>";

    h << "<h1>Statement of Account</h1>"
      << "<p class=\"sub\">" << esc(coName) << " &middot; " << esc(period) << "</p>"
      << "<p class=\"who\"><b>" << esc(d.value("partner", std::string{})) << "</b></p>";

    h << "<table><thead><tr>"
      << "<th>Date</th><th>Document</th><th>Type</th><th>Description</th>"
      << "<th class=\"num\">Debit</th><th class=\"num\">Credit</th><th class=\"num\">Balance</th>"
      << "</tr></thead><tbody>";

    h << "<tr><td></td><td></td><td>Opening balance</td><td></td>"
      << "<td class=\"num\"></td><td class=\"num\"></td><td class=\"num\">"
      << fmt2(d.value("opening_balance", 0.0)) << "</td></tr>";

    for (const auto& l : d["lines"]) {
        h << "<tr><td>" << esc(l.value("date", std::string{})) << "</td>"
          << "<td>" << esc(l.value("document", std::string{})) << "</td>"
          << "<td>" << esc(l.value("kind", std::string{})) << "</td>"
          << "<td>" << esc(l.value("description", std::string{})) << "</td>"
          << "<td class=\"num\">" << (l.value("debit", 0.0)  != 0.0 ? fmt2(l.value("debit", 0.0))  : "") << "</td>"
          << "<td class=\"num\">" << (l.value("credit", 0.0) != 0.0 ? fmt2(l.value("credit", 0.0)) : "") << "</td>"
          << "<td class=\"num\">" << fmt2(l.value("balance", 0.0)) << "</td></tr>";
    }

    h << "<tr class=\"tot\"><td></td><td></td><td>Closing balance</td><td></td>"
      << "<td class=\"num\">" << fmt2(d.value("total_debit", 0.0))  << "</td>"
      << "<td class=\"num\">" << fmt2(d.value("total_credit", 0.0)) << "</td>"
      << "<td class=\"num\">" << fmt2(d.value("closing_balance", 0.0)) << "</td></tr>";
    h << "</tbody></table>";

    h << "<div class=\"due\">Amount currently due "
      << "<span class=\"amt\">" << fmt2(d.value("amount_due", 0.0)) << "</span></div>";

    const auto& a = d["aging"];
    h << "<table class=\"aging\"><thead><tr><th>Ageing</th>"
      << "<th>Current</th><th>1&ndash;30</th><th>31&ndash;60</th><th>61&ndash;90</th><th>90+</th>"
      << "</tr></thead><tbody><tr><td>Outstanding</td>"
      << "<td>" << fmt2(a.value("current", 0.0))  << "</td>"
      << "<td>" << fmt2(a.value("d1_30", 0.0))    << "</td>"
      << "<td>" << fmt2(a.value("d31_60", 0.0))   << "</td>"
      << "<td>" << fmt2(a.value("d61_90", 0.0))   << "</td>"
      << "<td>" << fmt2(a.value("d90_plus", 0.0)) << "</td>"
      << "</tr></tbody></table>";

    h << "</body></html>";
    return h.str();
}

} // namespace cerp::modules::portal
