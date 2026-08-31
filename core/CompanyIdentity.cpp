// =============================================================
// core/CompanyIdentity.cpp — docs/094
// =============================================================
#include "CompanyIdentity.hpp"
#include "infrastructure/DbConnection.hpp"
#include <drogon/drogon.h>
#include <set>

namespace cerp::core {
namespace {

std::string s_(const pqxx::field& f) {
    return f.is_null() ? std::string{} : std::string(f.c_str());
}

} // namespace

CompanyIdentity CompanyIdentity::load(pqxx::transaction_base& txn, int companyId) {
    CompanyIdentity c;

    // The address falls back to the company's own partner record when the
    // letterhead lines have not been filled in — res_partner is where a company
    // keeps its postal address, and an empty letterhead should not blank the
    // document. res_partner has no street2, hence the single fallback line.
    const std::string sql =
        "SELECT c.id, c.name, COALESCE(c.email,''), COALESCE(c.phone,''), "
        "       COALESCE(c.website,''), COALESCE(c.vat,''), "
        "       COALESCE(c.reg_number,''), "
        "       COALESCE(NULLIF(c.street,''),   COALESCE(p.street,'')), "
        "       COALESCE(c.street2,''), COALESCE(c.street3,''), "
        "       COALESCE(NULLIF(c.city_country,''), COALESCE(p.city,'')), "
        "       COALESCE(c.bank_name,''), COALESCE(c.bank_account_name,''), "
        "       COALESCE(c.bank_account_no,''), COALESCE(c.bank_address,''), "
        "       COALESCE(c.bank_swift,''), "
        "       COALESCE(cur.name,'MYR'), COALESCE(c.payment_term_days,30) "
        "FROM res_company c "
        "LEFT JOIN res_partner  p   ON p.id   = c.partner_id "
        "LEFT JOIN res_currency cur ON cur.id = c.currency_id ";

    pqxx::result r = companyId > 0
        ? txn.exec(sql + "WHERE c.id = $1", pqxx::params{companyId})
        : txn.exec(sql + "ORDER BY c.id LIMIT 1");
    // A caller may name a company that does not exist (a stale session, a
    // deleted company). Fall back to the default rather than rendering a
    // document with no letterhead at all.
    if (r.empty() && companyId > 0) r = txn.exec(sql + "ORDER BY c.id LIMIT 1");
    if (r.empty()) return c;

    const auto& row = r[0];
    c.id               = row[0].as<int>(0);
    c.name             = s_(row[1]);
    c.email            = s_(row[2]);
    c.phone            = s_(row[3]);
    c.website          = s_(row[4]);
    c.vat              = s_(row[5]);
    c.regNumber        = s_(row[6]);
    c.street           = s_(row[7]);
    c.street2          = s_(row[8]);
    c.street3          = s_(row[9]);
    c.cityCountry      = s_(row[10]);
    c.bankName         = s_(row[11]);
    c.bankAccountName  = s_(row[12]);
    c.bankAccountNo    = s_(row[13]);
    c.bankAddress      = s_(row[14]);
    c.bankSwift        = s_(row[15]);
    c.currencyCode     = s_(row[16]);
    c.paymentTermDays  = row[17].as<int>(30);
    return c;
}

void CompanyIdentity::fillVars(std::map<std::string, std::string>& vars) const {
    vars["company_name"]         = name;
    vars["company_email"]        = email;
    vars["company_phone"]        = phone;
    vars["company_website"]      = website;
    vars["company_vat"]          = vat;
    vars["company_reg"]          = regNumber;
    vars["company_addr1"]        = street;
    vars["company_addr2"]        = street2;
    vars["company_addr3"]        = street3;
    vars["company_city_country"] = cityCountry;
    vars["currency_code"]        = currencyCode;
    vars["payment_term_days"]    = std::to_string(paymentTermDays);
    vars["bank_name"]            = bankName;
    vars["bank_account_name"]    = bankAccountName;
    vars["bank_account_no"]      = bankAccountNo;
    vars["bank_address"]         = bankAddress;
    vars["bank_swift"]           = bankSwift;
}

// ---- backfillCompanyIds -------------------------------------------------

int backfillCompanyIds(infrastructure::DbConnection& db) {
    int touched = 0;
    try {
        auto conn = db.acquire();
        pqxx::work txn{conn.get()};

        auto cr = txn.exec("SELECT id FROM res_company ORDER BY id");
        if (cr.size() != 1) { txn.commit(); return 0; }   // 0 or 2+: leave data alone
        const int only = cr[0][0].as<int>();

        // ir_sequence uses NULL company_id to mean a GLOBAL sequence, enforced by
        // a partial unique index (ir_sequence_code_global_idx ON (code) WHERE
        // company_id IS NULL). Attributing those would move every global sequence
        // into one company and change how document numbers are allocated.
        static const std::set<std::string> kSkip = {
            "ir_sequence",
            "res_company_users_rel",   // a link table; NULL is not meaningful there
        };

        for (const auto& r : txn.exec(
                 "SELECT c.table_name FROM information_schema.columns c "
                 "JOIN information_schema.tables t "
                 "  ON t.table_schema = c.table_schema AND t.table_name = c.table_name "
                 "WHERE c.table_schema = 'public' AND c.column_name = 'company_id' "
                 "  AND t.table_type = 'BASE TABLE' "
                 "ORDER BY c.table_name")) {
            const std::string table = r[0].c_str();
            if (kSkip.count(table)) continue;
            // The name comes from the catalogue rather than a request, but quote
            // it anyway so an unusual identifier cannot break the statement.
            const auto n = txn.exec(
                "UPDATE " + txn.quote_name(table) +
                " SET company_id = $1 WHERE company_id IS NULL",
                pqxx::params{only});
            if (n.affected_rows() > 0) {
                LOG_INFO << "[company] attributed " << n.affected_rows()
                         << " row(s) in " << table << " to company " << only;
                ++touched;
            }
        }
        txn.commit();
        if (touched > 0)
            LOG_INFO << "[company] backfill touched " << touched << " table(s)";
    } catch (const std::exception& ex) {
        // Never fatal: a database that cannot be backfilled still runs, it just
        // keeps sharing its unattributed rows.
        LOG_WARN << "[company] backfill skipped: " << ex.what();
    }
    return touched;
}

} // namespace cerp::core
