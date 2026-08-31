#pragma once
// =============================================================
// core/CompanyIdentity.hpp — who the company is (docs/094)
//
// One place that answers "what is this company called, where is it, what is
// its bank account". Before this, three call sites answered it three ways:
//
//   * the invoice PDF read res_company joined to res_partner;
//   * the template preview read ir_config_parameter company.* and fell back to
//     "Demo Company Sdn. Bhd." — so the preview showed different details from
//     the document it was previewing;
//   * the portal read a third mixture.
//
// The data now lives on res_company, because ir_config_parameter has
// UNIQUE(key) and no company_id: it is single-company by construction and
// could never hold a second company's letterhead.
// =============================================================
#include <map>
#include <string>
#include <pqxx/pqxx>

namespace cerp::infrastructure { class DbConnection; }

namespace cerp::core {

struct CompanyIdentity {
    int         id = 0;
    std::string name, email, phone, website, vat;
    std::string regNumber, street, street2, street3, cityCountry;
    std::string bankName, bankAccountName, bankAccountNo, bankAddress, bankSwift;
    std::string currencyCode = "MYR";
    int         paymentTermDays = 30;

    /// Load one company. `companyId <= 0` means "the default company"
    /// (lowest id), which is what single-company installations always want.
    static CompanyIdentity load(pqxx::transaction_base& txn, int companyId = 0);

    /// Write the company_* / bank_* template variables every document uses.
    /// Callers share this so a field can never appear on the invoice but not
    /// in its preview.
    void fillVars(std::map<std::string, std::string>& vars) const;
};

/**
 * @brief Give every unattributed row an owner, while that is still unambiguous.
 *
 * `company_id IS NULL` means "shared with every company". That is a real and
 * useful state — but it is also what every row written before multi-company
 * existed happens to carry, so the moment a second company is created, the
 * first company's customers, products and locations would all appear inside it.
 * Correct by the letter of the rule, and a leak by any reasonable reading.
 *
 * Rows are therefore attributed while there is exactly ONE company, when NULL
 * and that company are indistinguishable and nothing can observe the change.
 * It is a no-op today and the difference between a clean second company and a
 * pre-populated one tomorrow. Once a second company exists the guard stops
 * firing and existing data is never guessed at again.
 *
 * Runs after schema migrations, because it walks whatever tables exist by then.
 *
 * @return number of tables in which at least one row was attributed.
 */
int backfillCompanyIds(infrastructure::DbConnection& db);

} // namespace cerp::core
