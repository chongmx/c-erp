// =============================================================
// modules/account/AccountModule.cpp
//
// Phase 6 — minimal double-entry bookkeeping module.
// Implementation file — all inner classes and method bodies.
// =============================================================
#include "AccountModule.hpp"
#include "BaseModel.hpp"
#include "RecordRuleSql.hpp"
#include "IrSequence.hpp"
#include "PaymentAllocation.hpp"
#include <map>
#include "TaxEngine.hpp"
#include "TaxHelpers.hpp"
#include "DecimalPrecision.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "AuditService.hpp"
#include "AccountViews.hpp"
#include "MailHelpers.hpp"
#include "Errors.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace odoo::modules::account {

// ================================================================
// helpers
// ================================================================
static int m2oToId_(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer())
        return v[0].get<int>();
    return 0;
}

static std::string currentDate_() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
    return std::string(buf);
}

static std::string idsArray_(const std::vector<int>& ids) {
    std::string s = "{";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) s += ","; s += std::to_string(ids[i]);
    }
    return s + "}";
}

// ================================================================
// 1. MODELS
// ================================================================

// ----------------------------------------------------------------
// AccountAccount — account.account
// ----------------------------------------------------------------
class AccountAccount : public core::BaseModel<AccountAccount> {
public:
    ODOO_MODEL("account.account", "account_account")

    std::string name;
    std::string code;
    std::string accountType   = "asset_current";
    std::string internalGroup = "asset";
    int         currencyId    = 0;
    int         companyId     = 1;
    bool        reconcile     = false;
    bool        active        = true;
    std::string note;

    explicit AccountAccount(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAccount>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",           core::FieldType::Char,      "Account Name",   true});
        fieldRegistry_.add({"code",           core::FieldType::Char,      "Code",           true});
        fieldRegistry_.add({"account_type",   core::FieldType::Selection, "Type"});
        fieldRegistry_.add({"internal_group", core::FieldType::Selection, "Internal Group"});
        fieldRegistry_.add({"currency_id",    core::FieldType::Many2one,  "Currency",       false, false, true, false, "res.currency"});
        fieldRegistry_.add({"company_id",     core::FieldType::Many2one,  "Company",        true,  false, true, false, "res.company"});
        fieldRegistry_.add({"reconcile",      core::FieldType::Boolean,   "Can Reconcile"});
        fieldRegistry_.add({"active",         core::FieldType::Boolean,   "Active"});
        fieldRegistry_.add({"note",           core::FieldType::Text,      "Notes"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]           = name;
        j["code"]           = code;
        j["account_type"]   = accountType;
        j["internal_group"] = internalGroup;
        j["currency_id"]    = currencyId > 0 ? nlohmann::json(currencyId) : nlohmann::json(false);
        j["company_id"]     = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["reconcile"]      = reconcile;
        j["active"]         = active;
        j["note"]           = note.empty() ? nlohmann::json(nullptr) : nlohmann::json(note);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))           name          = j["name"].is_string() ? j["name"].get<std::string>() : name;
        if (j.contains("code"))           code          = j["code"].is_string() ? j["code"].get<std::string>() : code;
        if (j.contains("account_type"))   accountType   = j["account_type"].is_string() ? j["account_type"].get<std::string>() : accountType;
        if (j.contains("internal_group")) internalGroup = j["internal_group"].is_string() ? j["internal_group"].get<std::string>() : internalGroup;
        if (j.contains("currency_id"))    currencyId    = m2oToId_(j["currency_id"]);
        if (j.contains("company_id"))     companyId     = m2oToId_(j["company_id"]);
        if (j.contains("reconcile")  && j["reconcile"].is_boolean())  reconcile = j["reconcile"].get<bool>();
        if (j.contains("active")     && j["active"].is_boolean())     active    = j["active"].get<bool>();
        if (j.contains("note")       && j["note"].is_string())        note      = j["note"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Account name is required");
        if (code.empty()) e.push_back("Account code is required");
        return e;
    }
};

// ----------------------------------------------------------------
// AccountJournal — account.journal
// ----------------------------------------------------------------
class AccountJournal : public core::BaseModel<AccountJournal> {
public:
    ODOO_MODEL("account.journal", "account_journal")

    std::string name;
    std::string code;
    std::string type             = "general";
    int         currencyId       = 0;
    int         companyId        = 1;
    int         defaultAccountId = 0;
    int         sequence         = 10;
    bool        active           = true;

    explicit AccountJournal(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountJournal>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",               core::FieldType::Char,     "Journal Name",    true});
        fieldRegistry_.add({"code",               core::FieldType::Char,     "Short Code",      true});
        fieldRegistry_.add({"type",               core::FieldType::Selection,"Type"});
        fieldRegistry_.add({"currency_id",        core::FieldType::Many2one, "Currency",        false, false, true, false, "res.currency"});
        fieldRegistry_.add({"company_id",         core::FieldType::Many2one, "Company",         true,  false, true, false, "res.company"});
        fieldRegistry_.add({"default_account_id", core::FieldType::Many2one, "Default Account", false, false, true, false, "account.account"});
        fieldRegistry_.add({"sequence",           core::FieldType::Integer,  "Sequence"});
        fieldRegistry_.add({"active",             core::FieldType::Boolean,  "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]               = name;
        j["code"]               = code;
        j["type"]               = type;
        j["currency_id"]        = currencyId       > 0 ? nlohmann::json(currencyId)       : nlohmann::json(false);
        j["company_id"]         = companyId        > 0 ? nlohmann::json(companyId)        : nlohmann::json(false);
        j["default_account_id"] = defaultAccountId > 0 ? nlohmann::json(defaultAccountId) : nlohmann::json(false);
        j["sequence"]           = sequence;
        j["active"]             = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))               name             = j["name"].is_string() ? j["name"].get<std::string>() : name;
        if (j.contains("code"))               code             = j["code"].is_string() ? j["code"].get<std::string>() : code;
        if (j.contains("type"))               type             = j["type"].is_string() ? j["type"].get<std::string>() : type;
        if (j.contains("currency_id"))        currencyId       = m2oToId_(j["currency_id"]);
        if (j.contains("company_id"))         companyId        = m2oToId_(j["company_id"]);
        if (j.contains("default_account_id")) defaultAccountId = m2oToId_(j["default_account_id"]);
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (j.contains("active")   && j["active"].is_boolean())  active   = j["active"].get<bool>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Journal name is required");
        if (code.empty()) e.push_back("Journal code is required");
        return e;
    }
};

// ----------------------------------------------------------------
// AccountTax — account.tax
// ----------------------------------------------------------------
class AccountTax : public core::BaseModel<AccountTax> {
public:
    ODOO_MODEL("account.tax", "account_tax")

    std::string name;
    double      amount       = 0.0;
    std::string amountType   = "percent";
    std::string typeTaxUse   = "sale";
    bool        priceInclude = false;
    int         companyId    = 1;
    bool        active       = true;
    std::string description;
    std::string taxGroup;   // SST-02: 'sales' | 'service' | 'other'

    explicit AccountTax(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountTax>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",          core::FieldType::Char,      "Tax Name",         true});
        fieldRegistry_.add({"amount",        core::FieldType::Float,     "Amount"});
        fieldRegistry_.add({"amount_type",   core::FieldType::Selection, "Computation"});
        fieldRegistry_.add({"type_tax_use",  core::FieldType::Selection, "Tax Scope"});
        fieldRegistry_.add({"price_include", core::FieldType::Boolean,   "Price Included"});
        fieldRegistry_.add({"company_id",    core::FieldType::Many2one,  "Company",          true, false, true, false, "res.company"});
        fieldRegistry_.add({"active",        core::FieldType::Boolean,   "Active"});
        fieldRegistry_.add({"description",   core::FieldType::Char,      "Label on Invoice"});
        fieldRegistry_.add({"tax_group",     core::FieldType::Selection, "SST Category"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]          = name;
        j["amount"]        = amount;
        j["amount_type"]   = amountType;
        j["type_tax_use"]  = typeTaxUse;
        j["price_include"] = priceInclude;
        j["company_id"]    = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]        = active;
        j["description"]   = description.empty() ? nlohmann::json(nullptr) : nlohmann::json(description);
        j["tax_group"]     = taxGroup;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))          name         = j["name"].is_string() ? j["name"].get<std::string>() : name;
        if (j.contains("amount")     && j["amount"].is_number())          amount       = j["amount"].get<double>();
        if (j.contains("amount_type"))   amountType   = j["amount_type"].is_string() ? j["amount_type"].get<std::string>() : amountType;
        if (j.contains("type_tax_use"))  typeTaxUse   = j["type_tax_use"].is_string() ? j["type_tax_use"].get<std::string>() : typeTaxUse;
        if (j.contains("price_include") && j["price_include"].is_boolean()) priceInclude = j["price_include"].get<bool>();
        if (j.contains("company_id"))    companyId    = m2oToId_(j["company_id"]);
        if (j.contains("active")     && j["active"].is_boolean())          active       = j["active"].get<bool>();
        if (j.contains("description"))   description  = j["description"].is_string() ? j["description"].get<std::string>() : description;
        if (j.contains("tax_group"))     taxGroup     = j["tax_group"].is_string() ? j["tax_group"].get<std::string>() : taxGroup;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Tax name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// AccountMove — account.move
// ----------------------------------------------------------------
class AccountMove : public core::BaseModel<AccountMove> {
public:
    ODOO_MODEL("account.move", "account_move")

    std::string name          = "/";
    std::string ref;
    std::string narration;
    std::string moveType      = "entry";
    std::string state         = "draft";
    std::string date;
    std::string invoiceDate;
    std::string dueDate;
    int         journalId     = 0;
    int         partnerId     = 0;
    int         companyId     = 1;
    int         currencyId    = 0;
    std::string paymentState  = "not_paid";
    double      amountUntaxed = 0.0;
    double      amountTax     = 0.0;
    double      amountTotal   = 0.0;
    double      amountResidual= 0.0;
    int         paymentTermId  = 0;
    std::string invoiceOrigin;

    explicit AccountMove(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountMove>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",            core::FieldType::Char,      "Number"});
        fieldRegistry_.add({"ref",             core::FieldType::Char,      "Reference"});
        fieldRegistry_.add({"narration",       core::FieldType::Text,      "Notes"});
        fieldRegistry_.add({"move_type",       core::FieldType::Selection, "Type"});
        fieldRegistry_.add({"state",           core::FieldType::Selection, "Status",            false, true});
        fieldRegistry_.add({"date",            core::FieldType::Date,      "Date",              true});
        fieldRegistry_.add({"invoice_date",    core::FieldType::Date,      "Invoice Date"});
        fieldRegistry_.add({"due_date",        core::FieldType::Date,      "Due Date"});
        fieldRegistry_.add({"journal_id",      core::FieldType::Many2one,  "Journal",           true,  false, true, false, "account.journal"});
        fieldRegistry_.add({"partner_id",      core::FieldType::Many2one,  "Partner",           false, false, true, false, "res.partner"});
        fieldRegistry_.add({"company_id",      core::FieldType::Many2one,  "Company",           true,  false, true, false, "res.company"});
        fieldRegistry_.add({"currency_id",     core::FieldType::Many2one,  "Currency",          false, false, true, false, "res.currency"});
        fieldRegistry_.add({"payment_state",   core::FieldType::Selection, "Payment Status",    false, true});
        fieldRegistry_.add({"amount_untaxed",  core::FieldType::Monetary,  "Untaxed Amount",    false, true});
        fieldRegistry_.add({"amount_tax",      core::FieldType::Monetary,  "Tax",               false, true});
        fieldRegistry_.add({"amount_total",    core::FieldType::Monetary,  "Total",             false, true});
        fieldRegistry_.add({"amount_residual",  core::FieldType::Monetary,  "Amount Due",        false, true});
        fieldRegistry_.add({"payment_term_id",  core::FieldType::Many2one,  "Payment Terms",     false, false, true, false, "account.payment.term"});
        fieldRegistry_.add({"invoice_origin",   core::FieldType::Char,      "Source Document"});
        // P2: BIGINT micro-units (migration 911)
        fieldRegistry_.markScaled({"amount_untaxed", "amount_tax",
                                   "amount_total", "amount_residual"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]            = name;
        j["ref"]             = ref.empty()         ? nlohmann::json(nullptr)      : nlohmann::json(ref);
        j["narration"]       = narration.empty()   ? nlohmann::json(nullptr)      : nlohmann::json(narration);
        j["move_type"]       = moveType;
        j["state"]           = state;
        j["date"]            = date.empty()        ? nlohmann::json(currentDate_()): nlohmann::json(date);
        j["invoice_date"]    = invoiceDate.empty() ? nlohmann::json(nullptr)      : nlohmann::json(invoiceDate);
        j["due_date"]        = dueDate.empty()     ? nlohmann::json(nullptr)      : nlohmann::json(dueDate);
        j["journal_id"]      = journalId  > 0 ? nlohmann::json(journalId)  : nlohmann::json(false);
        j["partner_id"]      = partnerId  > 0 ? nlohmann::json(partnerId)  : nlohmann::json(false);
        j["company_id"]      = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["currency_id"]     = currencyId > 0 ? nlohmann::json(currencyId) : nlohmann::json(false);
        j["payment_state"]   = paymentState;
        j["amount_untaxed"]  = amountUntaxed;
        j["amount_tax"]      = amountTax;
        j["amount_total"]    = amountTotal;
        j["amount_residual"] = amountResidual;
        j["payment_term_id"] = paymentTermId > 0 ? nlohmann::json(paymentTermId) : nlohmann::json(false);
        j["invoice_origin"]  = invoiceOrigin.empty() ? nlohmann::json(nullptr) : nlohmann::json(invoiceOrigin);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")            && j["name"].is_string())          name           = j["name"].get<std::string>();
        if (j.contains("ref")             && j["ref"].is_string())           ref            = j["ref"].get<std::string>();
        if (j.contains("narration")       && j["narration"].is_string())     narration      = j["narration"].get<std::string>();
        if (j.contains("move_type")       && j["move_type"].is_string())     moveType       = j["move_type"].get<std::string>();
        if (j.contains("state")           && j["state"].is_string())         state          = j["state"].get<std::string>();
        if (j.contains("date")            && j["date"].is_string())          date           = j["date"].get<std::string>();
        if (j.contains("invoice_date")    && j["invoice_date"].is_string())  invoiceDate    = j["invoice_date"].get<std::string>();
        if (j.contains("due_date")        && j["due_date"].is_string())      dueDate        = j["due_date"].get<std::string>();
        if (j.contains("journal_id"))      journalId      = m2oToId_(j["journal_id"]);
        if (j.contains("partner_id"))      partnerId      = m2oToId_(j["partner_id"]);
        if (j.contains("company_id"))      companyId      = m2oToId_(j["company_id"]);
        if (j.contains("currency_id"))     currencyId     = m2oToId_(j["currency_id"]);
        if (j.contains("payment_state")   && j["payment_state"].is_string()) paymentState   = j["payment_state"].get<std::string>();
        if (j.contains("amount_untaxed")  && j["amount_untaxed"].is_number())  amountUntaxed  = j["amount_untaxed"].get<double>();
        if (j.contains("amount_tax")      && j["amount_tax"].is_number())      amountTax      = j["amount_tax"].get<double>();
        if (j.contains("amount_total")    && j["amount_total"].is_number())    amountTotal    = j["amount_total"].get<double>();
        if (j.contains("amount_residual") && j["amount_residual"].is_number()) amountResidual = j["amount_residual"].get<double>();
        if (j.contains("payment_term_id"))                                      paymentTermId  = m2oToId_(j["payment_term_id"]);
        if (j.contains("invoice_origin")  && j["invoice_origin"].is_string())  invoiceOrigin  = j["invoice_origin"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (journalId <= 0) e.push_back("Journal is required");
        return e;
    }
};

// ----------------------------------------------------------------
// AccountMoveLine — account.move.line
// ----------------------------------------------------------------
class AccountMoveLine : public core::BaseModel<AccountMoveLine> {
public:
    ODOO_MODEL("account.move.line", "account_move_line")

    int         moveId         = 0;
    int         accountId      = 0;
    int         journalId      = 0;
    int         companyId      = 0;
    std::string date;
    std::string name;
    std::string ref;
    int         partnerId      = 0;
    double      debit          = 0.0;
    double      credit         = 0.0;
    double      amountCurrency = 0.0;
    double      quantity       = 1.0;
    double      priceUnit      = 0.0;
    std::string displayType;   // '' | 'line_section' | 'line_note'
    int         taxLineId      = 0;
    // P3: which taxes this PRODUCT line is subject to, as a JSON id array.
    // Distinct from taxLineId, which marks a line that IS a generated tax.
    std::string taxIdsJson     = "[]";
    bool        reconciled     = false;

    explicit AccountMoveLine(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountMoveLine>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"move_id",         core::FieldType::Many2one, "Journal Entry",  true,  false, true, false, "account.move"});
        fieldRegistry_.add({"account_id",      core::FieldType::Many2one, "Account",        true,  false, true, false, "account.account"});
        fieldRegistry_.add({"journal_id",      core::FieldType::Many2one, "Journal",        false, false, true, false, "account.journal"});
        fieldRegistry_.add({"company_id",      core::FieldType::Many2one, "Company",        false, false, true, false, "res.company"});
        fieldRegistry_.add({"date",            core::FieldType::Date,     "Date"});
        fieldRegistry_.add({"name",            core::FieldType::Char,     "Label"});
        fieldRegistry_.add({"ref",             core::FieldType::Char,     "Reference"});
        fieldRegistry_.add({"partner_id",      core::FieldType::Many2one, "Partner",        false, false, true, false, "res.partner"});
        fieldRegistry_.add({"debit",           core::FieldType::Monetary, "Debit"});
        fieldRegistry_.add({"credit",          core::FieldType::Monetary, "Credit"});
        fieldRegistry_.add({"amount_currency", core::FieldType::Monetary, "Amount Currency"});
        fieldRegistry_.add({"quantity",        core::FieldType::Float,    "Quantity"});
        fieldRegistry_.add({"price_unit",      core::FieldType::Float,    "Unit Price"});
        fieldRegistry_.add({"display_type",    core::FieldType::Char,     "Display Type"});
        fieldRegistry_.add({"tax_line_id",     core::FieldType::Many2one, "Tax",            false, false, true, false, "account.tax"});
        // Migration 1000 added the column, but without this registration
        // BaseModel::write() dropped the value silently — the invoice form's
        // tax picker wrote and the line came back '[]'.
        fieldRegistry_.add({"tax_ids_json",    core::FieldType::Char,     "Taxes"});
        fieldRegistry_.add({"reconciled",      core::FieldType::Boolean,  "Reconciled"});
        // P2: BIGINT micro-units (migration 910). `balance` is generated in
        // the DB and never written from here, so it is not listed.
        fieldRegistry_.setPrecision(core::DecimalPrecision::kProductPrice, {"price_unit"});
        fieldRegistry_.markScaled({"debit", "credit", "amount_currency",
                                   "quantity", "price_unit"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["move_id"]         = moveId    > 0 ? nlohmann::json(moveId)    : nlohmann::json(false);
        j["account_id"]      = accountId > 0 ? nlohmann::json(accountId) : nlohmann::json(false);
        j["journal_id"]      = journalId > 0 ? nlohmann::json(journalId) : nlohmann::json(false);
        j["company_id"]      = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["date"]            = date.empty() ? nlohmann::json(nullptr)    : nlohmann::json(date);
        j["name"]            = name.empty() ? nlohmann::json(nullptr)    : nlohmann::json(name);
        j["ref"]             = ref.empty()  ? nlohmann::json(nullptr)    : nlohmann::json(ref);
        j["partner_id"]      = partnerId  > 0 ? nlohmann::json(partnerId)  : nlohmann::json(false);
        j["debit"]           = debit;
        j["credit"]          = credit;
        j["amount_currency"] = amountCurrency;
        j["quantity"]        = quantity;
        j["price_unit"]      = priceUnit;
        j["display_type"]    = displayType.empty() ? nlohmann::json("") : nlohmann::json(displayType);
        j["tax_line_id"]     = taxLineId > 0 ? nlohmann::json(taxLineId) : nlohmann::json(false);
        j["tax_ids_json"]    = taxIdsJson.empty() ? nlohmann::json("[]") : nlohmann::json(taxIdsJson);
        j["reconciled"]      = reconciled;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("move_id"))          moveId         = m2oToId_(j["move_id"]);
        if (j.contains("account_id"))       accountId      = m2oToId_(j["account_id"]);
        if (j.contains("journal_id"))       journalId      = m2oToId_(j["journal_id"]);
        if (j.contains("company_id"))       companyId      = m2oToId_(j["company_id"]);
        if (j.contains("date")          && j["date"].is_string())          date           = j["date"].get<std::string>();
        if (j.contains("name")          && j["name"].is_string())          name           = j["name"].get<std::string>();
        if (j.contains("ref")           && j["ref"].is_string())           ref            = j["ref"].get<std::string>();
        if (j.contains("partner_id"))       partnerId      = m2oToId_(j["partner_id"]);
        if (j.contains("debit")         && j["debit"].is_number())         debit          = j["debit"].get<double>();
        if (j.contains("credit")        && j["credit"].is_number())        credit         = j["credit"].get<double>();
        if (j.contains("amount_currency") && j["amount_currency"].is_number()) amountCurrency = j["amount_currency"].get<double>();
        if (j.contains("quantity")      && j["quantity"].is_number())      quantity       = j["quantity"].get<double>();
        if (j.contains("price_unit")    && j["price_unit"].is_number())   priceUnit      = j["price_unit"].get<double>();
        if (j.contains("display_type")  && j["display_type"].is_string()) displayType    = j["display_type"].get<std::string>();
        if (j.contains("tax_line_id"))      taxLineId      = m2oToId_(j["tax_line_id"]);
        // The picker sends "[]" to clear, so an empty string is normalised
        // rather than treated as "no change" — otherwise a tax could be added
        // but never removed.
        if (j.contains("tax_ids_json") && j["tax_ids_json"].is_string()) {
            taxIdsJson = j["tax_ids_json"].get<std::string>();
            if (taxIdsJson.empty()) taxIdsJson = "[]";
        }
        if (j.contains("reconciled")    && j["reconciled"].is_boolean())   reconciled     = j["reconciled"].get<bool>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (moveId    <= 0) e.push_back("Journal entry is required");
        if (accountId <= 0) e.push_back("Account is required");
        return e;
    }
};

// ----------------------------------------------------------------
// AccountPayment — account.payment
// ----------------------------------------------------------------
class AccountPayment : public core::BaseModel<AccountPayment> {
public:
    ODOO_MODEL("account.payment", "account_payment")

    std::string name         = "/";
    std::string date;
    int         journalId    = 0;
    int         partnerId    = 0;
    int         companyId    = 1;
    int         currencyId   = 0;
    double      amount       = 0.0;
    std::string paymentType  = "inbound";
    std::string partnerType  = "customer";
    std::string state        = "draft";
    int         moveId       = 0;
    std::string memo;

    explicit AccountPayment(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountPayment>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",         core::FieldType::Char,      "Payment Reference"});
        fieldRegistry_.add({"date",         core::FieldType::Date,      "Date",             true});
        fieldRegistry_.add({"journal_id",   core::FieldType::Many2one,  "Journal",          true,  false, true, false, "account.journal"});
        fieldRegistry_.add({"partner_id",   core::FieldType::Many2one,  "Partner",          false, false, true, false, "res.partner"});
        fieldRegistry_.add({"company_id",   core::FieldType::Many2one,  "Company",          true,  false, true, false, "res.company"});
        fieldRegistry_.add({"currency_id",  core::FieldType::Many2one,  "Currency",         false, false, true, false, "res.currency"});
        fieldRegistry_.add({"amount",       core::FieldType::Monetary,  "Amount"});
        fieldRegistry_.add({"payment_type", core::FieldType::Selection, "Payment Type"});
        fieldRegistry_.add({"partner_type", core::FieldType::Selection, "Partner Type"});
        fieldRegistry_.add({"state",        core::FieldType::Selection, "Status",           false, true});
        fieldRegistry_.add({"move_id",      core::FieldType::Many2one,  "Journal Entry",    false, true,  true, false, "account.move"});
        fieldRegistry_.add({"memo",         core::FieldType::Char,      "Memo"});
        fieldRegistry_.markScaled({"amount"});   // P2: migration 912
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]         = name;
        j["date"]         = date.empty() ? nlohmann::json(currentDate_()) : nlohmann::json(date);
        j["journal_id"]   = journalId  > 0 ? nlohmann::json(journalId)  : nlohmann::json(false);
        j["partner_id"]   = partnerId  > 0 ? nlohmann::json(partnerId)  : nlohmann::json(false);
        j["company_id"]   = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["currency_id"]  = currencyId > 0 ? nlohmann::json(currencyId) : nlohmann::json(false);
        j["amount"]       = amount;
        j["payment_type"] = paymentType;
        j["partner_type"] = partnerType;
        j["state"]        = state;
        j["move_id"]      = moveId > 0 ? nlohmann::json(moveId) : nlohmann::json(false);
        j["memo"]         = memo.empty() ? nlohmann::json(nullptr) : nlohmann::json(memo);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")         && j["name"].is_string())         name        = j["name"].get<std::string>();
        if (j.contains("date")         && j["date"].is_string())         date        = j["date"].get<std::string>();
        if (j.contains("journal_id"))   journalId   = m2oToId_(j["journal_id"]);
        if (j.contains("partner_id"))   partnerId   = m2oToId_(j["partner_id"]);
        if (j.contains("company_id"))   companyId   = m2oToId_(j["company_id"]);
        if (j.contains("currency_id"))  currencyId  = m2oToId_(j["currency_id"]);
        if (j.contains("amount")       && j["amount"].is_number())       amount      = j["amount"].get<double>();
        if (j.contains("payment_type") && j["payment_type"].is_string()) paymentType = j["payment_type"].get<std::string>();
        if (j.contains("partner_type") && j["partner_type"].is_string()) partnerType = j["partner_type"].get<std::string>();
        if (j.contains("state")        && j["state"].is_string())        state       = j["state"].get<std::string>();
        if (j.contains("move_id"))      moveId      = m2oToId_(j["move_id"]);
        if (j.contains("memo")         && j["memo"].is_string())         memo        = j["memo"].get<std::string>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (journalId <= 0) e.push_back("Journal is required");
        return e;
    }
};

// ----------------------------------------------------------------
// AccountPaymentTerm — account.payment.term
// ----------------------------------------------------------------
class AccountPaymentTerm : public core::BaseModel<AccountPaymentTerm> {
public:
    ODOO_MODEL("account.payment.term", "account_payment_term")

    std::string name;
    std::string note;
    std::string linesJson = "[{\"days\":0,\"value\":\"balance\",\"value_amount\":0}]";
    bool        active    = true;

    explicit AccountPaymentTerm(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountPaymentTerm>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char,    "Payment Term", true});
        fieldRegistry_.add({"note",       core::FieldType::Text,    "Notes"});
        fieldRegistry_.add({"lines_json", core::FieldType::Text,    "Terms (JSON)"});
        fieldRegistry_.add({"active",     core::FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["note"]       = note.empty() ? nlohmann::json(nullptr) : nlohmann::json(note);
        j["lines_json"] = linesJson;
        j["active"]     = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")       && j["name"].is_string())       name      = j["name"].get<std::string>();
        if (j.contains("note")       && j["note"].is_string())       note      = j["note"].get<std::string>();
        if (j.contains("lines_json") && j["lines_json"].is_string()) linesJson = j["lines_json"].get<std::string>();
        if (j.contains("active")     && j["active"].is_boolean())    active    = j["active"].get<bool>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Payment term name is required");
        return e;
    }
};


// ================================================================
// 2. VIEWMODELS
// ================================================================

// ----------------------------------------------------------------
// AccountViewModel<T> — generic CRTP ViewModel for simple CRUD
// ----------------------------------------------------------------
template<typename TModel>
class AccountViewModel : public core::BaseViewModel {
public:
    explicit AccountViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db))
    {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_METHOD("web_read",        handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("search",          handleSearch)
    }

    std::string modelName() const override { return TModel::MODEL_NAME; }

protected:
    std::shared_ptr<infrastructure::DbConnection> db_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const int newId = proto.create(v);
        if (infrastructure::AuditService::ready() && newId > 0)
            infrastructure::AuditService::instance().log(
                TModel::MODEL_NAME, "create", {newId}, ctx.uid);
        return newId;
    }
    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto result = proto.write(call.ids(), v);
        if (infrastructure::AuditService::ready() && !call.ids().empty())
            infrastructure::AuditService::instance().log(
                TModel::MODEL_NAME, "write", call.ids(), ctx.uid);
        return result;
    }
    nlohmann::json handleUnlink(const core::CallKwArgs& call) {
        TModel proto(db_);
        const auto ctx = extractContext_(call);
        proto.setUserContext(ctx);
        const auto ids = call.ids();
        const auto result = proto.unlink(ids);
        if (infrastructure::AuditService::ready() && !ids.empty())
            infrastructure::AuditService::instance().log(
                TModel::MODEL_NAME, "unlink", ids, ctx.uid);
        return result;
    }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());  // schema metadata — no rules needed
    }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const core::CallKwArgs& call) {
        TModel proto(db_);
        proto.setUserContext(extractContext_(call));
        auto ids = proto.search(call.domain(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
        nlohmann::json arr = nlohmann::json::array();
        for (int id : ids) arr.push_back(id);
        return arr;
    }
};

// ----------------------------------------------------------------
// AccountMoveViewModel — adds action_post / button_cancel
// ----------------------------------------------------------------
class AccountMoveViewModel : public AccountViewModel<AccountMove> {
public:
    explicit AccountMoveViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : AccountViewModel<AccountMove>(std::move(db))
    {
        REGISTER_METHOD("action_post",              handleActionPost)
        REGISTER_METHOD("button_cancel",            handleButtonCancel)
        REGISTER_METHOD("action_reverse",           handleActionReverse)
        REGISTER_METHOD("button_draft",             handleButtonDraft)
        REGISTER_METHOD("recompute_totals",         handleRecomputeTotals)
        REGISTER_METHOD("action_register_payment",  handleActionRegisterPayment)
    }

    std::string modelName() const override { return "account.move"; }

private:
    nlohmann::json handleActionPost(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            auto r = txn.exec(
                "SELECT state, move_type, journal_id, date "
                "FROM account_move WHERE id = $1",
                pqxx::params{id});
            if (r.empty()) throw std::runtime_error("Move not found: " + std::to_string(id));

            std::string state    = r[0][0].c_str();
            std::string moveType = r[0][1].c_str();
            int         jid      = r[0][2].as<int>();
            std::string date     = r[0][3].c_str();

            if (state != "draft")
                throw std::runtime_error("Only draft entries can be posted");

            // Lock dates (docs/088): a closed period is closed. 'account.lock_date'
            // blocks everyone; 'account.tax_lock_date' additionally protects periods
            // whose tax return has been filed. Both are plain config parameters.
            {
                auto lock = txn.exec(
                    "SELECT COALESCE(MAX(value),'') FROM ir_config_parameter "
                    "WHERE key IN ('account.lock_date','account.tax_lock_date') "
                    "AND value <> '' AND value >= $1", pqxx::params{date});
                if (!lock.empty() && !lock[0][0].is_null()) {
                    const std::string blocking = lock[0][0].c_str();
                    if (!blocking.empty())
                        throw infrastructure::ValidationError(
                            "This period is locked (entries on or before " + blocking +
                            " cannot be posted). Change the date or move the lock date.");
                }
            }

            // Validate balance for journal entries
            if (moveType == "entry") {
                auto bal = txn.exec(
                    "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0) "
                    "FROM account_move_line WHERE move_id = $1",
                    pqxx::params{id});
                double diff = bal[0][0].as<double>();
                if (std::abs(diff) > 0.001)
                    throw std::runtime_error("Journal entry is not balanced (debit ≠ credit)");
            }

            // Get journal code
            auto jrow = txn.exec("SELECT code FROM account_journal WHERE id = $1",
                                  pqxx::params{jid});
            std::string jcode = jrow.empty() ? "MISC" : std::string(jrow[0][0].c_str());

            // P4: invoice numbering via ir.sequence.
            //
            // What this replaces was genuinely unsafe: the number was
            // `COUNT(*) of posted moves for this journal+year + 1`, computed
            // with no lock. Two concurrent posts read the same count and
            // produced the SAME invoice number — duplicate numbers on a legal
            // document. It also reused numbers if a posted move was ever
            // deleted or reset to draft.
            //
            // ir.sequence takes a row lock (SELECT ... FOR UPDATE) inside this
            // transaction, so concurrent posts serialise, and the allocation
            // rolls back with the post if it fails — gapless, which is what
            // tax-invoice numbering requires.
            //
            // Customer invoices and refunds use ONE dedicated sequence,
            // `account.move.INV` — prefix "INV", padding 6, no reset — so
            // they read INV000001, INV000002, … as a single continuous
            // series regardless of which sale journal posted them (seeded
            // by migration 1020). Everything else (vendor bills, journal
            // entries, payments) keeps the per-journal sequence, created on
            // first post.
            // out_refund (customer credit note) is a "reverse invoice" — its own
            // RINV series (docs/082); out_invoice keeps the INV series; everything
            // else uses a per-journal sequence created on first post.
            const bool isCustomerInvoice = (moveType == "out_invoice");
            const bool isReverseInvoice  = (moveType == "out_refund");
            const std::string seqCode =
                isReverseInvoice  ? "account.move.RINV"
              : isCustomerInvoice ? "account.move.INV"
              :                     "account.move." + jcode;

            if (!isCustomerInvoice && !isReverseInvoice) {
                // Journals are user-defined, so a per-journal sequence is
                // created on first post. ON CONFLICT DO NOTHING makes
                // concurrent first-posts safe. The INV sequence is seeded
                // in migration 1020, so it is never created here.
                txn.exec(
                    "INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy) "
                    "VALUES ($1, $2, $3, 4, 'yearly') "
                    "ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING",
                    pqxx::params{seqCode,
                                 "Journal — " + jcode,
                                 jcode + "/%(year)s/"});
            }

            std::ostringstream ss;
            ss << core::IrSequence::instance().nextByCode(txn, seqCode);

            txn.exec(
                "UPDATE account_move "
                "SET state = 'posted', name = $1, write_date = now() "
                "WHERE id = $2",
                pqxx::params{ss.str(), id});

            // Analytic accounting: for every posted journal item tagged to an
            // analytic account, generate an analytic line. Amount uses the
            // margin sign (credit − debit): revenue positive, cost negative.
            // Idempotent — a re-post never duplicates (NOT EXISTS on move_line).
            txn.exec(
                "INSERT INTO account_analytic_line "
                "(name, date, amount, account_id, general_account_id, move_line_id, company_id) "
                "SELECT COALESCE(NULLIF(aml.name,''),'/'), $2::date, (aml.credit - aml.debit), "
                "       aml.analytic_account_id, aml.account_id, aml.id, aml.company_id "
                "FROM account_move_line aml "
                "WHERE aml.move_id=$1 AND aml.analytic_account_id IS NOT NULL "
                "AND NOT EXISTS (SELECT 1 FROM account_analytic_line al WHERE al.move_line_id=aml.id)",
                pqxx::params{id, date});

            odoo::modules::mail::postLog(txn, "account.move", id, 0,
                "Invoice posted.", "log_note");
        }

        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log(
                "account.move", "action_post", ids, extractContext_(call).uid);
        return true;
    }

    nlohmann::json handleButtonCancel(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "UPDATE account_move SET state = 'cancel', write_date = now() "
            "WHERE id = ANY($1::int[]) AND state = 'posted'",
            pqxx::params{idsArray_(ids)});
        for (int id : ids)
            odoo::modules::mail::postLog(txn, "account.move", id, 0,
                "Invoice cancelled.", "log_note");
        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log(
                "account.move", "action_cancel", ids, extractContext_(call).uid);
        return true;
    }

    /**
     * @brief Create a credit note / vendor refund by REVERSING an invoice/bill.
     *
     * A reversal copies every ledger line of the posted source with debit and
     * credit swapped into a new draft move whose move_type is flipped
     * (out_invoice → out_refund, in_invoice → in_refund). Swapping the legs of a
     * balanced entry yields a balanced entry with the opposite effect — so the
     * credit note is correctly signed by construction (income debited, receivable
     * credited) without re-deriving anything. Returns the new move id.
     */
    nlohmann::json handleActionReverse(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return nlohmann::json(false);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json created = nlohmann::json::array();

        for (int id : ids) {
            auto src = txn.exec(
                "SELECT move_type, state, name FROM account_move WHERE id=$1",
                pqxx::params{id});
            if (src.empty())
                throw std::runtime_error("Move not found: " + std::to_string(id));
            const std::string mt      = src[0]["move_type"].c_str();
            const std::string state   = src[0]["state"].c_str();
            const std::string srcName = src[0]["name"].c_str();

            const std::string revType = (mt == "out_invoice") ? "out_refund"
                                      : (mt == "in_invoice")  ? "in_refund"  : "";
            if (revType.empty())
                throw infrastructure::ValidationError(
                    "Only a customer invoice or a vendor bill can be credited / refunded.");
            if (state != "posted")
                throw infrastructure::ValidationError(
                    "Post the invoice before creating a credit note.");

            // The reversing move: same partner/journal/amounts, type flipped,
            // linked back to the source, left in draft for the user to review.
            auto nm = txn.exec(
                "INSERT INTO account_move "
                "(move_type, state, date, invoice_date, journal_id, partner_id, currency_id, "
                " company_id, payment_term_id, amount_untaxed, amount_tax, amount_total, "
                " amount_residual, payment_state, line_precision, reversed_entry_id, ref, invoice_origin) "
                // ref = "Reversal of <invoice>"; the reference document (Source /
                // invoice_origin) stays the originating sales order — so the credit
                // note points at the SO, and the invoice number lives on the lines.
                "SELECT $2, 'draft', CURRENT_DATE, CURRENT_DATE, journal_id, partner_id, currency_id, "
                " company_id, payment_term_id, amount_untaxed, amount_tax, amount_total, "
                " amount_total, 'not_paid', line_precision, id, "
                " 'Reversal of ' || name, COALESCE(invoice_origin, name) "
                "FROM account_move WHERE id=$1 RETURNING id",
                pqxx::params{id, revType});
            const int newId = nm[0][0].as<int>();

            // Copy every ledger line with debit<->credit (and amount_currency)
            // swapped. Each real line is tagged with the invoice number it
            // reverses (name + ref) so the credit note is self-documenting.
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, partner_id, "
                " debit, credit, amount_currency, quantity, price_unit, tax_line_id, "
                " name, ref, display_type, tax_ids_json, analytic_account_id) "
                "SELECT $2, account_id, journal_id, company_id, CURRENT_DATE, partner_id, "
                " credit, debit, -COALESCE(amount_currency,0), quantity, price_unit, tax_line_id, "
                " CASE WHEN COALESCE(display_type,'')='' "
                "      THEN TRIM(BOTH ' ' FROM COALESCE(name,'') || ' (reverses ' || $3 || ')') "
                "      ELSE name END, "
                " $3, display_type, tax_ids_json, analytic_account_id "
                "FROM account_move_line WHERE move_id=$1",
                pqxx::params{id, newId, srcName});

            odoo::modules::mail::postLog(txn, "account.move", newId, 0,
                "Credit note / refund created as a reversal of " + srcName + ".", "log_note");
            created.push_back(newId);
        }

        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log(
                "account.move", "action_reverse", ids, extractContext_(call).uid);
        return created.empty() ? nlohmann::json(false) : created[0];
    }

    /**
     * @brief Recompute an invoice's tax lines from its product lines. (P3)
     *
     * Deletes the generated tax lines and rebuilds them, so the result is the
     * same however many times it runs — the function is called on every line
     * edit, and appending instead of replacing would multiply the tax on each
     * save.
     *
     * A "generated tax line" is exactly `tax_line_id IS NOT NULL`. Lines a
     * user entered by hand have no tax_line_id and are never touched.
     *
     * @returns total tax in micro-units.
     */
    // Which side a document's PRODUCT lines sit on.
    //
    //   out_invoice  Dr receivable / Cr revenue + tax     → product = credit
    //   out_refund   Cr receivable / Dr revenue + tax     → product = debit
    //   in_invoice   Cr payable    / Dr expense + tax     → product = debit
    //   in_refund    Dr payable    / Cr expense + tax     → product = credit
    //
    // Everything below used to assume the out_invoice shape unconditionally,
    // which is why a hand-entered credit note came out with a zero total and
    // its counterparty line untouched: its revenue lines are debits, so the
    // "sum the credits" pass found nothing (docs/082 follow-up, docs/092).
    static bool productLinesAreCredit_(const std::string& moveType) {
        return moveType == "out_invoice" || moveType == "in_refund";
    }

    long long recomputeTaxLines_(pqxx::work& txn, int moveId) {
        // Rebuild, do not append.
        txn.exec("DELETE FROM account_move_line "
                 " WHERE move_id = $1 AND tax_line_id IS NOT NULL",
                 pqxx::params{moveId});

        auto hdr = txn.exec(
            "SELECT journal_id, company_id, date, COALESCE(line_precision, 0) AS lp, "
            "       COALESCE(move_type,'entry') AS move_type "
            "  FROM account_move WHERE id = $1", pqxx::params{moveId});
        if (hdr.empty()) return 0;
        // A tax line follows its product lines onto the same side, or the
        // entry cannot balance.
        const bool taxOnCredit = productLinesAreCredit_(hdr[0]["move_type"].c_str());
        const int journalId = hdr[0]["journal_id"].is_null() ? 0 : hdr[0]["journal_id"].as<int>();
        const int companyId = hdr[0]["company_id"].is_null() ? 1 : hdr[0]["company_id"].as<int>();
        const std::string date = hdr[0]["date"].is_null() ? "" : hdr[0]["date"].c_str();
        const int linePrec = hdr[0]["lp"].as<int>(0);

        auto lines = txn.exec(
            "SELECT id, quantity, price_unit, tax_ids_json, partner_id "
            "  FROM account_move_line "
            " WHERE move_id = $1 AND display_type = '' AND tax_line_id IS NULL",
            pqxx::params{moveId});

        // Accumulate per tax so one tax used on several lines produces ONE tax
        // line, which is what an invoice is expected to show.
        std::map<int, core::Money> byTax;
        std::map<int, std::string> taxNames;
        core::Money grandTotal = core::Money::zero();

        for (const auto& l : lines) {
            nlohmann::json vals;
            vals["quantity"]     = core::Money::fromMicros(l["quantity"].as<long long>(0)).toJson();
            vals["price_unit"]   = core::Money::fromMicros(l["price_unit"].as<long long>(0)).toJson();
            vals["discount"]     = 0.0;
            vals["tax_ids_json"] = l["tax_ids_json"].is_null() ? "[]" : l["tax_ids_json"].c_str();

            const auto taxes = core::loadTaxes(txn, vals["tax_ids_json"].get<std::string>());
            if (taxes.empty()) continue;

            const int dp = linePrec > 0 ? linePrec
                         : (core::DecimalPrecision::ready()
                                ? core::DecimalPrecision::instance()
                                      .digits(core::DecimalPrecision::kAccount, 2)
                                : 2);
            const auto res = core::TaxEngine::compute(
                core::Money::fromJson(vals["price_unit"].get<double>()),
                core::Money::fromJson(vals["quantity"].get<double>()),
                core::Money::zero(), taxes, dp);

            for (const auto& c : res.components) {
                byTax[c.taxId]    += c.amount;
                taxNames[c.taxId]  = c.name;
                grandTotal        += c.amount;
            }
        }

        // One line per tax, posted to that tax's account, on the same side as
        // the product lines it was computed from.
        for (const auto& [taxId, amount] : byTax) {
            if (amount.isZero()) continue;
            auto ta = txn.exec("SELECT account_id FROM account_tax WHERE id = $1",
                               pqxx::params{taxId});
            int acctId = (ta.empty() || ta[0][0].is_null()) ? 0 : ta[0][0].as<int>();
            if (acctId == 0) {
                // No tax account configured: fall back to Tax Payable so the
                // entry still balances rather than silently dropping the tax.
                auto fb = txn.exec(
                    "SELECT id FROM account_account "
                    " WHERE code = '2200' AND company_id = $1 LIMIT 1",
                    pqxx::params{companyId});
                if (fb.empty()) continue;   // nothing sane to post to
                acctId = fb[0][0].as<int>();
            }
            pqxx::params p;
            p.append(moveId); p.append(acctId);
            if (journalId > 0) p.append(journalId); else p.append(nullptr);
            p.append(companyId); p.append(date);
            p.append(taxNames[taxId]); p.append(taxId);
            p.append(amount.toDb());
            txn.exec(
                taxOnCredit
                    ? "INSERT INTO account_move_line "
                      "(move_id, account_id, journal_id, company_id, date, name, "
                      " tax_line_id, credit, debit, display_type) "
                      "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,0,'')"
                    : "INSERT INTO account_move_line "
                      "(move_id, account_id, journal_id, company_id, date, name, "
                      " tax_line_id, debit, credit, display_type) "
                      "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,0,'')", p);
        }
        return grandTotal.toDb();
    }

    nlohmann::json handleRecomputeTotals(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            auto typeRow = txn.exec(
                "SELECT COALESCE(move_type,'entry') FROM account_move WHERE id=$1",
                pqxx::params{id});
            if (typeRow.empty()) continue;
            // A credit note's product lines are DEBITS. Summing credits on one
            // gave zero, so a hand-entered credit note showed a zero total and
            // its counterparty line was never updated (docs/092).
            const bool prodCredit = productLinesAreCredit_(typeRow[0][0].c_str());

            // tax_line_id IS NULL excludes the generated tax lines. They sit on
            // the same side as the product lines, so without this filter the tax
            // would be counted as revenue and then added again as tax —
            // inflating the document by the tax amount on every recompute.
            auto incRow = txn.exec(
                prodCredit
                    ? "SELECT COALESCE(SUM(credit),0) FROM account_move_line "
                      "WHERE move_id=$1 AND credit>0 AND display_type='' "
                      "  AND tax_line_id IS NULL"
                    : "SELECT COALESCE(SUM(debit),0) FROM account_move_line "
                      "WHERE move_id=$1 AND debit>0 AND display_type='' "
                      "  AND tax_line_id IS NULL",
                pqxx::params{id});
            // P2: every value in this block is micro-units — SUM() over
            // micro-unit columns, added to a micro-unit column, written back to
            // micro-unit columns. Self-consistent, so no scaling is applied
            // here. Held as long long rather than double so the units are
            // explicit and the arithmetic is exact by construction.
            long long untaxed = incRow[0][0].as<long long>(0);

            auto mvRow = txn.exec(
                "SELECT amount_tax, payment_state FROM account_move WHERE id=$1",
                pqxx::params{id});
            if (mvRow.empty()) continue;

            std::string ps = mvRow[0][1].c_str();

            // P3: compute tax from the product lines and regenerate the tax
            // lines, instead of trusting whatever amount_tax already held.
            // Before this, amount_tax could only be copied from a sale order
            // or typed by hand — an invoice created directly always showed
            // zero tax, and no tax line was ever posted to the ledger.
            const long long tax = recomputeTaxLines_(txn, id);

            long long total    = untaxed + tax;
            long long residual = (ps == "not_paid" || ps == "partial") ? total : 0;

            // amount_tax is written too. The original only ever READ it —
            // so a computed tax reached the ledger as a tax line and the
            // total, but the header still displayed zero tax.
            txn.exec(
                "UPDATE account_move "
                "SET amount_untaxed=$1, amount_tax=$2, amount_total=$3, "
                "    amount_residual=$4, write_date=now() "
                "WHERE id=$5",
                pqxx::params{untaxed, tax, total, residual, id});

            // Update the counterparty (AR/AP) line to match the new total. It
            // is always on the side OPPOSITE the product lines, which is what
            // makes it identifiable without knowing the account.
            txn.exec(
                prodCredit
                    ? "UPDATE account_move_line SET debit=$1, write_date=now() "
                      "WHERE move_id=$2 AND debit>0"
                    : "UPDATE account_move_line SET credit=$1, write_date=now() "
                      "WHERE move_id=$2 AND credit>0",
                pqxx::params{total, id});
        }

        txn.commit();
        return true;
    }

    nlohmann::json handleActionRegisterPayment(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        // --- Extract kwargs ---
        std::string payDate;
        if (call.kwargs.contains("payment_date") && call.kwargs["payment_date"].is_string())
            payDate = call.kwargs["payment_date"].get<std::string>();
        if (payDate.empty()) {
            auto t = std::time(nullptr); auto tm = *std::localtime(&t);
            char buf[11]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            payDate = buf;
        }

        int    kwJournalId = 0;
        double kwAmount    = -1.0;   // -1 = use full residual
        std::string kwMemo;
        if (call.kwargs.contains("journal_id") && call.kwargs["journal_id"].is_number_integer())
            kwJournalId = call.kwargs["journal_id"].get<int>();
        if (call.kwargs.contains("amount") && call.kwargs["amount"].is_number())
            kwAmount = call.kwargs["amount"].get<double>();
        if (call.kwargs.contains("memo") && call.kwargs["memo"].is_string())
            kwMemo = call.kwargs["memo"].get<std::string>();

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json result = nlohmann::json::array();

        for (int id : ids) {
            // --- Read invoice ---
            auto invRow = txn.exec(
                "SELECT amount_total, amount_residual, partner_id, company_id, currency_id, "
                "       move_type, name, state, payment_state "
                "FROM account_move WHERE id=$1",
                pqxx::params{id});
            if (invRow.empty())
                throw std::runtime_error("Invoice not found: " + std::to_string(id));

            std::string invState   = invRow[0]["state"].c_str();
            std::string payState   = invRow[0]["payment_state"].c_str();
            if (invState != "posted")
                throw odoo::infrastructure::AccessDeniedError(
                    "Payment can only be registered on posted invoices");
            if (payState == "paid")
                throw odoo::infrastructure::AccessDeniedError(
                    "Invoice is already fully paid");

            // P2: amount_residual is BIGINT micro-units (migration 911).
            // Reading it straight as a double would yield 250000000 where
            // 250.00 is meant, and every payment comparison below would be
            // wrong by a factor of a million.
            const double amountResidual =
                core::Money::fromMicros(invRow[0]["amount_residual"].as<long long>(0))
                    .toJson();
            int    partnerId      = invRow[0]["partner_id"].is_null()  ? 0 : invRow[0]["partner_id"].as<int>();
            int    companyId      = invRow[0]["company_id"].is_null()  ? 1 : invRow[0]["company_id"].as<int>();
            int    currencyId     = invRow[0]["currency_id"].is_null() ? 0 : invRow[0]["currency_id"].as<int>();
            std::string moveType  = invRow[0]["move_type"].c_str();
            std::string invName   = invRow[0]["name"].c_str();

            double payAmount = (kwAmount > 0)
                ? std::min(kwAmount, amountResidual)
                : amountResidual;
            if (payAmount <= 0) payAmount = amountResidual;

            std::string memo = kwMemo.empty() ? invName : kwMemo;

            bool isOutInvoice   = (moveType == "out_invoice" || moveType == "out_refund");
            std::string payType = isOutInvoice ? "inbound"  : "outbound";
            std::string partType= isOutInvoice ? "customer" : "supplier";

            // --- Resolve journal ---
            int journalId = kwJournalId;
            if (journalId <= 0) {
                auto jdefRow = txn.exec(
                    "SELECT id FROM account_journal "
                    "WHERE type IN ('bank','cash') AND company_id=$1 "
                    "ORDER BY (type='bank') DESC, id LIMIT 1",
                    pqxx::params{companyId});
                if (jdefRow.empty())
                    throw std::runtime_error("No bank or cash journal found");
                journalId = jdefRow[0][0].as<int>();
            }

            // --- Cash/bank account for the journal ---
            auto jrow = txn.exec(
                "SELECT code, default_account_id FROM account_journal WHERE id=$1",
                pqxx::params{journalId});
            if (jrow.empty()) throw std::runtime_error("Journal not found");
            std::string jcode         = jrow[0][0].c_str();
            int         cashAccountId = jrow[0][1].is_null() ? 0 : jrow[0][1].as<int>();
            if (cashAccountId == 0) {
                auto arow = txn.exec(
                    "SELECT id FROM account_account "
                    "WHERE account_type='asset_cash' AND company_id=$1 AND active=TRUE LIMIT 1",
                    pqxx::params{companyId});
                if (!arow.empty()) cashAccountId = arow[0][0].as<int>();
            }

            // --- Partner (receivable/payable) account ---
            std::string accType = isOutInvoice ? "asset_receivable" : "liability_payable";
            auto arow = txn.exec(
                "SELECT id FROM account_account "
                "WHERE account_type=$1 AND company_id=$2 AND active=TRUE LIMIT 1",
                pqxx::params{accType, companyId});
            int partnerAccId = arow.empty() ? cashAccountId : arow[0][0].as<int>();

            // --- Create account_payment ---
            pqxx::params pp;
            pp.append(payDate); pp.append(journalId);
            if (partnerId > 0) pp.append(partnerId); else pp.append(nullptr);
            pp.append(companyId);
            if (currencyId > 0) pp.append(currencyId); else pp.append(nullptr);
            // P2: account_payment.amount is micro-units (migration 912)
            pp.append(core::Money::fromJson(payAmount).toDb());
            pp.append(payType); pp.append(partType); pp.append(memo);

            auto pmtRow = txn.exec(
                "INSERT INTO account_payment "
                "(date, journal_id, partner_id, company_id, currency_id, "
                " amount, payment_type, partner_type, memo, state) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,'draft') RETURNING id",
                pp);
            int pmtId = pmtRow[0][0].as<int>();

            // --- Generate journal entry name ---
            std::string year = payDate.size() >= 4 ? payDate.substr(0, 4) : "2026";
            // P4: same COUNT(*)+1 race as invoice numbering had — two
            // concurrent payments produced the same journal entry name.
            const std::string pSeqCode = "account.move." + jcode;
            txn.exec(
                "INSERT INTO ir_sequence (code, name, prefix, padding, reset_policy) "
                "VALUES ($1, $2, $3, 4, 'yearly') "
                "ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING",
                pqxx::params{pSeqCode, "Journal — " + jcode, jcode + "/%(year)s/"});
            std::ostringstream ss;
            ss << core::IrSequence::instance().nextByCode(txn, pSeqCode);

            // --- Create journal entry ---
            pqxx::params mp;
            mp.append(ss.str()); mp.append(payDate); mp.append(journalId);
            if (partnerId > 0) mp.append(partnerId); else mp.append(nullptr);
            mp.append(companyId);
            if (currencyId > 0) mp.append(currencyId); else mp.append(nullptr);
            mp.append(memo);

            auto moveRow = txn.exec(
                "INSERT INTO account_move "
                "(name, move_type, state, date, journal_id, partner_id, "
                " company_id, currency_id, narration) "
                "VALUES ($1,'entry','posted',$2,$3,$4,$5,$6,$7) RETURNING id",
                mp);
            int moveId = moveRow[0][0].as<int>();

            // DR: cash account (inbound) or payable (outbound)
            // CR: receivable (inbound) or cash account (outbound)
            int drAccId = isOutInvoice ? cashAccountId : partnerAccId;
            int crAccId = isOutInvoice ? partnerAccId  : cashAccountId;

            // P2: debit/credit are BIGINT micro-units (migration 910). This is
            // raw SQL, so it bypasses BaseModel::normalizeForDb_ and must do
            // the major->micro conversion itself. The lambda takes micro-units
            // directly so the FX arithmetic below never round-trips through
            // double — the whole point of P2.
            auto insertLine = [&](int acctId, long long debit, long long credit) {
                pqxx::params lp;
                lp.append(moveId); lp.append(acctId); lp.append(journalId);
                lp.append(companyId); lp.append(payDate); lp.append(memo);
                if (partnerId > 0) lp.append(partnerId); else lp.append(nullptr);
                lp.append(debit);
                lp.append(credit);
                txn.exec(
                    "INSERT INTO account_move_line "
                    "(move_id, account_id, journal_id, company_id, date, name, "
                    " partner_id, debit, credit) "
                    "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)", lp);
            };
            // The two legs are NOT written yet. Under a foreign settlement the
            // cash leg and the receivable leg are DIFFERENT amounts, and the
            // gap between them is exactly the realised FX — which only the
            // allocator below can compute. Writing them here (both at
            // payAmount) left the entry out of balance by the FX difference
            // the moment a 7900 line was added.

            // Link payment → move
            txn.exec(
                "UPDATE account_payment SET state='posted', move_id=$1, name=$2, write_date=now() "
                "WHERE id=$3",
                pqxx::params{moveId, ss.str(), pmtId});

            // --- Allocate (P1) ---
            //
            // Replaces `residual = residual - paid` with a reconcile row.
            // The scalar could not represent one payment across several
            // invoices, an unallocated advance, or a reversal; residual is
            // now DERIVED from the allocation rows so the two cannot
            // disagree, and "fully paid" is an exact isZero() rather than a
            // `< 0.001` epsilon.
            //
            // receivedBase: what actually landed, in base currency. The
            // dialog collects it for a foreign-currency invoice (the bank
            // converts on receipt, so the rate is derived from the amount
            // rather than typed — docs/048 §4.6). Zero means same-currency.
            const core::Money receivedBase =
                call.kwargs.contains("amount_received_base") &&
                call.kwargs["amount_received_base"].is_number()
                    ? core::Money::fromJson(call.kwargs["amount_received_base"].get<double>())
                    : core::Money::zero();

            const auto alloc = core::PaymentAllocation::allocate(
                txn, pmtId, receivedBase, {id});

            // --- The journal entry, now that the FX is known ---
            //
            // Same-currency settlement: both legs are payAmount, as before.
            //
            // Foreign settlement (docs/048 §4.6): the bank converted on
            // receipt, so the two legs differ.
            //
            //     100 USD invoice booked at 4.70   AR carries 470.00 MYR
            //     bank credits                                 448.50 MYR
            //
            //     DR Bank            448.50   <- what actually moved
            //     DR FX loss          21.50   <- the gap
            //     CR Receivable      470.00   <- what the AR was carrying
            //
            // The receivable must be relieved at its BOOKED base value or the
            // customer's ledger never clears; the bank must be debited with
            // what actually landed or the cash book is wrong. Both cannot be
            // the same number, and the difference is the realised FX.
            const long long fxMicros     = alloc.totalFxDiff.toDb();
            const long long payMicros    = core::Money::fromJson(payAmount).toDb();
            const bool      hasFx        = fxMicros != 0;
            const long long cashMicros   = hasFx ? receivedBase.toDb() : payMicros;
            // booked = received - fxDiff, since fxDiff = settlement - booked.
            const long long bookedMicros = hasFx ? cashMicros - fxMicros : payMicros;

            insertLine(drAccId, isOutInvoice ? cashMicros : bookedMicros, 0);
            insertLine(crAccId, 0, isOutInvoice ? bookedMicros : cashMicros);

            // Realised FX goes to 7900 as a journal line on the PAYMENT
            // entry, never as a line on the customer invoice — the customer
            // owes the invoice amount regardless of what the ringgit did.
            if (hasFx) {
                auto fxAcc = txn.exec(
                    "SELECT id FROM account_account "
                    " WHERE code = '7900' AND company_id = $1 LIMIT 1",
                    pqxx::params{companyId});
                if (!fxAcc.empty()) {
                    const int fxId = fxAcc[0][0].as<int>();
                    // gap = booked - cash. On a RECEIPT it balances on the
                    // debit side (a positive gap is a loss); on a PAYMENT the
                    // legs are reversed, so the same gap balances on the
                    // credit side and a positive gap is a gain — we settled a
                    // liability for less base currency than it was booked at.
                    const long long gap = bookedMicros - cashMicros;   // = -fxDiff
                    const bool debitSide = isOutInvoice ? (gap >= 0) : (gap < 0);
                    const long long mag  = gap >= 0 ? gap : -gap;
                    insertLine(fxId, debitSide ? mag : 0, debitSide ? 0 : mag);

                    // "Loss" is from the company's point of view: on a receipt
                    // a negative fxDiff means less base landed than was booked;
                    // on a payment the sign flips.
                    const bool isLoss = isOutInvoice ? (fxMicros < 0) : (fxMicros > 0);
                    odoo::modules::mail::postLog(
                        txn, "account.move", id, 0,
                        std::string("Realised FX ") + (isLoss ? "loss " : "gain ") +
                            core::Money::fromMicros(mag).toString(2) + " posted to 7900.",
                        "log_note");
                }
            }

            // Chatter
            std::ostringstream logMsg;
            logMsg << std::fixed << std::setprecision(2)
                   << "Payment of " << payAmount << " registered on " << payDate << ".";
            if (!memo.empty() && memo != invName)
                logMsg << " Ref: " << memo;
            odoo::modules::mail::postLog(txn, "account.move", id, 0, logMsg.str(), "log_note");

            // Residual and payment_state are DERIVED from the allocation rows
            // by PaymentAllocation::refreshResidual, so they are read back
            // rather than recomputed here — one source of truth.
            auto st = txn.exec(
                "SELECT payment_state, amount_residual FROM account_move WHERE id=$1",
                pqxx::params{id});
            result.push_back(nlohmann::json{
                {"payment_id",      pmtId},
                {"payment_state",   st.empty() ? std::string("partial")
                                               : std::string(st[0][0].c_str())},
                {"amount_residual", st.empty() ? 0.0
                    : core::Money::fromMicros(st[0][1].as<long long>(0)).toJson()},
                {"unallocated",     alloc.unallocated.toJson()},
                {"fx_difference",   alloc.totalFxDiff.toJson()}
            });
        }

        txn.commit();
        return (result.size() == 1) ? result[0] : result;
    }

    nlohmann::json handleButtonDraft(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "UPDATE account_move SET state = 'draft', name = '/', write_date = now() "
            "WHERE id = ANY($1::int[]) AND state = 'cancel'",
            pqxx::params{idsArray_(ids)});
        for (int id : ids)
            odoo::modules::mail::postLog(txn, "account.move", id, 0,
                "Reset to draft.", "log_note");
        txn.commit();
        return true;
    }
};

// ----------------------------------------------------------------
// AccountPaymentViewModel — adds action_post / action_cancel
// ----------------------------------------------------------------
class AccountPaymentViewModel : public AccountViewModel<AccountPayment> {
public:
    explicit AccountPaymentViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : AccountViewModel<AccountPayment>(std::move(db))
    {
        REGISTER_METHOD("action_post",   handleActionPost)
        REGISTER_METHOD("action_cancel", handleActionCancel)
    }

    std::string modelName() const override { return "account.payment"; }

private:
    nlohmann::json handleActionPost(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int pid : ids) {
            auto r = txn.exec(
                "SELECT state, date, journal_id, partner_id, company_id, "
                "       currency_id, amount, payment_type, partner_type, memo "
                "FROM account_payment WHERE id = $1",
                pqxx::params{pid});
            if (r.empty()) throw std::runtime_error("Payment not found");

            std::string state      = r[0][0].c_str();
            std::string date       = r[0][1].c_str();
            int         journalId  = r[0][2].as<int>();
            int         partnerId  = r[0][3].is_null() ? 0 : r[0][3].as<int>();
            int         companyId  = r[0][4].as<int>();
            int         currencyId = r[0][5].is_null() ? 0 : r[0][5].as<int>();
            double      amount     = r[0][6].as<double>();
            std::string payType    = r[0][7].c_str();
            std::string partType   = r[0][8].c_str();
            std::string memo       = r[0][9].is_null() ? "" : std::string(r[0][9].c_str());

            if (state != "draft")
                throw std::runtime_error("Only draft payments can be posted");

            // Journal info
            auto jrow = txn.exec(
                "SELECT code, default_account_id FROM account_journal WHERE id = $1",
                pqxx::params{journalId});
            if (jrow.empty()) throw std::runtime_error("Journal not found");
            std::string jcode         = jrow[0][0].c_str();
            int         cashAccountId = jrow[0][1].is_null() ? 0 : jrow[0][1].as<int>();

            // Fallback for cash/bank account
            if (cashAccountId == 0) {
                auto arow = txn.exec(
                    "SELECT id FROM account_account "
                    "WHERE account_type = 'asset_cash' AND company_id = $1 AND active = TRUE "
                    "LIMIT 1",
                    pqxx::params{companyId});
                if (!arow.empty()) cashAccountId = arow[0][0].as<int>();
            }

            // Partner account (receivable or payable)
            std::string accType = (partType == "customer") ? "asset_receivable" : "liability_payable";
            auto arow = txn.exec(
                "SELECT id FROM account_account "
                "WHERE account_type = $1 AND company_id = $2 AND active = TRUE "
                "LIMIT 1",
                pqxx::params{accType, companyId});
            int partnerAccountId = arow.empty() ? cashAccountId : arow[0][0].as<int>();

            // Generate move name
            std::string year = date.size() >= 4 ? date.substr(0, 4) : "2026";
            auto cnt = txn.exec(
                "SELECT COUNT(*) FROM account_move "
                "WHERE journal_id = $1 AND state = 'posted' "
                "AND EXTRACT(YEAR FROM date::date) = $2::int",
                pqxx::params{journalId, std::stoi(year)});
            int seq = cnt[0][0].as<int>() + 1;

            std::ostringstream ss;
            ss << jcode << "/" << year << "/"
               << std::setfill('0') << std::setw(4) << seq;

            // Create the journal entry
            pqxx::params moveParams;
            moveParams.append(ss.str());
            moveParams.append(date);
            moveParams.append(journalId);
            if (partnerId > 0) moveParams.append(partnerId); else moveParams.append(nullptr);
            moveParams.append(companyId);
            if (currencyId > 0) moveParams.append(currencyId); else moveParams.append(nullptr);
            if (!memo.empty()) moveParams.append(memo); else moveParams.append(nullptr);

            auto moveRow = txn.exec(
                "INSERT INTO account_move "
                "(name, move_type, state, date, journal_id, partner_id, "
                " company_id, currency_id, narration) "
                "VALUES ($1, 'entry', 'posted', $2, $3, $4, $5, $6, $7) "
                "RETURNING id",
                moveParams);
            int moveId = moveRow[0][0].as<int>();

            // Determine debit/credit sides
            int    drAccountId, crAccountId;
            double drAmount, crAmount;
            if (payType == "inbound") {
                // Customer pays us: DR Cash/Bank, CR Receivable
                drAccountId = cashAccountId;
                crAccountId = partnerAccountId;
            } else {
                // We pay supplier: DR Payable, CR Cash/Bank
                drAccountId = partnerAccountId;
                crAccountId = cashAccountId;
            }
            drAmount = amount;
            crAmount = amount;

            // Insert debit line
            pqxx::params l1;
            l1.append(moveId);
            l1.append(drAccountId);
            l1.append(journalId);
            l1.append(companyId);
            l1.append(date);
            l1.append(memo.empty() ? "Payment" : memo);
            if (partnerId > 0) l1.append(partnerId); else l1.append(nullptr);
            l1.append(drAmount);
            l1.append(0.0);
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, name, partner_id, debit, credit) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                l1);

            // Insert credit line
            pqxx::params l2;
            l2.append(moveId);
            l2.append(crAccountId);
            l2.append(journalId);
            l2.append(companyId);
            l2.append(date);
            l2.append(memo.empty() ? "Payment" : memo);
            if (partnerId > 0) l2.append(partnerId); else l2.append(nullptr);
            l2.append(0.0);
            l2.append(crAmount);
            txn.exec(
                "INSERT INTO account_move_line "
                "(move_id, account_id, journal_id, company_id, date, name, partner_id, debit, credit) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                l2);

            // Update payment
            txn.exec(
                "UPDATE account_payment "
                "SET state = 'posted', move_id = $1, name = $2, write_date = now() "
                "WHERE id = $3",
                pqxx::params{moveId, ss.str(), pid});
        }

        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log(
                "account.payment", "action_post", ids, extractContext_(call).uid);
        return true;
    }

    nlohmann::json handleActionCancel(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "UPDATE account_payment SET state = 'cancelled', write_date = now() "
            "WHERE id = ANY($1::int[]) AND state IN ('draft','posted')",
            pqxx::params{idsArray_(ids)});
        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log(
                "account.payment", "action_cancel", ids, extractContext_(call).uid);
        return true;
    }
};


// ================================================================
// Analytic accounting — cost centres
// ================================================================
class AccountAnalyticAccount : public core::BaseModel<AccountAnalyticAccount> {
public:
    ODOO_MODEL("account.analytic.account", "account_analytic_account")
    std::string name, code;
    int         partnerId = 0, companyId = 0;
    bool        active = true;
    explicit AccountAnalyticAccount(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAnalyticAccount>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char,    "Analytic Account", true});
        fieldRegistry_.add({"code",       core::FieldType::Char,    "Reference"});
        fieldRegistry_.add({"partner_id", core::FieldType::Many2one,"Customer", false, false, true, true, "res.partner"});
        fieldRegistry_.add({"company_id", core::FieldType::Many2one,"Company",  false, false, true, true, "res.company"});
        fieldRegistry_.add({"active",     core::FieldType::Boolean, "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["code"]       = code.empty() ? nlohmann::json(false) : nlohmann::json(code);
        j["partner_id"] = partnerId > 0 ? nlohmann::json(partnerId) : nlohmann::json(false);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]     = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")   && j["name"].is_string())   name   = j["name"].get<std::string>();
        if (j.contains("code")   && j["code"].is_string())   code   = j["code"].get<std::string>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
        if (j.contains("partner_id")) partnerId = m2oToId_(j["partner_id"]);
        if (j.contains("company_id")) companyId = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override { std::vector<std::string> e; if (name.empty()) e.push_back("Name is required"); return e; }
};

class AccountAnalyticLine : public core::BaseModel<AccountAnalyticLine> {
public:
    ODOO_MODEL("account.analytic.line", "account_analytic_line")
    std::string name, date;
    double      amount = 0.0;
    int         accountId = 0;          // the analytic account
    int         generalAccountId = 0;   // the GL account
    int         moveLineId = 0;
    int         companyId = 0;
    explicit AccountAnalyticLine(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAnalyticLine>(std::move(db)) { date = currentDate_(); }
    void registerFields() override {
        fieldRegistry_.add({"name",               core::FieldType::Char,    "Description"});
        fieldRegistry_.add({"date",               core::FieldType::Date,    "Date"});
        fieldRegistry_.add({"amount",             core::FieldType::Monetary,"Amount"});
        fieldRegistry_.add({"account_id",         core::FieldType::Many2one,"Analytic Account", true, false, true, true, "account.analytic.account"});
        fieldRegistry_.add({"general_account_id", core::FieldType::Many2one,"Financial Account", false, false, true, true, "account.account"});
        fieldRegistry_.add({"move_line_id",       core::FieldType::Many2one,"Journal Item", false, false, true, false, "account.move.line"});
        fieldRegistry_.add({"company_id",         core::FieldType::Many2one,"Company", false, false, true, true, "res.company"});
        fieldRegistry_.markScaled({"amount"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]               = name;
        j["date"]               = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["amount"]             = amount;
        j["account_id"]         = accountId > 0 ? nlohmann::json(accountId) : nlohmann::json(false);
        j["general_account_id"] = generalAccountId > 0 ? nlohmann::json(generalAccountId) : nlohmann::json(false);
        j["move_line_id"]       = moveLineId > 0 ? nlohmann::json(moveLineId) : nlohmann::json(false);
        j["company_id"]         = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")   && j["name"].is_string())   name   = j["name"].get<std::string>();
        if (j.contains("date")   && j["date"].is_string())   date   = j["date"].get<std::string>();
        if (j.contains("amount") && j["amount"].is_number()) amount = j["amount"].get<double>();
        if (j.contains("account_id"))         accountId        = m2oToId_(j["account_id"]);
        if (j.contains("general_account_id")) generalAccountId = m2oToId_(j["general_account_id"]);
        if (j.contains("move_line_id"))       moveLineId       = m2oToId_(j["move_line_id"]);
        if (j.contains("company_id"))         companyId        = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override { std::vector<std::string> e; if (accountId <= 0) e.push_back("Analytic account is required"); return e; }
};

// Analytic account list with a computed balance (Σ of its lines' amounts).
class AccountAnalyticAccountViewModel : public core::BaseViewModel {
public:
    explicit AccountAnalyticAccountViewModel(std::shared_ptr<infrastructure::DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("write",           handleWrite)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
    }
    std::string modelName() const override { return "account.analytic.account"; }
private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        const int lim = call.limit() > 0 ? call.limit() : 80;
        const int off = call.offset();
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT a.id, a.name, a.code, a.partner_id, rp.name AS partner_name,
                   COALESCE((SELECT SUM(amount) FROM account_analytic_line WHERE account_id=a.id),0) AS balance
            FROM account_analytic_account a
            LEFT JOIN res_partner rp ON rp.id = a.partner_id
            WHERE a.active = TRUE ORDER BY a.id DESC)";
        sql += " LIMIT " + std::to_string(lim);
        if (off > 0) sql += " OFFSET " + std::to_string(off);
        auto res = txn.exec(sql);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]         = row["id"].as<int>();
            j["name"]       = row["name"].is_null() ? "" : row["name"].c_str();
            j["code"]       = row["code"].is_null() ? nlohmann::json(false) : nlohmann::json(row["code"].c_str());
            j["partner_id"] = row["partner_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["partner_id"].as<int>(), row["partner_name"].is_null()?"":std::string(row["partner_name"].c_str())});
            j["balance"]    = core::Money::fromMicros(row["balance"].as<long long>(0)).toJson();
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call)        { AccountAnalyticAccount p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const core::CallKwArgs& call)      { AccountAnalyticAccount p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const core::CallKwArgs& call)       { AccountAnalyticAccount p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)      { AccountAnalyticAccount p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call)   { AccountAnalyticAccount p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) { AccountAnalyticAccount p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }
};

// Analytic lines (Items) — join the analytic account name; filter by account_id.
class AccountAnalyticLineViewModel : public core::BaseViewModel {
public:
    explicit AccountAnalyticLineViewModel(std::shared_ptr<infrastructure::DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",     handleSearchRead)
        REGISTER_METHOD("web_search_read", handleSearchRead)
        REGISTER_METHOD("read",            handleRead)
        REGISTER_MUTATOR("create",          handleCreate)
        REGISTER_MUTATOR("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
    }
    std::string modelName() const override { return "account.analytic.line"; }
private:
    std::shared_ptr<infrastructure::DbConnection> db_;
    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        int acctFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size()==3 && c[0].is_string() && c[0].get<std::string>()=="account_id" && c[2].is_number_integer())
                acctFilter = c[2].get<int>(); }
        const int lim = call.limit() > 0 ? call.limit() : 80;
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT l.id, l.name, l.date, l.amount, l.account_id, a.name AS account_name
            FROM account_analytic_line l LEFT JOIN account_analytic_account a ON a.id = l.account_id WHERE TRUE )";
        pqxx::params p; int pc = 0;
        if (acctFilter > 0) { sql += " AND l.account_id=$1"; p.append(acctFilter); pc = 1; }
        // S-30: enforce ir.rule on this custom read (record-rule bypass fix, 071 §1.2).
        core::appendRecordRuleSubquery(sql, p, "account.analytic.line", core::RuleOp::Read,
                                       extractContext_(call), "account_analytic_line", "l.id", pc);
        sql += " ORDER BY l.date DESC, l.id DESC LIMIT " + std::to_string(lim);
        auto res = txn.exec(sql, p);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]         = row["id"].as<int>();
            j["name"]       = row["name"].is_null() ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            j["date"]       = row["date"].is_null() ? nlohmann::json(false) : nlohmann::json(row["date"].c_str());
            j["amount"]     = core::Money::fromMicros(row["amount"].as<long long>(0)).toJson();
            j["account_id"] = row["account_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["account_id"].as<int>(), row["account_name"].is_null()?"":std::string(row["account_name"].c_str())});
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call)        { AccountAnalyticLine p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const core::CallKwArgs& call)      { AccountAnalyticLine p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)      { AccountAnalyticLine p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call)   { AccountAnalyticLine p(db_); return p.fieldsGet(call.fields()); }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) { AccountAnalyticLine p(db_); p.setUserContext(extractContext_(call)); return p.searchCount(call.domain()); }
};

class AnalyticListView : public core::BaseView {
    std::string model_, name_, label_; bool isLine_;
public:
    AnalyticListView(std::string model, std::string name, std::string label, bool isLine)
        : model_(std::move(model)), name_(std::move(name)), label_(std::move(label)), isLine_(isLine) {}
    std::string viewName()  const override { return name_; }
    std::string modelName() const override { return model_; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        if (isLine_)
            return "<list string=\"" + label_ + "\"><field name=\"date\"/><field name=\"name\"/>"
                   "<field name=\"account_id\"/><field name=\"amount\"/></list>";
        return "<list string=\"" + label_ + "\"><field name=\"name\"/><field name=\"code\"/>"
               "<field name=\"partner_id\"/><field name=\"balance\"/></list>";
    }
    nlohmann::json fields() const override {
        if (isLine_) return {
            {"date",       {{"type","date"},     {"string","Date"}}},
            {"name",       {{"type","char"},     {"string","Description"}}},
            {"account_id", {{"type","many2one"}, {"string","Analytic Account"}, {"relation","account.analytic.account"}}},
            {"amount",     {{"type","monetary"}, {"string","Amount"}}},
        };
        return {
            {"name",       {{"type","char"},     {"string","Analytic Account"}}},
            {"code",       {{"type","char"},     {"string","Reference"}}},
            {"partner_id", {{"type","many2one"}, {"string","Customer"}, {"relation","res.partner"}}},
            {"balance",    {{"type","monetary"}, {"string","Balance"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// Bank reconciliation
// ================================================================
class AccountBankStatement : public core::BaseModel<AccountBankStatement> {
public:
    ODOO_MODEL("account.bank.statement", "account_bank_statement")
    std::string name, date, state = "open";
    int         journalId = 0, companyId = 0;
    double      balanceStart = 0.0, balanceEnd = 0.0;
    explicit AccountBankStatement(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBankStatement>(std::move(db)) { date = currentDate_(); }
    void registerFields() override {
        fieldRegistry_.add({"name",          core::FieldType::Char,    "Reference"});
        fieldRegistry_.add({"date",          core::FieldType::Date,    "Date"});
        fieldRegistry_.add({"journal_id",    core::FieldType::Many2one,"Bank Journal", true, false, true, true, "account.journal"});
        fieldRegistry_.add({"balance_start", core::FieldType::Monetary,"Starting Balance"});
        fieldRegistry_.add({"balance_end",   core::FieldType::Monetary,"Ending Balance"});
        fieldRegistry_.add({"state",         core::FieldType::Char,    "Status"});
        fieldRegistry_.add({"company_id",    core::FieldType::Many2one,"Company", false, false, true, true, "res.company"});
        fieldRegistry_.markScaled({"balance_start", "balance_end"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]          = name.empty() ? nlohmann::json(false) : nlohmann::json(name);
        j["date"]          = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["journal_id"]    = journalId > 0 ? nlohmann::json(journalId) : nlohmann::json(false);
        j["balance_start"] = balanceStart;
        j["balance_end"]   = balanceEnd;
        j["state"]         = state;
        j["company_id"]    = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")  && j["name"].is_string())  name  = j["name"].get<std::string>();
        if (j.contains("date")  && j["date"].is_string())  date  = j["date"].get<std::string>();
        if (j.contains("state") && j["state"].is_string()) state = j["state"].get<std::string>();
        if (j.contains("balance_start") && j["balance_start"].is_number()) balanceStart = j["balance_start"].get<double>();
        if (j.contains("balance_end")   && j["balance_end"].is_number())   balanceEnd   = j["balance_end"].get<double>();
        if (j.contains("journal_id")) journalId = m2oToId_(j["journal_id"]);
        if (j.contains("company_id")) companyId = m2oToId_(j["company_id"]);
    }
};

class AccountBankStatementLine : public core::BaseModel<AccountBankStatementLine> {
public:
    ODOO_MODEL("account.bank.statement.line", "account_bank_statement_line")
    int         statementId = 0, partnerId = 0, reconciledMoveId = 0, companyId = 0;
    std::string date, name, paymentRef;
    double      amount = 0.0;
    bool        isReconciled = false;
    explicit AccountBankStatementLine(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBankStatementLine>(std::move(db)) { date = currentDate_(); }
    void registerFields() override {
        fieldRegistry_.add({"statement_id",  core::FieldType::Many2one,"Statement", true, false, true, false, "account.bank.statement"});
        fieldRegistry_.add({"date",          core::FieldType::Date,    "Date"});
        fieldRegistry_.add({"name",          core::FieldType::Char,    "Label"});
        fieldRegistry_.add({"payment_ref",   core::FieldType::Char,    "Reference"});
        fieldRegistry_.add({"partner_id",    core::FieldType::Many2one,"Partner", false, false, true, true, "res.partner"});
        fieldRegistry_.add({"amount",        core::FieldType::Monetary,"Amount"});
        fieldRegistry_.add({"is_reconciled", core::FieldType::Boolean, "Reconciled"});
        fieldRegistry_.add({"company_id",    core::FieldType::Many2one,"Company", false, false, true, true, "res.company"});
        fieldRegistry_.markScaled({"amount"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["statement_id"]  = statementId > 0 ? nlohmann::json(statementId) : nlohmann::json(false);
        j["date"]          = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["name"]          = name.empty() ? nlohmann::json(false) : nlohmann::json(name);
        j["payment_ref"]   = paymentRef.empty() ? nlohmann::json(false) : nlohmann::json(paymentRef);
        j["partner_id"]    = partnerId > 0 ? nlohmann::json(partnerId) : nlohmann::json(false);
        j["amount"]        = amount;
        j["is_reconciled"] = isReconciled;
        j["company_id"]    = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("date")        && j["date"].is_string())        date       = j["date"].get<std::string>();
        if (j.contains("name")        && j["name"].is_string())        name       = j["name"].get<std::string>();
        if (j.contains("payment_ref") && j["payment_ref"].is_string()) paymentRef = j["payment_ref"].get<std::string>();
        if (j.contains("amount")      && j["amount"].is_number())      amount     = j["amount"].get<double>();
        if (j.contains("statement_id")) statementId = m2oToId_(j["statement_id"]);
        if (j.contains("partner_id"))   partnerId   = m2oToId_(j["partner_id"]);
        if (j.contains("company_id"))   companyId   = m2oToId_(j["company_id"]);
    }
};

// Statement lines viewmodel: list by statement, suggest matches, reconcile.
class AccountBankStatementLineViewModel : public core::BaseViewModel {
public:
    explicit AccountBankStatementLineViewModel(std::shared_ptr<infrastructure::DbConnection> db) : db_(std::move(db)) {
        REGISTER_METHOD("search_read",    handleSearchRead)
        REGISTER_METHOD("read",           handleRead)
        REGISTER_MUTATOR("create",         handleCreate)
        REGISTER_MUTATOR("write",          handleWrite)
        REGISTER_MUTATOR("unlink",         handleUnlink)
        REGISTER_METHOD("fields_get",     handleFieldsGet)
        REGISTER_METHOD("suggest_matches",handleSuggestMatches)
        REGISTER_METHOD("reconcile",      handleReconcile)
    }
    std::string modelName() const override { return "account.bank.statement.line"; }
private:
    std::shared_ptr<infrastructure::DbConnection> db_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        int stmtFilter = 0;
        { auto d = call.domain(); if (d.is_array()) for (const auto& c : d)
            if (c.is_array() && c.size()==3 && c[0].is_string() && c[0].get<std::string>()=="statement_id" && c[2].is_number_integer())
                stmtFilter = c[2].get<int>(); }
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        std::string sql = R"(
            SELECT l.id, l.statement_id, l.date, l.name, l.payment_ref, l.amount,
                   l.is_reconciled, l.partner_id, rp.name AS partner_name, l.reconciled_move_id
            FROM account_bank_statement_line l LEFT JOIN res_partner rp ON rp.id = l.partner_id WHERE TRUE )";
        pqxx::params p; int pc = 0;
        if (stmtFilter > 0) { sql += " AND l.statement_id=$1"; p.append(stmtFilter); pc = 1; }
        // S-30: enforce ir.rule on this custom read (record-rule bypass fix, 071 §1.2).
        core::appendRecordRuleSubquery(sql, p, "account.bank.statement.line", core::RuleOp::Read,
                                       extractContext_(call), "account_bank_statement_line", "l.id", pc);
        sql += " ORDER BY l.date, l.id";
        auto res = txn.exec(sql, p);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json j;
            j["id"]            = row["id"].as<int>();
            j["statement_id"]  = row["statement_id"].is_null() ? nlohmann::json(false) : nlohmann::json(row["statement_id"].as<int>());
            j["date"]          = row["date"].is_null() ? nlohmann::json(false) : nlohmann::json(row["date"].c_str());
            j["name"]          = row["name"].is_null() ? nlohmann::json(false) : nlohmann::json(row["name"].c_str());
            j["payment_ref"]   = row["payment_ref"].is_null() ? nlohmann::json(false) : nlohmann::json(row["payment_ref"].c_str());
            j["amount"]        = core::Money::fromMicros(row["amount"].as<long long>(0)).toJson();
            j["is_reconciled"] = row["is_reconciled"].as<bool>(false);
            j["partner_id"]    = row["partner_id"].is_null() ? nlohmann::json(false)
                : nlohmann::json::array({row["partner_id"].as<int>(), row["partner_name"].is_null()?"":std::string(row["partner_name"].c_str())});
            arr.push_back(std::move(j));
        }
        return arr;
    }
    nlohmann::json handleRead(const core::CallKwArgs& call)      { AccountBankStatementLine p(db_); p.setUserContext(extractContext_(call)); return p.read(call.ids(), call.fields()); }
    nlohmann::json handleCreate(const core::CallKwArgs& call)    { AccountBankStatementLine p(db_); p.setUserContext(extractContext_(call)); return p.create(call.arg(0)); }
    nlohmann::json handleWrite(const core::CallKwArgs& call)     { AccountBankStatementLine p(db_); p.setUserContext(extractContext_(call)); return p.write(call.ids(), call.arg(1)); }
    nlohmann::json handleUnlink(const core::CallKwArgs& call)    { AccountBankStatementLine p(db_); p.setUserContext(extractContext_(call)); return p.unlink(call.ids()); }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) { AccountBankStatementLine p(db_); return p.fieldsGet(call.fields()); }

    // For the UI: open invoices/bills that could clear this line (best matches
    // — equal residual, then same partner — first).
    nlohmann::json handleSuggestMatches(const core::CallKwArgs& call) {
        int lineId = 0;
        { const auto v = call.arg(0);
          if (v.is_object() && v.contains("line_id")) lineId = m2oToId_(v["line_id"]);
          else if (!call.ids().empty()) lineId = call.ids().front(); }
        if (lineId <= 0) throw std::runtime_error("suggest_matches: line_id required");
        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        auto ln = txn.exec("SELECT amount, partner_id FROM account_bank_statement_line WHERE id=$1", pqxx::params{lineId});
        if (ln.empty()) throw std::runtime_error("statement line not found");
        const long long amt = ln[0]["amount"].as<long long>(0);
        const long long a   = amt < 0 ? -amt : amt;
        const int partner   = ln[0]["partner_id"].is_null() ? 0 : ln[0]["partner_id"].as<int>();
        auto res = txn.exec(
            "SELECT id, name, partner_id, amount_residual, move_type FROM account_move "
            "WHERE state='posted' AND amount_residual > 0 "
            "AND move_type IN ('out_invoice','in_invoice','out_refund','in_refund') "
            "ORDER BY (amount_residual = $1) DESC, (partner_id = $2) DESC, date LIMIT 20",
            pqxx::params{a, partner});
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : res)
            arr.push_back({{"id", r["id"].as<int>()},
                           {"name", r["name"].is_null()?"":r["name"].c_str()},
                           {"move_type", r["move_type"].is_null()?"":r["move_type"].c_str()},
                           {"amount_residual", core::Money::fromMicros(r["amount_residual"].as<long long>(0)).toJson()}});
        return arr;
    }

    // Reconcile a statement line against an invoice/bill: post the bank entry
    // (Dr Bank / Cr Receivable on an inflow), clear the invoice residual, and
    // mark the line reconciled.
    nlohmann::json handleReconcile(const core::CallKwArgs& call) {
        int lineId = 0, moveId = 0;
        const auto v = call.arg(0);
        if (v.is_object()) { lineId = m2oToId_(v.contains("line_id")?v["line_id"]:nlohmann::json(0)); moveId = m2oToId_(v.contains("move_id")?v["move_id"]:nlohmann::json(0)); }
        else if (v.is_number_integer()) { lineId = v.get<int>(); moveId = m2oToId_(call.arg(1)); }
        if (lineId <= 0 || moveId <= 0) throw std::runtime_error("reconcile: line_id and move_id required");

        auto conn = db_->acquire(); pqxx::work txn{conn.get()};
        auto ln = txn.exec(
            "SELECT bsl.amount, bsl.company_id, bs.journal_id, bsl.is_reconciled, bsl.partner_id "
            "FROM account_bank_statement_line bsl JOIN account_bank_statement bs ON bs.id=bsl.statement_id "
            "WHERE bsl.id=$1", pqxx::params{lineId});
        if (ln.empty()) throw std::runtime_error("statement line not found");
        if (ln[0]["is_reconciled"].as<bool>(false)) throw std::runtime_error("line already reconciled");
        const long long amount = ln[0]["amount"].as<long long>(0);
        const int comp = ln[0]["company_id"].is_null() ? 1 : ln[0]["company_id"].as<int>();
        const int jid  = ln[0]["journal_id"].as<int>();
        const int lnPartner = ln[0]["partner_id"].is_null() ? 0 : ln[0]["partner_id"].as<int>();

        // 071 §1.5: revalidate the target move before mutating it. Without this a
        // billing user could drive an unrelated or already-paid move to 'paid' and
        // post a bank entry against it. It must be a posted, still-open customer/
        // vendor invoice in the SAME company (and same partner, when both name one).
        {
            auto mv = txn.exec(
                "SELECT state, amount_residual, move_type, company_id, partner_id "
                "FROM account_move WHERE id=$1", pqxx::params{moveId});
            if (mv.empty()) throw infrastructure::ValidationError("Reconcile: invoice not found.");
            const std::string mState = mv[0]["state"].c_str();
            const std::string mType  = mv[0]["move_type"].c_str();
            const long long   mResid = mv[0]["amount_residual"].is_null() ? 0 : mv[0]["amount_residual"].as<long long>(0);
            const int mComp    = mv[0]["company_id"].is_null() ? 0 : mv[0]["company_id"].as<int>();
            const int mPartner = mv[0]["partner_id"].is_null() ? 0 : mv[0]["partner_id"].as<int>();
            const bool isInvoice = (mType=="out_invoice"||mType=="in_invoice"||mType=="out_refund"||mType=="in_refund");
            if (mState != "posted") throw infrastructure::ValidationError("Reconcile: the invoice is not posted.");
            if (!isInvoice)         throw infrastructure::ValidationError("Reconcile: the target is not a customer/vendor invoice.");
            if (mResid <= 0)        throw infrastructure::ValidationError("Reconcile: the invoice is already fully paid.");
            if (mComp > 0 && comp != mComp)
                                    throw infrastructure::ValidationError("Reconcile: the invoice belongs to a different company.");
            if (lnPartner > 0 && mPartner > 0 && lnPartner != mPartner)
                                    throw infrastructure::ValidationError("Reconcile: the invoice partner does not match the statement line.");
        }

        // Bank account = journal default, else code 1100.
        int bankAcct = 0;
        { auto r = txn.exec("SELECT default_account_id FROM account_journal WHERE id=$1", pqxx::params{jid});
          if (!r.empty() && !r[0][0].is_null()) bankAcct = r[0][0].as<int>(); }
        if (bankAcct <= 0) { auto r = txn.exec("SELECT id FROM account_account WHERE code='1100' AND company_id=$1 LIMIT 1", pqxx::params{comp}); bankAcct = r.empty()?0:r[0][0].as<int>(); }
        // Counterpart = the invoice's receivable/payable account, else 1200.
        int cpAcct = 0;
        { auto r = txn.exec(
            "SELECT aml.account_id FROM account_move_line aml JOIN account_account a ON a.id=aml.account_id "
            "WHERE aml.move_id=$1 AND a.account_type IN ('asset_receivable','liability_payable') LIMIT 1",
            pqxx::params{moveId}); if (!r.empty()) cpAcct = r[0][0].as<int>(); }
        if (cpAcct <= 0) { auto r = txn.exec("SELECT id FROM account_account WHERE code='1200' AND company_id=$1 LIMIT 1", pqxx::params{comp}); cpAcct = r.empty()?0:r[0][0].as<int>(); }
        if (bankAcct <= 0 || cpAcct <= 0) throw std::runtime_error("bank or counterpart account not configured");

        // Post the bank entry.
        const bool     inflow = amount >= 0;
        const long long a = amount < 0 ? -amount : amount;
        const int moveJe = txn.exec(
            "INSERT INTO account_move (name, move_type, state, date, journal_id, company_id) "
            "VALUES ('/','entry','posted',CURRENT_DATE,$1,$2) RETURNING id", pqxx::params{jid, comp})[0][0].as<int>();
        txn.exec("UPDATE account_move SET name=$2 WHERE id=$1", pqxx::params{moveJe, std::string("BNK/") + std::to_string(moveJe)});
        auto jeLine = [&](int acct, long long dr, long long cr) {
            txn.exec("INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, date, name, debit, credit) "
                     "VALUES ($1,$2,$3,$4,CURRENT_DATE,'Bank reconciliation',$5,$6)",
                     pqxx::params{moveJe, acct, jid, comp, dr, cr});
        };
        jeLine(bankAcct, inflow ? a : 0, inflow ? 0 : a);
        jeLine(cpAcct,   inflow ? 0 : a, inflow ? a : 0);

        // Clear the invoice residual + payment_state.
        txn.exec(
            "UPDATE account_move SET "
            "  amount_residual = GREATEST(amount_residual - $2, 0), "
            "  payment_state = CASE WHEN amount_residual - $2 <= 0 THEN 'paid' ELSE 'partial' END, "
            "  write_date=now() WHERE id=$1", pqxx::params{moveId, a});
        // Mark the line reconciled.
        txn.exec("UPDATE account_bank_statement_line SET is_reconciled=TRUE, reconciled_move_id=$2, write_date=now() WHERE id=$1",
                 pqxx::params{lineId, moveJe});
        txn.commit();
        nlohmann::json out; out["bank_move_id"] = moveJe; out["invoice_id"] = moveId; return out;
    }
};

class BankStatementListView : public core::BaseView {
    std::string model_, name_, label_; bool isLine_;
public:
    BankStatementListView(std::string model, std::string name, std::string label, bool isLine)
        : model_(std::move(model)), name_(std::move(name)), label_(std::move(label)), isLine_(isLine) {}
    std::string viewName()  const override { return name_; }
    std::string modelName() const override { return model_; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        if (isLine_)
            return "<list string=\"" + label_ + "\"><field name=\"date\"/><field name=\"name\"/>"
                   "<field name=\"partner_id\"/><field name=\"amount\"/><field name=\"is_reconciled\"/></list>";
        return "<list string=\"" + label_ + "\"><field name=\"name\"/><field name=\"date\"/>"
               "<field name=\"journal_id\"/><field name=\"balance_end\"/><field name=\"state\"/></list>";
    }
    nlohmann::json fields() const override {
        if (isLine_) return {
            {"date",          {{"type","date"},     {"string","Date"}}},
            {"name",          {{"type","char"},     {"string","Label"}}},
            {"partner_id",    {{"type","many2one"}, {"string","Partner"}, {"relation","res.partner"}}},
            {"amount",        {{"type","monetary"}, {"string","Amount"}}},
            {"is_reconciled", {{"type","boolean"},  {"string","Reconciled"}}},
        };
        return {
            {"name",        {{"type","char"},      {"string","Reference"}}},
            {"date",        {{"type","date"},      {"string","Date"}}},
            {"journal_id",  {{"type","many2one"},  {"string","Bank Journal"}, {"relation","account.journal"}}},
            {"balance_end", {{"type","monetary"},  {"string","Ending Balance"}}},
            {"state",       {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// 3. MODULE — method implementations
// ================================================================

AccountModule::AccountModule(core::ModelFactory&     modelFactory,
                             core::ServiceFactory&   serviceFactory,
                             core::ViewModelFactory& viewModelFactory,
                             core::ViewFactory&      viewFactory)
    : models_    (modelFactory)
    , services_  (serviceFactory)
    , viewModels_(viewModelFactory)
    , views_     (viewFactory)
{}

std::string AccountModule::moduleName() const { return "account"; }
std::string AccountModule::version()    const { return "19.0.1.0.0"; }
std::vector<std::string> AccountModule::dependencies() const { return {"ir"}; }

// ================================================================
// Fixed Assets — register, straight-line depreciation, entries (docs/084)
// ================================================================

// Asset type / category: the template that seeds an asset's schedule shape and
// the three accounts a depreciation entry touches.
class AccountAssetType : public core::BaseModel<AccountAssetType> {
public:
    ODOO_MODEL("account.asset.type", "account_asset_type")
    std::string name;
    int number = 5;            // number of depreciation entries
    int periodMonths = 12;     // months between entries (12 = yearly, 1 = monthly)
    int accountAssetId = 0, accountDepreciationId = 0, accountExpenseId = 0, journalId = 0, companyId = 1;
    explicit AccountAssetType(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAssetType>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",                    core::FieldType::Char,     "Asset Type", true});
        fieldRegistry_.add({"number",                  core::FieldType::Integer,  "Number of Depreciations"});
        fieldRegistry_.add({"period_months",           core::FieldType::Integer,  "Months per Period"});
        fieldRegistry_.add({"account_asset_id",        core::FieldType::Many2one, "Asset Account",        false,false,true,false,"account.account"});
        fieldRegistry_.add({"account_depreciation_id", core::FieldType::Many2one, "Depreciation Account", false,false,true,false,"account.account"});
        fieldRegistry_.add({"account_expense_id",      core::FieldType::Many2one, "Expense Account",      false,false,true,false,"account.account"});
        fieldRegistry_.add({"journal_id",              core::FieldType::Many2one, "Journal",              false,false,true,false,"account.journal"});
        fieldRegistry_.add({"company_id",              core::FieldType::Many2one, "Company",              false,false,true,false,"res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["number"] = number; j["period_months"] = periodMonths;
        j["account_asset_id"]        = accountAssetId        > 0 ? nlohmann::json(accountAssetId)        : nlohmann::json(false);
        j["account_depreciation_id"] = accountDepreciationId > 0 ? nlohmann::json(accountDepreciationId) : nlohmann::json(false);
        j["account_expense_id"]      = accountExpenseId      > 0 ? nlohmann::json(accountExpenseId)      : nlohmann::json(false);
        j["journal_id"]              = journalId             > 0 ? nlohmann::json(journalId)             : nlohmann::json(false);
        j["company_id"]              = companyId             > 0 ? nlohmann::json(companyId)             : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("number") && j["number"].is_number()) number = j["number"].get<int>();
        if (j.contains("period_months") && j["period_months"].is_number()) periodMonths = j["period_months"].get<int>();
        if (j.contains("account_asset_id"))        accountAssetId        = m2oToId_(j["account_asset_id"]);
        if (j.contains("account_depreciation_id")) accountDepreciationId = m2oToId_(j["account_depreciation_id"]);
        if (j.contains("account_expense_id"))      accountExpenseId      = m2oToId_(j["account_expense_id"]);
        if (j.contains("journal_id"))              journalId             = m2oToId_(j["journal_id"]);
        if (j.contains("company_id"))              companyId             = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Asset type name is required"); return e;
    }
};

// A depreciable fixed asset.
class AccountAsset : public core::BaseModel<AccountAsset> {
public:
    ODOO_MODEL("account.asset", "account_asset")
    std::string name;
    int    assetTypeId = 0;
    double value = 0.0;           // gross cost
    double valueResidual = 0.0;   // net book value (remaining)
    std::string acquisitionDate;
    int    number = 5, periodMonths = 12;
    int    accountAssetId = 0, accountDepreciationId = 0, accountExpenseId = 0, journalId = 0, companyId = 1;
    std::string state = "draft"; // draft | open | close
    explicit AccountAsset(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAsset>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",                    core::FieldType::Char,      "Asset Name", true});
        fieldRegistry_.add({"asset_type_id",           core::FieldType::Many2one,  "Asset Type",  false,false,true,false,"account.asset.type"});
        fieldRegistry_.add({"value",                   core::FieldType::Monetary,  "Gross Value"});
        fieldRegistry_.add({.name="value_residual",    .type=core::FieldType::Monetary, .string="Book Value", .readonly=true});
        fieldRegistry_.add({"acquisition_date",        core::FieldType::Date,      "Acquisition Date"});
        fieldRegistry_.add({"number",                  core::FieldType::Integer,   "Number of Depreciations"});
        fieldRegistry_.add({"period_months",           core::FieldType::Integer,   "Months per Period"});
        fieldRegistry_.add({"account_asset_id",        core::FieldType::Many2one,  "Asset Account",        false,false,true,false,"account.account"});
        fieldRegistry_.add({"account_depreciation_id", core::FieldType::Many2one,  "Depreciation Account", false,false,true,false,"account.account"});
        fieldRegistry_.add({"account_expense_id",      core::FieldType::Many2one,  "Expense Account",      false,false,true,false,"account.account"});
        fieldRegistry_.add({"journal_id",              core::FieldType::Many2one,  "Journal",              false,false,true,false,"account.journal"});
        fieldRegistry_.add({.name="state",             .type=core::FieldType::Selection, .string="Status", .readonly=true});
        fieldRegistry_.add({"company_id",              core::FieldType::Many2one,  "Company",              false,false,true,false,"res.company"});
        fieldRegistry_.markScaled({"value", "value_residual"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;
        j["asset_type_id"] = assetTypeId > 0 ? nlohmann::json(assetTypeId) : nlohmann::json(false);
        j["value"] = value; j["value_residual"] = valueResidual;
        j["acquisition_date"] = acquisitionDate.empty() ? nlohmann::json(false) : nlohmann::json(acquisitionDate);
        j["number"] = number; j["period_months"] = periodMonths;
        j["account_asset_id"]        = accountAssetId        > 0 ? nlohmann::json(accountAssetId)        : nlohmann::json(false);
        j["account_depreciation_id"] = accountDepreciationId > 0 ? nlohmann::json(accountDepreciationId) : nlohmann::json(false);
        j["account_expense_id"]      = accountExpenseId      > 0 ? nlohmann::json(accountExpenseId)      : nlohmann::json(false);
        j["journal_id"]              = journalId             > 0 ? nlohmann::json(journalId)             : nlohmann::json(false);
        j["state"] = state;
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("asset_type_id")) assetTypeId = m2oToId_(j["asset_type_id"]);
        if (j.contains("value") && j["value"].is_number()) value = j["value"].get<double>();
        if (j.contains("value_residual") && j["value_residual"].is_number()) valueResidual = j["value_residual"].get<double>();
        if (j.contains("acquisition_date") && j["acquisition_date"].is_string()) acquisitionDate = j["acquisition_date"].get<std::string>();
        if (j.contains("number") && j["number"].is_number()) number = j["number"].get<int>();
        if (j.contains("period_months") && j["period_months"].is_number()) periodMonths = j["period_months"].get<int>();
        if (j.contains("account_asset_id"))        accountAssetId        = m2oToId_(j["account_asset_id"]);
        if (j.contains("account_depreciation_id")) accountDepreciationId = m2oToId_(j["account_depreciation_id"]);
        if (j.contains("account_expense_id"))      accountExpenseId      = m2oToId_(j["account_expense_id"]);
        if (j.contains("journal_id"))              journalId             = m2oToId_(j["journal_id"]);
        if (j.contains("state") && j["state"].is_string()) state = j["state"].get<std::string>();
        if (j.contains("company_id"))              companyId             = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Asset name is required");
        return e;
    }
};

// One row of the depreciation schedule.
class AccountAssetDepreciationLine : public core::BaseModel<AccountAssetDepreciationLine> {
public:
    ODOO_MODEL("account.asset.depreciation.line", "account_asset_depreciation_line")
    int    assetId = 0, sequence = 0, moveId = 0;
    std::string depreciationDate;
    double amount = 0.0, remainingValue = 0.0, depreciatedValue = 0.0;
    bool   posted = false;
    explicit AccountAssetDepreciationLine(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAssetDepreciationLine>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"asset_id",          core::FieldType::Many2one, "Asset", false,false,true,false,"account.asset"});
        fieldRegistry_.add({"sequence",          core::FieldType::Integer,  "#"});
        fieldRegistry_.add({"depreciation_date", core::FieldType::Date,     "Date"});
        fieldRegistry_.add({"amount",            core::FieldType::Monetary, "Depreciation"});
        fieldRegistry_.add({"remaining_value",   core::FieldType::Monetary, "Book Value"});
        fieldRegistry_.add({"depreciated_value", core::FieldType::Monetary, "Cumulative"});
        fieldRegistry_.add({"move_id",           core::FieldType::Many2one, "Journal Entry", false,false,true,false,"account.move"});
        fieldRegistry_.add({"posted",            core::FieldType::Boolean,  "Posted"});
        fieldRegistry_.markScaled({"amount", "remaining_value", "depreciated_value"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["asset_id"]          = assetId > 0 ? nlohmann::json(assetId) : nlohmann::json(false);
        j["sequence"]          = sequence;
        j["depreciation_date"] = depreciationDate.empty() ? nlohmann::json(false) : nlohmann::json(depreciationDate);
        j["amount"]            = amount; j["remaining_value"] = remainingValue; j["depreciated_value"] = depreciatedValue;
        j["move_id"]           = moveId > 0 ? nlohmann::json(moveId) : nlohmann::json(false);
        j["posted"]            = posted;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("asset_id")) assetId = m2oToId_(j["asset_id"]);
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (j.contains("depreciation_date") && j["depreciation_date"].is_string()) depreciationDate = j["depreciation_date"].get<std::string>();
        if (j.contains("amount") && j["amount"].is_number()) amount = j["amount"].get<double>();
        if (j.contains("remaining_value") && j["remaining_value"].is_number()) remainingValue = j["remaining_value"].get<double>();
        if (j.contains("depreciated_value") && j["depreciated_value"].is_number()) depreciatedValue = j["depreciated_value"].get<double>();
        if (j.contains("move_id")) moveId = m2oToId_(j["move_id"]);
        if (j.contains("posted") && j["posted"].is_boolean()) posted = j["posted"].get<bool>();
    }
};

// Asset workflow: confirm (build the straight-line schedule) and depreciate
// (post the due depreciation journal entries).
class AccountAssetViewModel : public AccountViewModel<AccountAsset> {
public:
    explicit AccountAssetViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : AccountViewModel<AccountAsset>(std::move(db))
    {
        REGISTER_METHOD("action_confirm",    handleConfirm)
        REGISTER_METHOD("action_depreciate", handleDepreciate)
        REGISTER_METHOD("action_close",      handleClose)
    }
    std::string modelName() const override { return "account.asset"; }

private:
    // Build the straight-line schedule and open the asset.
    nlohmann::json handleConfirm(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            auto a = txn.exec(
                "SELECT value, number, period_months, acquisition_date, asset_type_id, state, "
                "  account_asset_id, account_depreciation_id, account_expense_id, journal_id "
                "FROM account_asset WHERE id=$1", pqxx::params{id});
            if (a.empty()) throw std::runtime_error("Asset not found");
            if (std::string(a[0]["state"].c_str()) != "draft")
                throw infrastructure::ValidationError("Only a draft asset can be confirmed.");
            long long value = a[0]["value"].as<long long>(0);
            int number = a[0]["number"].as<int>(5);
            int period = a[0]["period_months"].as<int>(12);
            if (number < 1) number = 1;
            if (value <= 0) throw infrastructure::ValidationError("Set a gross value greater than zero first.");
            // Inherit missing accounts/schedule from the asset type.
            int assetTypeId = a[0]["asset_type_id"].is_null() ? 0 : a[0]["asset_type_id"].as<int>();
            if (assetTypeId > 0) {
                txn.exec(
                    "UPDATE account_asset t SET "
                    " account_asset_id        = COALESCE(NULLIF(t.account_asset_id,0),        y.account_asset_id), "
                    " account_depreciation_id = COALESCE(NULLIF(t.account_depreciation_id,0), y.account_depreciation_id), "
                    " account_expense_id      = COALESCE(NULLIF(t.account_expense_id,0),      y.account_expense_id), "
                    " journal_id              = COALESCE(NULLIF(t.journal_id,0),              y.journal_id) "
                    "FROM account_asset_type y WHERE y.id=$2 AND t.id=$1", pqxx::params{id, assetTypeId});
            }
            std::string acq = a[0]["acquisition_date"].is_null()
                ? "" : std::string(a[0]["acquisition_date"].c_str());
            // Rebuild the schedule.
            txn.exec("DELETE FROM account_asset_depreciation_line WHERE asset_id=$1 AND posted=FALSE", pqxx::params{id});
            long long already = txn.exec(
                "SELECT COALESCE(SUM(amount),0) FROM account_asset_depreciation_line WHERE asset_id=$1 AND posted=TRUE",
                pqxx::params{id})[0][0].as<long long>(0);
            int postedCount = txn.exec(
                "SELECT COUNT(*) FROM account_asset_depreciation_line WHERE asset_id=$1 AND posted=TRUE",
                pqxx::params{id})[0][0].as<int>(0);
            long long remaining = value - already;
            int left = number - postedCount;
            if (left < 1) { txn.exec("UPDATE account_asset SET state='open' WHERE id=$1", pqxx::params{id}); continue; }
            long long per = remaining / left;
            long long cumulative = already;
            for (int k = 0; k < left; ++k) {
                long long amt = (k == left - 1) ? (remaining - per * (left - 1)) : per; // last line absorbs rounding
                cumulative += amt;
                const int seq = postedCount + k + 1;
                // date = acquisition_date + (seq * period) months
                txn.exec(
                    "INSERT INTO account_asset_depreciation_line "
                    "(asset_id, sequence, depreciation_date, amount, remaining_value, depreciated_value, posted) "
                    "VALUES ($1, $2, (COALESCE($3::date, CURRENT_DATE) + ($4 * INTERVAL '1 month')), $5, $6, $7, FALSE)",
                    pqxx::params{id, seq, acq.empty() ? nullptr : acq.c_str(), seq * period,
                                 amt, value - cumulative, cumulative});
            }
            txn.exec("UPDATE account_asset SET state='open', value_residual = value - $2 WHERE id=$1",
                     pqxx::params{id, already});
        }
        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log("account.asset", "action_confirm", ids, extractContext_(call).uid);
        return true;
    }

    // Post the depreciation journal entry for every due (date <= as_of), not-yet
    // posted schedule line: Dr expense / Cr accumulated depreciation.
    nlohmann::json handleDepreciate(const core::CallKwArgs& call) {
        std::string asOf;
        if (call.kwargs.contains("date") && call.kwargs["date"].is_string())
            asOf = call.kwargs["date"].get<std::string>();
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        int posted = 0;
        for (int id : ids) {
            auto a = txn.exec(
                "SELECT company_id, journal_id, account_expense_id, account_depreciation_id, name "
                "FROM account_asset WHERE id=$1", pqxx::params{id});
            if (a.empty()) continue;
            const int companyId = a[0]["company_id"].is_null() ? 1 : a[0]["company_id"].as<int>();
            const int journalId = a[0]["journal_id"].is_null() ? 0 : a[0]["journal_id"].as<int>();
            const int expId     = a[0]["account_expense_id"].is_null() ? 0 : a[0]["account_expense_id"].as<int>();
            const int depId     = a[0]["account_depreciation_id"].is_null() ? 0 : a[0]["account_depreciation_id"].as<int>();
            const std::string aname = a[0]["name"].c_str();
            if (!journalId || !expId || !depId)
                throw infrastructure::ValidationError("Set the journal, expense and depreciation accounts on the asset first.");
            std::string jcode = txn.exec("SELECT code FROM account_journal WHERE id=$1",
                                         pqxx::params{journalId})[0][0].c_str();
            std::string seqCode = "account.move." + jcode;
            txn.exec("INSERT INTO ir_sequence (code,name,prefix,padding,reset_policy) "
                     "VALUES ($1,$2,$3,4,'yearly') ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING",
                     pqxx::params{seqCode, "Journal — " + jcode, jcode + "/%(year)s/"});
            auto due = txn.exec(
                "SELECT id, to_char(depreciation_date,'YYYY-MM-DD') dt, amount "
                "FROM account_asset_depreciation_line "
                "WHERE asset_id=$1 AND posted=FALSE AND depreciation_date <= COALESCE($2::date, CURRENT_DATE) "
                "ORDER BY sequence", pqxx::params{id, asOf.empty() ? nullptr : asOf.c_str()});
            for (const auto& d : due) {
                const int lineId    = d["id"].as<int>();
                const std::string dt = d["dt"].c_str();
                const long long amt = d["amount"].as<long long>(0);
                // Post the depreciation entry (Dr expense / Cr accumulated depreciation).
                const std::string num = core::IrSequence::instance().nextByCode(txn, seqCode);
                int moveId = txn.exec(
                    "INSERT INTO account_move (name, move_type, state, date, journal_id, company_id, "
                    " amount_untaxed, amount_tax, amount_total, amount_residual, ref) "
                    "VALUES ($1,'entry','posted',$2::date,$3,$4,0,0,$5,0,$6) RETURNING id",
                    pqxx::params{num, dt, journalId, companyId, amt, "Depreciation — " + aname})[0][0].as<int>();
                txn.exec(
                    "INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, date, name, debit, credit) "
                    "VALUES ($1,$2,$3,$4,$5::date,$6,$7,0), ($1,$8,$3,$4,$5::date,$6,0,$7)",
                    pqxx::params{moveId, expId, journalId, companyId, dt,
                                 "Depreciation — " + aname, amt, depId});
                txn.exec("UPDATE account_asset_depreciation_line SET posted=TRUE, move_id=$2 WHERE id=$1",
                         pqxx::params{lineId, moveId});
                ++posted;
            }
            // Refresh book value; close the asset when fully depreciated.
            txn.exec(
                "UPDATE account_asset SET value_residual = value - "
                "  COALESCE((SELECT SUM(amount) FROM account_asset_depreciation_line WHERE asset_id=$1 AND posted=TRUE),0), "
                "  state = CASE WHEN NOT EXISTS (SELECT 1 FROM account_asset_depreciation_line WHERE asset_id=$1 AND posted=FALSE) "
                "               THEN 'close' ELSE state END "
                "WHERE id=$1", pqxx::params{id});
        }
        txn.commit();
        if (infrastructure::AuditService::ready())
            infrastructure::AuditService::instance().log("account.asset", "action_depreciate", ids, extractContext_(call).uid);
        return posted;
    }

    nlohmann::json handleClose(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("UPDATE account_asset SET state='close' WHERE id = ANY($1::int[])",
                 pqxx::params{idsArray_(ids)});
        txn.commit();
        return true;
    }
};

// ================================================================
// Budgets — budgetary positions, budgets, planned vs actual (docs/085)
// ================================================================

// A budgetary position: the named set of GL accounts a budget line measures.
class AccountBudgetPost : public core::BaseModel<AccountBudgetPost> {
public:
    ODOO_MODEL("account.budget.post", "account_budget_post")
    std::string name;
    std::string accountIdsJson = "[]";   // ["4000","5000"] account ids
    int companyId = 1;
    explicit AccountBudgetPost(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBudgetPost>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",              core::FieldType::Char,     "Budgetary Position", true});
        fieldRegistry_.add({"account_ids_json",  core::FieldType::Text,     "Accounts (JSON ids)"});
        fieldRegistry_.add({"company_id",        core::FieldType::Many2one, "Company", false,false,true,false,"res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["account_ids_json"] = accountIdsJson;
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("account_ids_json") && j["account_ids_json"].is_string()) accountIdsJson = j["account_ids_json"].get<std::string>();
        if (j.contains("company_id")) companyId = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Budgetary position name is required"); return e;
    }
};

// A budget over a period, holding one line per budgetary position.
class AccountBudget : public core::BaseModel<AccountBudget> {
public:
    ODOO_MODEL("account.budget", "account_budget")
    std::string name, dateFrom, dateTo, state = "draft";  // draft|confirm|done
    int companyId = 1;
    explicit AccountBudget(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBudget>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char, "Budget Name", true});
        fieldRegistry_.add({"date_from",  core::FieldType::Date, "Start Date"});
        fieldRegistry_.add({"date_to",    core::FieldType::Date, "End Date"});
        fieldRegistry_.add({.name="state", .type=core::FieldType::Selection, .string="Status", .readonly=true});
        fieldRegistry_.add({"company_id", core::FieldType::Many2one, "Company", false,false,true,false,"res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;
        j["date_from"] = dateFrom.empty() ? nlohmann::json(false) : nlohmann::json(dateFrom);
        j["date_to"]   = dateTo.empty()   ? nlohmann::json(false) : nlohmann::json(dateTo);
        j["state"] = state;
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("date_from") && j["date_from"].is_string()) dateFrom = j["date_from"].get<std::string>();
        if (j.contains("date_to")   && j["date_to"].is_string())   dateTo   = j["date_to"].get<std::string>();
        if (j.contains("state") && j["state"].is_string()) state = j["state"].get<std::string>();
        if (j.contains("company_id")) companyId = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Budget name is required"); return e;
    }
};

// One budget line: a position, a planned amount, and the actual read from the ledger.
class AccountBudgetLine : public core::BaseModel<AccountBudgetLine> {
public:
    ODOO_MODEL("account.budget.line", "account_budget_line")
    int budgetId = 0, postId = 0;
    double plannedAmount = 0.0, practicalAmount = 0.0;
    explicit AccountBudgetLine(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBudgetLine>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"budget_id",        core::FieldType::Many2one, "Budget",   false,false,true,false,"account.budget"});
        fieldRegistry_.add({"post_id",          core::FieldType::Many2one, "Position", false,false,true,false,"account.budget.post"});
        fieldRegistry_.add({"planned_amount",   core::FieldType::Monetary, "Planned"});
        fieldRegistry_.add({.name="practical_amount", .type=core::FieldType::Monetary, .string="Actual", .readonly=true});
        fieldRegistry_.markScaled({"planned_amount", "practical_amount"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["budget_id"] = budgetId > 0 ? nlohmann::json(budgetId) : nlohmann::json(false);
        j["post_id"]   = postId   > 0 ? nlohmann::json(postId)   : nlohmann::json(false);
        j["planned_amount"] = plannedAmount; j["practical_amount"] = practicalAmount;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("budget_id")) budgetId = m2oToId_(j["budget_id"]);
        if (j.contains("post_id"))   postId   = m2oToId_(j["post_id"]);
        if (j.contains("planned_amount")   && j["planned_amount"].is_number())   plannedAmount   = j["planned_amount"].get<double>();
        if (j.contains("practical_amount") && j["practical_amount"].is_number()) practicalAmount = j["practical_amount"].get<double>();
    }
};

// Budget workflow: recompute actuals from the ledger, confirm, done.
class AccountBudgetViewModel : public AccountViewModel<AccountBudget> {
public:
    explicit AccountBudgetViewModel(std::shared_ptr<infrastructure::DbConnection> db)
        : AccountViewModel<AccountBudget>(std::move(db))
    {
        REGISTER_METHOD("action_compute", handleCompute)
        REGISTER_METHOD("action_confirm", handleConfirmBudget)
        REGISTER_METHOD("action_done",    handleDone)
        REGISTER_METHOD("action_draft",   handleDraft)
    }
    std::string modelName() const override { return "account.budget"; }

private:
    // Actual = net movement (debit − credit for costs, credit − debit for income)
    // on the position's accounts, within the budget period, posted only. We report
    // the ledger's natural sign per account type so an expense budget of 10,000
    // compares against 8,000 of spend as a positive number.
    nlohmann::json handleCompute(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            txn.exec(R"(
                UPDATE account_budget_line bl SET practical_amount = COALESCE((
                    SELECT SUM(CASE WHEN aa.account_type LIKE 'income%'
                                    THEN aml.credit - aml.debit
                                    ELSE aml.debit - aml.credit END)
                    FROM account_move_line aml
                    JOIN account_move  m  ON m.id  = aml.move_id AND m.state = 'posted'
                    JOIN account_account aa ON aa.id = aml.account_id
                    JOIN account_budget b   ON b.id = bl.budget_id
                    WHERE aml.date BETWEEN COALESCE(b.date_from,'1900-01-01'::date)
                                       AND COALESCE(b.date_to,  '2999-12-31'::date)
                      AND aml.account_id = ANY (
                          SELECT jsonb_array_elements_text(
                                   COALESCE(NULLIF(p.account_ids_json,'')::jsonb,'[]'::jsonb))::int
                          FROM account_budget_post p WHERE p.id = bl.post_id)
                ), 0)
                WHERE bl.budget_id = $1
            )", pqxx::params{id});
        }
        txn.commit();
        return true;
    }
    nlohmann::json setState_(const core::CallKwArgs& call, const char* st) {
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("UPDATE account_budget SET state=$2, write_date=now() WHERE id = ANY($1::int[])",
                 pqxx::params{idsArray_(ids), st});
        txn.commit();
        return true;
    }
    nlohmann::json handleConfirmBudget(const core::CallKwArgs& call) { return setState_(call, "confirm"); }
    nlohmann::json handleDone (const core::CallKwArgs& call)        { return setState_(call, "done"); }
    nlohmann::json handleDraft(const core::CallKwArgs& call)        { return setState_(call, "draft"); }
};

// ================================================================
// Configuration reference data (docs/086)
//   Account Types · Fiscal Positions (+ tax mapping) · Incoterms · Journal Groups
// All are plain reference models — they render in the polished generic form.
// ================================================================

// The account classification list behind account.account.account_type.
class AccountAccountType : public core::BaseModel<AccountAccountType> {
public:
    ODOO_MODEL("account.account.type", "account_account_type")
    std::string name, code, internalGroup;
    explicit AccountAccountType(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountAccountType>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",           core::FieldType::Char, "Account Type", true});
        fieldRegistry_.add({"code",           core::FieldType::Char, "Technical Code"});
        fieldRegistry_.add({"internal_group", core::FieldType::Char, "Internal Group"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["code"] = code; j["internal_group"] = internalGroup;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())           name          = j["name"].get<std::string>();
        if (j.contains("code")           && j["code"].is_string())           code          = j["code"].get<std::string>();
        if (j.contains("internal_group") && j["internal_group"].is_string()) internalGroup = j["internal_group"].get<std::string>();
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Account type name is required"); return e;
    }
};

// Incoterm (International Commercial Term) — a short reference list.
class AccountIncoterms : public core::BaseModel<AccountIncoterms> {
public:
    ODOO_MODEL("account.incoterms", "account_incoterms")
    std::string code, name;
    bool active = true;
    explicit AccountIncoterms(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountIncoterms>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"code",   core::FieldType::Char,    "Code", true});
        fieldRegistry_.add({"name",   core::FieldType::Char,    "Name", true});
        fieldRegistry_.add({"active", core::FieldType::Boolean, "Active"});
    }
    void serializeFields(nlohmann::json& j) const override { j["code"] = code; j["name"] = name; j["active"] = active; }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("code") && j["code"].is_string()) code = j["code"].get<std::string>();
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (code.empty()) e.push_back("Incoterm code is required");
        if (name.empty()) e.push_back("Incoterm name is required");
        return e;
    }
};

// A named group of journals (used to filter reporting).
class AccountJournalGroup : public core::BaseModel<AccountJournalGroup> {
public:
    ODOO_MODEL("account.journal.group", "account_journal_group")
    std::string name, journalIdsJson = "[]";
    int companyId = 1;
    explicit AccountJournalGroup(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountJournalGroup>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",              core::FieldType::Char,     "Journal Group", true});
        fieldRegistry_.add({"journal_ids_json",  core::FieldType::Text,     "Journals (JSON ids)"});
        fieldRegistry_.add({"company_id",        core::FieldType::Many2one, "Company", false,false,true,false,"res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["journal_ids_json"] = journalIdsJson;
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("journal_ids_json") && j["journal_ids_json"].is_string()) journalIdsJson = j["journal_ids_json"].get<std::string>();
        if (j.contains("company_id")) companyId = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Journal group name is required"); return e;
    }
};

// Fiscal position — substitutes taxes for a customer/vendor (e.g. an exempt
// or export customer). The tax substitutions live in child rows.
class AccountFiscalPosition : public core::BaseModel<AccountFiscalPosition> {
public:
    ODOO_MODEL("account.fiscal.position", "account_fiscal_position")
    std::string name, note, country;
    bool autoApply = false, active = true;
    int companyId = 1;
    explicit AccountFiscalPosition(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountFiscalPosition>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",       core::FieldType::Char,    "Fiscal Position", true});
        fieldRegistry_.add({"note",       core::FieldType::Text,    "Notes"});
        fieldRegistry_.add({"country",    core::FieldType::Char,    "Country"});
        fieldRegistry_.add({"auto_apply", core::FieldType::Boolean, "Apply Automatically"});
        fieldRegistry_.add({"active",     core::FieldType::Boolean, "Active"});
        fieldRegistry_.add({"company_id", core::FieldType::Many2one, "Company", false,false,true,false,"res.company"});
        // Renders as an editable sub-table in the generic form.
        fieldRegistry_.add({"tax_ids", core::FieldType::One2many, "Tax Mapping",
                            false, false, false, false, "account.fiscal.position.tax", "position_id"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["note"] = note; j["country"] = country;
        j["auto_apply"] = autoApply; j["active"] = active;
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")    && j["name"].is_string())    name    = j["name"].get<std::string>();
        if (j.contains("note")    && j["note"].is_string())    note    = j["note"].get<std::string>();
        if (j.contains("country") && j["country"].is_string()) country = j["country"].get<std::string>();
        if (j.contains("auto_apply") && j["auto_apply"].is_boolean()) autoApply = j["auto_apply"].get<bool>();
        if (j.contains("active")     && j["active"].is_boolean())     active    = j["active"].get<bool>();
        if (j.contains("company_id")) companyId = m2oToId_(j["company_id"]);
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Fiscal position name is required"); return e;
    }
};

// One substitution: "when this tax would apply, use that one instead".
class AccountFiscalPositionTax : public core::BaseModel<AccountFiscalPositionTax> {
public:
    ODOO_MODEL("account.fiscal.position.tax", "account_fiscal_position_tax")
    int positionId = 0, taxSrcId = 0, taxDestId = 0;
    explicit AccountFiscalPositionTax(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountFiscalPositionTax>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"position_id", core::FieldType::Many2one, "Fiscal Position", false,false,true,false,"account.fiscal.position"});
        fieldRegistry_.add({"tax_src_id",  core::FieldType::Many2one, "Tax on Product",  false,false,true,false,"account.tax"});
        fieldRegistry_.add({"tax_dest_id", core::FieldType::Many2one, "Tax to Apply",    false,false,true,false,"account.tax"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["position_id"] = positionId > 0 ? nlohmann::json(positionId) : nlohmann::json(false);
        j["tax_src_id"]  = taxSrcId   > 0 ? nlohmann::json(taxSrcId)   : nlohmann::json(false);
        j["tax_dest_id"] = taxDestId  > 0 ? nlohmann::json(taxDestId)  : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("position_id")) positionId = m2oToId_(j["position_id"]);
        if (j.contains("tax_src_id"))  taxSrcId   = m2oToId_(j["tax_src_id"]);
        if (j.contains("tax_dest_id")) taxDestId  = m2oToId_(j["tax_dest_id"]);
    }
};

// ================================================================
// Bank Accounts — the account master + its debit/credit register (docs/087)
// ================================================================
class AccountBankAccount : public core::BaseModel<AccountBankAccount> {
public:
    ODOO_MODEL("account.bank.account", "account_bank_account")
    std::string name, bankName, accountNumber;
    int journalId = 0, currencyId = 0, companyId = 1;
    bool active = true;
    explicit AccountBankAccount(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBankAccount>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"name",           core::FieldType::Char,     "Account Name", true});
        fieldRegistry_.add({"bank_name",      core::FieldType::Char,     "Bank"});
        fieldRegistry_.add({"account_number", core::FieldType::Char,     "Account Number"});
        fieldRegistry_.add({"journal_id",     core::FieldType::Many2one, "Journal",  false,false,true,false,"account.journal"});
        fieldRegistry_.add({"currency_id",    core::FieldType::Many2one, "Currency", false,false,true,false,"res.currency"});
        fieldRegistry_.add({"company_id",     core::FieldType::Many2one, "Company",  false,false,true,false,"res.company"});
        fieldRegistry_.add({"active",         core::FieldType::Boolean,  "Active"});
        fieldRegistry_.add({"line_ids", core::FieldType::One2many, "Entries",
                            false,false,false,false, "account.bank.account.line", "bank_account_id"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["bank_name"] = bankName; j["account_number"] = accountNumber;
        j["journal_id"]  = journalId  > 0 ? nlohmann::json(journalId)  : nlohmann::json(false);
        j["currency_id"] = currencyId > 0 ? nlohmann::json(currencyId) : nlohmann::json(false);
        j["company_id"]  = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())           name          = j["name"].get<std::string>();
        if (j.contains("bank_name")      && j["bank_name"].is_string())      bankName      = j["bank_name"].get<std::string>();
        if (j.contains("account_number") && j["account_number"].is_string()) accountNumber = j["account_number"].get<std::string>();
        if (j.contains("journal_id"))  journalId  = m2oToId_(j["journal_id"]);
        if (j.contains("currency_id")) currencyId = m2oToId_(j["currency_id"]);
        if (j.contains("company_id"))  companyId  = m2oToId_(j["company_id"]);
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e; if (name.empty()) e.push_back("Bank account name is required"); return e;
    }
};

// One register row: index, description, date, debit, credit.
class AccountBankAccountLine : public core::BaseModel<AccountBankAccountLine> {
public:
    ODOO_MODEL("account.bank.account.line", "account_bank_account_line")
    int bankAccountId = 0, sequence = 0;
    std::string date, name;
    double debit = 0.0, credit = 0.0;
    explicit AccountBankAccountLine(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<AccountBankAccountLine>(std::move(db)) {}
    void registerFields() override {
        fieldRegistry_.add({"bank_account_id", core::FieldType::Many2one, "Bank Account", false,false,true,false,"account.bank.account"});
        fieldRegistry_.add({"sequence",        core::FieldType::Integer,  "Index"});
        fieldRegistry_.add({"date",            core::FieldType::Date,     "Date"});
        fieldRegistry_.add({"name",            core::FieldType::Char,     "Description"});
        fieldRegistry_.add({"debit",           core::FieldType::Monetary, "Debit"});
        fieldRegistry_.add({"credit",          core::FieldType::Monetary, "Credit"});
        fieldRegistry_.markScaled({"debit", "credit"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["bank_account_id"] = bankAccountId > 0 ? nlohmann::json(bankAccountId) : nlohmann::json(false);
        j["sequence"] = sequence;
        j["date"] = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["name"] = name; j["debit"] = debit; j["credit"] = credit;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("bank_account_id")) bankAccountId = m2oToId_(j["bank_account_id"]);
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (j.contains("date") && j["date"].is_string()) date = j["date"].get<std::string>();
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("debit")  && j["debit"].is_number())  debit  = j["debit"].get<double>();
        if (j.contains("credit") && j["credit"].is_number()) credit = j["credit"].get<double>();
    }
};

void AccountModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("account.account",      [db]{ return std::make_shared<AccountAccount>(db); });
    models_.registerCreator("account.journal",      [db]{ return std::make_shared<AccountJournal>(db); });
    models_.registerCreator("account.tax",          [db]{ return std::make_shared<AccountTax>(db); });
    models_.registerCreator("account.move",         [db]{ return std::make_shared<AccountMove>(db); });
    models_.registerCreator("account.move.line",    [db]{ return std::make_shared<AccountMoveLine>(db); });
    models_.registerCreator("account.payment",      [db]{ return std::make_shared<AccountPayment>(db); });
    models_.registerCreator("account.payment.term", [db]{ return std::make_shared<AccountPaymentTerm>(db); });
    models_.registerCreator("account.analytic.account", [db]{ return std::make_shared<AccountAnalyticAccount>(db); });
    models_.registerCreator("account.analytic.line",    [db]{ return std::make_shared<AccountAnalyticLine>(db); });
    models_.registerCreator("account.bank.statement",      [db]{ return std::make_shared<AccountBankStatement>(db); });
    models_.registerCreator("account.bank.statement.line", [db]{ return std::make_shared<AccountBankStatementLine>(db); });
    models_.registerCreator("account.asset.type",          [db]{ return std::make_shared<AccountAssetType>(db); });
    models_.registerCreator("account.asset",               [db]{ return std::make_shared<AccountAsset>(db); });
    models_.registerCreator("account.asset.depreciation.line", [db]{ return std::make_shared<AccountAssetDepreciationLine>(db); });
    models_.registerCreator("account.budget.post", [db]{ return std::make_shared<AccountBudgetPost>(db); });
    models_.registerCreator("account.budget",      [db]{ return std::make_shared<AccountBudget>(db); });
    models_.registerCreator("account.budget.line", [db]{ return std::make_shared<AccountBudgetLine>(db); });
    models_.registerCreator("account.account.type",        [db]{ return std::make_shared<AccountAccountType>(db); });
    models_.registerCreator("account.incoterms",           [db]{ return std::make_shared<AccountIncoterms>(db); });
    models_.registerCreator("account.journal.group",       [db]{ return std::make_shared<AccountJournalGroup>(db); });
    models_.registerCreator("account.fiscal.position",     [db]{ return std::make_shared<AccountFiscalPosition>(db); });
    models_.registerCreator("account.fiscal.position.tax", [db]{ return std::make_shared<AccountFiscalPositionTax>(db); });
    models_.registerCreator("account.bank.account",      [db]{ return std::make_shared<AccountBankAccount>(db); });
    models_.registerCreator("account.bank.account.line", [db]{ return std::make_shared<AccountBankAccountLine>(db); });
}

void AccountModule::registerServices() {}
void AccountModule::registerRoutes()   {}

void AccountModule::registerViews() {
    views_.registerView<AccountAccountListView>  ("account.account.list");
    views_.registerView<AccountAccountFormView>  ("account.account.form");
    views_.registerView<AccountJournalListView>  ("account.journal.list");
    views_.registerView<AccountJournalFormView>  ("account.journal.form");
    views_.registerView<AccountTaxListView>      ("account.tax.list");
    views_.registerView<AccountTaxFormView>      ("account.tax.form");
    views_.registerView<AccountMoveListView>     ("account.move.list");
    views_.registerView<AccountMoveFormView>     ("account.move.form");
    views_.registerView<AccountMoveLineListView> ("account.move.line.list");
    views_.registerView<AccountMoveLineFormView> ("account.move.line.form");
    views_.registerView<AccountPaymentListView>  ("account.payment.list");
    views_.registerView<AccountPaymentFormView>  ("account.payment.form");
    views_.registerView<AccountPaymentTermListView>("account.payment.term.list");
    views_.registerView<AccountPaymentTermFormView>("account.payment.term.form");
    views_.registerCreator("account.analytic.account.list", []{ return std::make_shared<AnalyticListView>("account.analytic.account","account.analytic.account.list","Analytic Accounts",false); });
    views_.registerCreator("account.analytic.line.list",    []{ return std::make_shared<AnalyticListView>("account.analytic.line","account.analytic.line.list","Analytic Items",true); });
    views_.registerCreator("account.bank.statement.list",      []{ return std::make_shared<BankStatementListView>("account.bank.statement","account.bank.statement.list","Bank Statements",false); });
    views_.registerCreator("account.bank.statement.line.list", []{ return std::make_shared<BankStatementListView>("account.bank.statement.line","account.bank.statement.line.list","Statement Lines",true); });
}

void AccountModule::registerViewModels() {
    auto db = services_.db();

    viewModels_.registerCreator("account.account", [db]{
        return std::make_shared<AccountViewModel<AccountAccount>>(db);
    });
    viewModels_.registerCreator("account.journal", [db]{
        return std::make_shared<AccountViewModel<AccountJournal>>(db);
    });
    viewModels_.registerCreator("account.tax", [db]{
        return std::make_shared<AccountViewModel<AccountTax>>(db);
    });
    viewModels_.registerCreator("account.move", [db]{
        return std::make_shared<AccountMoveViewModel>(db);
    });
    viewModels_.registerCreator("account.move.line", [db]{
        return std::make_shared<AccountViewModel<AccountMoveLine>>(db);
    });
    viewModels_.registerCreator("account.payment", [db]{
        return std::make_shared<AccountPaymentViewModel>(db);
    });
    viewModels_.registerCreator("account.payment.term", [db]{
        return std::make_shared<AccountViewModel<AccountPaymentTerm>>(db);
    });
    viewModels_.registerCreator("account.analytic.account", [db]{
        return std::make_shared<AccountAnalyticAccountViewModel>(db);
    });
    viewModels_.registerCreator("account.analytic.line", [db]{
        return std::make_shared<AccountAnalyticLineViewModel>(db);
    });
    viewModels_.registerCreator("account.bank.statement", [db]{
        return std::make_shared<AccountViewModel<AccountBankStatement>>(db);
    });
    viewModels_.registerCreator("account.bank.statement.line", [db]{
        return std::make_shared<AccountBankStatementLineViewModel>(db);
    });
    viewModels_.registerCreator("account.asset.type", [db]{
        return std::make_shared<AccountViewModel<AccountAssetType>>(db);
    });
    viewModels_.registerCreator("account.asset", [db]{
        return std::make_shared<AccountAssetViewModel>(db);
    });
    viewModels_.registerCreator("account.asset.depreciation.line", [db]{
        return std::make_shared<AccountViewModel<AccountAssetDepreciationLine>>(db);
    });
    viewModels_.registerCreator("account.budget.post", [db]{
        return std::make_shared<AccountViewModel<AccountBudgetPost>>(db);
    });
    viewModels_.registerCreator("account.budget", [db]{
        return std::make_shared<AccountBudgetViewModel>(db);
    });
    viewModels_.registerCreator("account.budget.line", [db]{
        return std::make_shared<AccountViewModel<AccountBudgetLine>>(db);
    });
    viewModels_.registerCreator("account.account.type", [db]{
        return std::make_shared<AccountViewModel<AccountAccountType>>(db);
    });
    viewModels_.registerCreator("account.incoterms", [db]{
        return std::make_shared<AccountViewModel<AccountIncoterms>>(db);
    });
    viewModels_.registerCreator("account.journal.group", [db]{
        return std::make_shared<AccountViewModel<AccountJournalGroup>>(db);
    });
    viewModels_.registerCreator("account.fiscal.position", [db]{
        return std::make_shared<AccountViewModel<AccountFiscalPosition>>(db);
    });
    viewModels_.registerCreator("account.fiscal.position.tax", [db]{
        return std::make_shared<AccountViewModel<AccountFiscalPositionTax>>(db);
    });
    viewModels_.registerCreator("account.bank.account", [db]{
        return std::make_shared<AccountViewModel<AccountBankAccount>>(db);
    });
    viewModels_.registerCreator("account.bank.account.line", [db]{
        return std::make_shared<AccountViewModel<AccountBankAccountLine>>(db);
    });
}

void AccountModule::initialize() {
    ensureSchema_();
    seedChartOfAccounts_();
    seedJournals_();
    seedStockValuationAccounts_();
    seedTaxes_();
    seedAnalyticSchema_();
    seedPaymentTerms_();
    seedMenus_();
}

// ----------------------------------------------------------
// Schema
// ----------------------------------------------------------
void AccountModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // account_account
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_account (
            id             SERIAL PRIMARY KEY,
            name           VARCHAR NOT NULL,
            code           VARCHAR NOT NULL,
            account_type   VARCHAR NOT NULL DEFAULT 'asset_current',
            internal_group VARCHAR NOT NULL DEFAULT 'asset',
            currency_id    INTEGER REFERENCES res_currency(id),
            company_id     INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            reconcile      BOOLEAN NOT NULL DEFAULT FALSE,
            active         BOOLEAN NOT NULL DEFAULT TRUE,
            note           TEXT,
            create_date    TIMESTAMP DEFAULT now(),
            write_date     TIMESTAMP DEFAULT now(),
            UNIQUE (code, company_id)
        )
    )");

    // account_journal
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_journal (
            id                 SERIAL PRIMARY KEY,
            name               VARCHAR NOT NULL,
            code               VARCHAR(10) NOT NULL,
            type               VARCHAR NOT NULL DEFAULT 'general',
            currency_id        INTEGER REFERENCES res_currency(id),
            company_id         INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            default_account_id INTEGER REFERENCES account_account(id),
            sequence           INTEGER NOT NULL DEFAULT 10,
            active             BOOLEAN NOT NULL DEFAULT TRUE,
            create_date        TIMESTAMP DEFAULT now(),
            write_date         TIMESTAMP DEFAULT now(),
            UNIQUE (code, company_id)
        )
    )");

    // account_tax
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_tax (
            id            SERIAL PRIMARY KEY,
            name          VARCHAR NOT NULL,
            amount        NUMERIC(16,4) NOT NULL DEFAULT 0,
            amount_type   VARCHAR NOT NULL DEFAULT 'percent',
            type_tax_use  VARCHAR NOT NULL DEFAULT 'sale',
            price_include BOOLEAN NOT NULL DEFAULT FALSE,
            company_id    INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            active        BOOLEAN NOT NULL DEFAULT TRUE,
            description   VARCHAR,
            create_date   TIMESTAMP DEFAULT now(),
            write_date    TIMESTAMP DEFAULT now()
        )
    )");
    // Malaysian SST-02 classification: 'sales' | 'service' | 'other'. (docs/083)
    txn.exec("ALTER TABLE account_tax ADD COLUMN IF NOT EXISTS tax_group VARCHAR NOT NULL DEFAULT ''");
    // Classify any unclassified tax so the Tax Report can group it.
    txn.exec(
        "UPDATE account_tax SET tax_group = "
        "  CASE WHEN name ILIKE '%service%' THEN 'service' "
        "       WHEN type_tax_use = 'sale'  THEN 'sales' "
        "       ELSE 'other' END "
        "WHERE COALESCE(tax_group,'') = ''");
    // Seed the standard Malaysian SST rates (idempotent by name) so a fresh
    // chart is Malaysia-ready. The existing generic 15% taxes are left intact.
    txn.exec(
        "INSERT INTO account_tax (name, amount, amount_type, type_tax_use, tax_group, company_id) "
        "SELECT v.name, v.amount, 'percent', 'sale', v.grp, 1 "
        "FROM (VALUES ('Service Tax 8%', 8, 'service'), "
        "             ('Sales Tax 10%', 10, 'sales'), "
        "             ('Sales Tax 5%',   5, 'sales')) AS v(name, amount, grp) "
        "WHERE NOT EXISTS (SELECT 1 FROM account_tax t WHERE t.name = v.name)");

    // account_payment_term (referenced by account_move.payment_term_id)
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_payment_term (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            note        TEXT,
            lines_json  TEXT NOT NULL DEFAULT '[{"days":0,"value":"balance","value_amount":0}]',
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    // account_move
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_move (
            id              SERIAL PRIMARY KEY,
            name            VARCHAR NOT NULL DEFAULT '/',
            ref             VARCHAR,
            narration       TEXT,
            move_type       VARCHAR NOT NULL DEFAULT 'entry',
            state           VARCHAR NOT NULL DEFAULT 'draft',
            date            DATE NOT NULL DEFAULT CURRENT_DATE,
            invoice_date    DATE,
            due_date        DATE,
            journal_id      INTEGER NOT NULL REFERENCES account_journal(id),
            partner_id      INTEGER REFERENCES res_partner(id),
            company_id      INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            currency_id     INTEGER REFERENCES res_currency(id),
            payment_term_id INTEGER REFERENCES account_payment_term(id),
            invoice_origin  VARCHAR,
            payment_state   VARCHAR NOT NULL DEFAULT 'not_paid',
            amount_untaxed  NUMERIC(16,2) NOT NULL DEFAULT 0,
            amount_tax      NUMERIC(16,2) NOT NULL DEFAULT 0,
            amount_total    NUMERIC(16,2) NOT NULL DEFAULT 0,
            amount_residual NUMERIC(16,2) NOT NULL DEFAULT 0,
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");
    // migrations: add columns added after initial schema creation
    txn.exec(R"(
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS payment_term_id INTEGER REFERENCES account_payment_term(id)
    )");
    txn.exec(R"(
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS invoice_origin VARCHAR
    )");
    // Credit note / refund reversal link → the source invoice/bill it reverses.
    txn.exec(R"(
        ALTER TABLE account_move
            ADD COLUMN IF NOT EXISTS reversed_entry_id INTEGER REFERENCES account_move(id)
    )");

    // account_move_line
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_move_line (
            id               SERIAL PRIMARY KEY,
            move_id          INTEGER NOT NULL REFERENCES account_move(id) ON DELETE CASCADE,
            account_id       INTEGER NOT NULL REFERENCES account_account(id),
            journal_id       INTEGER REFERENCES account_journal(id),
            company_id       INTEGER REFERENCES res_company(id),
            date             DATE,
            name             VARCHAR,
            ref              VARCHAR,
            partner_id       INTEGER REFERENCES res_partner(id),
            debit            NUMERIC(16,2) NOT NULL DEFAULT 0,
            credit           NUMERIC(16,2) NOT NULL DEFAULT 0,
            balance          NUMERIC(16,2) GENERATED ALWAYS AS (debit - credit) STORED,
            amount_currency  NUMERIC(16,2) NOT NULL DEFAULT 0,
            quantity         NUMERIC(16,4) NOT NULL DEFAULT 1,
            price_unit       NUMERIC(16,4) NOT NULL DEFAULT 0,
            display_type     VARCHAR NOT NULL DEFAULT '',
            tax_line_id      INTEGER REFERENCES account_tax(id),
            reconciled       BOOLEAN NOT NULL DEFAULT FALSE,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        ALTER TABLE account_move_line
            ADD COLUMN IF NOT EXISTS price_unit NUMERIC(16,4) NOT NULL DEFAULT 0
    )");
    txn.exec(R"(
        ALTER TABLE account_move_line
            ADD COLUMN IF NOT EXISTS display_type VARCHAR NOT NULL DEFAULT ''
    )");

    // account_payment
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_payment (
            id            SERIAL PRIMARY KEY,
            name          VARCHAR NOT NULL DEFAULT '/',
            date          DATE NOT NULL DEFAULT CURRENT_DATE,
            journal_id    INTEGER NOT NULL REFERENCES account_journal(id),
            partner_id    INTEGER REFERENCES res_partner(id),
            company_id    INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            currency_id   INTEGER REFERENCES res_currency(id),
            amount        NUMERIC(16,2) NOT NULL DEFAULT 0,
            payment_type  VARCHAR NOT NULL DEFAULT 'inbound',
            partner_type  VARCHAR NOT NULL DEFAULT 'customer',
            state         VARCHAR NOT NULL DEFAULT 'draft',
            move_id       INTEGER REFERENCES account_move(id),
            memo          VARCHAR,
            create_date   TIMESTAMP DEFAULT now(),
            write_date    TIMESTAMP DEFAULT now()
        )
    )");

    // Fixed assets (docs/084)
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_asset_type (
            id                      SERIAL PRIMARY KEY,
            name                    VARCHAR NOT NULL,
            number                  INTEGER NOT NULL DEFAULT 5,
            period_months           INTEGER NOT NULL DEFAULT 12,
            account_asset_id        INTEGER REFERENCES account_account(id),
            account_depreciation_id INTEGER REFERENCES account_account(id),
            account_expense_id      INTEGER REFERENCES account_account(id),
            journal_id              INTEGER REFERENCES account_journal(id),
            company_id              INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            create_date             TIMESTAMP DEFAULT now(),
            write_date              TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_asset (
            id                      SERIAL PRIMARY KEY,
            name                    VARCHAR NOT NULL,
            asset_type_id           INTEGER REFERENCES account_asset_type(id),
            value                   BIGINT NOT NULL DEFAULT 0,
            value_residual          BIGINT NOT NULL DEFAULT 0,
            acquisition_date        DATE,
            number                  INTEGER NOT NULL DEFAULT 5,
            period_months           INTEGER NOT NULL DEFAULT 12,
            account_asset_id        INTEGER REFERENCES account_account(id),
            account_depreciation_id INTEGER REFERENCES account_account(id),
            account_expense_id      INTEGER REFERENCES account_account(id),
            journal_id              INTEGER REFERENCES account_journal(id),
            state                   VARCHAR NOT NULL DEFAULT 'draft',
            company_id              INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            create_date             TIMESTAMP DEFAULT now(),
            write_date              TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_asset_depreciation_line (
            id                 SERIAL PRIMARY KEY,
            asset_id           INTEGER NOT NULL REFERENCES account_asset(id) ON DELETE CASCADE,
            sequence           INTEGER NOT NULL DEFAULT 0,
            depreciation_date  DATE,
            amount             BIGINT NOT NULL DEFAULT 0,
            remaining_value    BIGINT NOT NULL DEFAULT 0,
            depreciated_value  BIGINT NOT NULL DEFAULT 0,
            move_id            INTEGER REFERENCES account_move(id),
            posted             BOOLEAN NOT NULL DEFAULT FALSE,
            create_date        TIMESTAMP DEFAULT now(),
            write_date         TIMESTAMP DEFAULT now()
        )
    )");

    // Budgets (docs/085)
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_budget_post (
            id               SERIAL PRIMARY KEY,
            name             VARCHAR NOT NULL,
            account_ids_json TEXT NOT NULL DEFAULT '[]',
            company_id       INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_budget (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            date_from   DATE,
            date_to     DATE,
            state       VARCHAR NOT NULL DEFAULT 'draft',
            company_id  INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_budget_line (
            id               SERIAL PRIMARY KEY,
            budget_id        INTEGER NOT NULL REFERENCES account_budget(id) ON DELETE CASCADE,
            post_id          INTEGER REFERENCES account_budget_post(id),
            planned_amount   BIGINT NOT NULL DEFAULT 0,
            practical_amount BIGINT NOT NULL DEFAULT 0,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");

    // Configuration reference data (docs/086)
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_account_type (
            id             SERIAL PRIMARY KEY,
            name           VARCHAR NOT NULL,
            code           VARCHAR,
            internal_group VARCHAR,
            create_date    TIMESTAMP DEFAULT now(),
            write_date     TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_incoterms (
            id          SERIAL PRIMARY KEY,
            code        VARCHAR NOT NULL,
            name        VARCHAR NOT NULL,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_journal_group (
            id               SERIAL PRIMARY KEY,
            name             VARCHAR NOT NULL,
            journal_ids_json TEXT NOT NULL DEFAULT '[]',
            company_id       INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            create_date      TIMESTAMP DEFAULT now(),
            write_date       TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_fiscal_position (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            note        TEXT,
            country     VARCHAR,
            auto_apply  BOOLEAN NOT NULL DEFAULT FALSE,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            company_id  INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_fiscal_position_tax (
            id          SERIAL PRIMARY KEY,
            position_id INTEGER NOT NULL REFERENCES account_fiscal_position(id) ON DELETE CASCADE,
            tax_src_id  INTEGER REFERENCES account_tax(id),
            tax_dest_id INTEGER REFERENCES account_tax(id),
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");
    // Seed the account-type list from the classifications the chart uses, and
    // the standard Incoterms — both idempotent (by name / code).
    txn.exec(R"(
        INSERT INTO account_account_type (name, code, internal_group)
        SELECT v.name, v.code, v.grp FROM (VALUES
            ('Bank and Cash',        'asset_cash',        'asset'),
            ('Current Assets',       'asset_current',     'asset'),
            ('Receivable',           'asset_receivable',  'asset'),
            ('Fixed Assets',         'asset_fixed',       'asset'),
            ('Current Liabilities',  'liability_current', 'liability'),
            ('Payable',              'liability_payable', 'liability'),
            ('Equity',               'equity',            'equity'),
            ('Current Year Earnings','equity_unaffected', 'equity'),
            ('Income',               'income',            'income'),
            ('Other Income',         'income_other',      'income'),
            ('Expenses',             'expense',           'expense'),
            ('Cost of Revenue',      'expense_direct_cost','expense'),
            ('Depreciation',         'expense_depreciation','expense')
        ) AS v(name, code, grp)
        WHERE NOT EXISTS (SELECT 1 FROM account_account_type t WHERE t.code = v.code)
    )");
    // The Customers / Vendors menus filter on customer_rank / vendor_rank, but a
    // partner only gets ranked when it is used. Backfill from documents already
    // on file so those lists are meaningful instead of mysteriously empty.
    txn.exec(
        "UPDATE res_partner p SET customer_rank = 1 "
        "WHERE COALESCE(p.customer_rank,0) = 0 AND EXISTS ("
        "  SELECT 1 FROM account_move m WHERE m.partner_id = p.id "
        "  AND m.move_type IN ('out_invoice','out_refund'))");
    txn.exec(
        "UPDATE res_partner p SET vendor_rank = 1 "
        "WHERE COALESCE(p.vendor_rank,0) = 0 AND EXISTS ("
        "  SELECT 1 FROM account_move m WHERE m.partner_id = p.id "
        "  AND m.move_type IN ('in_invoice','in_refund'))");

    // Bank accounts + their debit/credit register (docs/087)
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_bank_account (
            id             SERIAL PRIMARY KEY,
            name           VARCHAR NOT NULL,
            bank_name      VARCHAR,
            account_number VARCHAR,
            journal_id     INTEGER REFERENCES account_journal(id),
            currency_id    INTEGER REFERENCES res_currency(id),
            company_id     INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
            active         BOOLEAN NOT NULL DEFAULT TRUE,
            create_date    TIMESTAMP DEFAULT now(),
            write_date     TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_bank_account_line (
            id              SERIAL PRIMARY KEY,
            bank_account_id INTEGER NOT NULL REFERENCES account_bank_account(id) ON DELETE CASCADE,
            sequence        INTEGER NOT NULL DEFAULT 0,
            date            DATE,
            name            VARCHAR,
            debit           BIGINT NOT NULL DEFAULT 0,
            credit          BIGINT NOT NULL DEFAULT 0,
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        INSERT INTO account_incoterms (code, name)
        SELECT v.code, v.name FROM (VALUES
            ('EXW','Ex Works'), ('FCA','Free Carrier'), ('FAS','Free Alongside Ship'),
            ('FOB','Free on Board'), ('CFR','Cost and Freight'),
            ('CIF','Cost, Insurance and Freight'), ('CPT','Carriage Paid To'),
            ('CIP','Carriage and Insurance Paid To'), ('DAP','Delivered at Place'),
            ('DPU','Delivered at Place Unloaded'), ('DDP','Delivered Duty Paid')
        ) AS v(code, name)
        WHERE NOT EXISTS (SELECT 1 FROM account_incoterms i WHERE i.code = v.code)
    )");

    txn.commit();
}

// ----------------------------------------------------------
// Seeds
// ----------------------------------------------------------
void AccountModule::seedChartOfAccounts_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    if (txn.exec("SELECT COUNT(*) FROM account_account")[0][0].as<int>() > 0) return;

    txn.exec(R"(
        INSERT INTO account_account (code, name, account_type, internal_group, reconcile, company_id) VALUES
            ('1000', 'Cash',                  'asset_cash',        'asset',     FALSE, 1),
            ('1100', 'Bank',                  'asset_cash',        'asset',     FALSE, 1),
            ('1200', 'Accounts Receivable',   'asset_receivable',  'asset',     TRUE,  1),
            ('2000', 'Accounts Payable',      'liability_payable', 'liability', TRUE,  1),
            ('3000', 'Share Capital',          'equity',            'equity',    FALSE, 1),
            ('4000', 'Sales Revenue',          'income',            'income',    FALSE, 1),
            ('5000', 'Cost of Goods Sold',     'expense',           'expense',   FALSE, 1),
            ('6000', 'Operating Expenses',     'expense',           'expense',   FALSE, 1),
            ('9999', 'Undistributed Profit',   'equity_unaffected', 'equity',    FALSE, 1)
        ON CONFLICT (code, company_id) DO NOTHING
    )");
    txn.exec("SELECT setval('account_account_id_seq', (SELECT MAX(id) FROM account_account), true)");
    txn.commit();
}

// Stock-valuation accounts + the Inventory journal. Runs UNCONDITIONALLY every
// boot (idempotent ON CONFLICT), unlike seedChartOfAccounts_/seedJournals_ which
// only seed a fresh DB — so an existing database gains the costing accounts too.
void AccountModule::seedAnalyticSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_analytic_account (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            code        VARCHAR,
            partner_id  INTEGER REFERENCES res_partner(id) ON DELETE SET NULL,
            company_id  INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_analytic_line (
            id                 SERIAL PRIMARY KEY,
            name               VARCHAR,
            date               DATE NOT NULL DEFAULT CURRENT_DATE,
            amount             BIGINT NOT NULL DEFAULT 0,
            account_id         INTEGER NOT NULL REFERENCES account_analytic_account(id) ON DELETE CASCADE,
            general_account_id INTEGER REFERENCES account_account(id) ON DELETE SET NULL,
            move_line_id       INTEGER REFERENCES account_move_line(id) ON DELETE SET NULL,
            company_id         INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date        TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_analytic_line_account ON account_analytic_line(account_id)");
    // Journal items can be tagged to an analytic account.
    txn.exec("ALTER TABLE account_move_line ADD COLUMN IF NOT EXISTS analytic_account_id INTEGER "
             "REFERENCES account_analytic_account(id) ON DELETE SET NULL");

    // Bank reconciliation: statements + lines.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_bank_statement (
            id            SERIAL PRIMARY KEY,
            name          VARCHAR,
            date          DATE NOT NULL DEFAULT CURRENT_DATE,
            journal_id    INTEGER REFERENCES account_journal(id) ON DELETE SET NULL,
            balance_start BIGINT NOT NULL DEFAULT 0,
            balance_end   BIGINT NOT NULL DEFAULT 0,
            state         VARCHAR NOT NULL DEFAULT 'open',
            company_id    INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date   TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS account_bank_statement_line (
            id                 SERIAL PRIMARY KEY,
            statement_id       INTEGER NOT NULL REFERENCES account_bank_statement(id) ON DELETE CASCADE,
            date               DATE NOT NULL DEFAULT CURRENT_DATE,
            name               VARCHAR,
            payment_ref        VARCHAR,
            partner_id         INTEGER REFERENCES res_partner(id) ON DELETE SET NULL,
            amount             BIGINT NOT NULL DEFAULT 0,
            is_reconciled      BOOLEAN NOT NULL DEFAULT FALSE,
            reconciled_move_id INTEGER REFERENCES account_move(id) ON DELETE SET NULL,
            company_id         INTEGER REFERENCES res_company(id) ON DELETE SET NULL,
            create_date        TIMESTAMP DEFAULT now(), write_date TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_bsl_statement ON account_bank_statement_line(statement_id)");

    txn.commit();
}

void AccountModule::seedStockValuationAccounts_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO account_account (code, name, account_type, internal_group, reconcile, company_id) VALUES
            ('1400', 'Stock Valuation',           'asset_current', 'asset',   FALSE, 1),
            ('1410', 'Stock Interim (Received)',  'asset_current', 'asset',   FALSE, 1),
            ('1430', 'Stock Interim (Production)','asset_current', 'asset',   FALSE, 1),
            ('5100', 'Inventory Adjustment',      'expense',       'expense', FALSE, 1),
            ('5200', 'Landed Costs',              'expense',       'expense', FALSE, 1)
        ON CONFLICT (code, company_id) DO NOTHING
    )");
    txn.exec("SELECT setval('account_account_id_seq', (SELECT MAX(id) FROM account_account), true)");
    txn.exec("INSERT INTO account_journal (code, name, type, sequence, company_id) "
             "VALUES ('STJ', 'Inventory', 'general', 50, 1) "
             "ON CONFLICT (code, company_id) DO NOTHING");
    txn.exec("SELECT setval('account_journal_id_seq', (SELECT MAX(id) FROM account_journal), true)");
    txn.commit();
}

void AccountModule::seedJournals_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    if (txn.exec("SELECT COUNT(*) FROM account_journal")[0][0].as<int>() > 0) return;

    // Bank journal default account = 1100 (Bank), Cash = 1000 (Cash)
    // Get account ids (seeded above)
    auto bankAcc = txn.exec("SELECT id FROM account_account WHERE code='1100' AND company_id=1");
    auto cashAcc = txn.exec("SELECT id FROM account_account WHERE code='1000' AND company_id=1");
    int bankId = bankAcc.empty() ? 0 : bankAcc[0][0].as<int>();
    int cashId = cashAcc.empty() ? 0 : cashAcc[0][0].as<int>();

    if (bankId > 0 && cashId > 0) {
        txn.exec(
            "INSERT INTO account_journal (code, name, type, default_account_id, sequence, company_id) VALUES "
            "('SAL', 'Sales',     'sale',     NULL,    10, 1), "
            "('PUR', 'Purchases', 'purchase', NULL,    20, 1), "
            "('BNK', 'Bank',      'bank',     $1,      30, 1), "
            "('CSH', 'Cash',      'cash',     $2,      40, 1) "
            "ON CONFLICT (code, company_id) DO NOTHING",
            pqxx::params{bankId, cashId});
    } else {
        txn.exec(R"(
            INSERT INTO account_journal (code, name, type, sequence, company_id) VALUES
                ('SAL', 'Sales',     'sale',     10, 1),
                ('PUR', 'Purchases', 'purchase', 20, 1),
                ('BNK', 'Bank',      'bank',     30, 1),
                ('CSH', 'Cash',      'cash',     40, 1)
            ON CONFLICT (code, company_id) DO NOTHING
        )");
    }
    txn.exec("SELECT setval('account_journal_id_seq', (SELECT MAX(id) FROM account_journal), true)");
    txn.commit();
}

void AccountModule::seedTaxes_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    if (txn.exec("SELECT COUNT(*) FROM account_tax")[0][0].as<int>() > 0) return;

    txn.exec(R"(
        INSERT INTO account_tax (name, amount, amount_type, type_tax_use, company_id) VALUES
            ('15% Sales Tax',    15, 'percent', 'sale',     1),
            ('15% Purchase Tax', 15, 'percent', 'purchase', 1)
        ON CONFLICT DO NOTHING
    )");
    txn.exec("SELECT setval('account_tax_id_seq', (SELECT MAX(id) FROM account_tax), true)");
    txn.commit();
}

void AccountModule::seedPaymentTerms_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    if (txn.exec("SELECT COUNT(*) FROM account_payment_term")[0][0].as<int>() > 0) return;

    txn.exec(R"(
        INSERT INTO account_payment_term (name, lines_json) VALUES
            ('Immediate Payment', '[{"days":0,"value":"balance","value_amount":0}]'),
            ('30 Days',           '[{"days":30,"value":"percent","value_amount":100}]')
        ON CONFLICT DO NOTHING
    )");
    txn.exec("SELECT setval('account_payment_term_id_seq', (SELECT MAX(id) FROM account_payment_term), true)");
    txn.commit();
}

// ----------------------------------------------------------
// IR menu entries for account module (idempotent)
// ----------------------------------------------------------
void AccountModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context, domain) VALUES
            (4,  'Chart of Accounts',  'account.account', 'list,form', 'accounts',       '{}', NULL),
            (5,  'Journals',           'account.journal', 'list,form', 'journals',        '{}', NULL),
            (6,  'Journal Entries',    'account.move',    'list,form', 'moves',           '{}', NULL),
            (7,  'Payments',           'account.payment', 'list,form', 'payments',        '{}', NULL),
            (32, 'Customer Invoices',  'account.move',    'list,form', 'out-invoices',    '{}', '[["move_type","=","out_invoice"]]'),
            (33, 'Vendor Bills',       'account.move',    'list,form', 'in-invoices',     '{}', '[["move_type","=","in_invoice"]]'),
            (60, 'Analytic Accounts',  'account.analytic.account', 'list,form', 'analytic-accounts', '{}', NULL),
            (61, 'Analytic Items',     'account.analytic.line',    'list',      'analytic-items',    '{}', NULL),
            (62, 'Bank Statements',    'account.bank.statement',   'list,form', 'bank-statements',   '{}', NULL),
            (63, 'Bank Reconciliation','bank.reconcile',           'list',      'bank-reconcile',    '{}', NULL),
            (73, 'Financial Reports',  'account.report',           'list',      'financial-reports', '{}', NULL),
            (74, 'Credit Notes',       'account.move',    'list,form', 'out-refunds', '{}', '[["move_type","=","out_refund"]]'),
            (75, 'Vendor Refunds',     'account.move',    'list,form', 'in-refunds',  '{}', '[["move_type","=","in_refund"]]'),
            (76, 'Assets',             'account.asset',      'list,form', 'assets',      '{}', NULL),
            (77, 'Asset Types',        'account.asset.type', 'list,form', 'asset-types', '{}', NULL),
            (78, 'Budgets',            'account.budget',      'list,form', 'budgets',              '{}', NULL),
            (79, 'Budgetary Positions','account.budget.post', 'list,form', 'budgetary-positions',  '{}', NULL),
            (80, 'Currencies',         'res.currency',             'list,form', 'currencies',       '{}', NULL),
            (81, 'Account Types',      'account.account.type',     'list,form', 'account-types',    '{}', NULL),
            (82, 'Fiscal Positions',   'account.fiscal.position',  'list,form', 'fiscal-positions', '{}', NULL),
            (83, 'Incoterms',          'account.incoterms',        'list,form', 'incoterms',        '{}', NULL),
            (84, 'Journal Groups',     'account.journal.group',    'list,form', 'journal-groups',   '{}', NULL),
            (85, 'Bank Accounts',      'account.bank.account',     'list,form', 'bank-accounts',    '{}', NULL),
            (86, 'Dashboard',          'account.dashboard',        'list',      'acct-dashboard',   '{}', NULL),
            (87, 'Settings',           'account.settings',         'list',      'acct-settings',    '{}', NULL),
            -- Customers / Vendors dropdown extras + per-journal-type entry views
            (88, 'Customers',      'res.partner',     'list,form', 'acct-customers', '{}', '[["customer_rank",">",0]]'),
            (89, 'Vendors',        'res.partner',     'list,form', 'acct-vendors',   '{}', '[["vendor_rank",">",0]]'),
            (90, 'Products',       'product.product', 'list,form', 'acct-products',  '{}', NULL),
            (91, 'Sales Journal',      'account.move', 'list,form', 'jrnl-sales',     '{}', '[["move_type","=","out_invoice"]]'),
            (92, 'Purchases Journal',  'account.move', 'list,form', 'jrnl-purchases', '{}', '[["move_type","=","in_invoice"]]'),
            (93, 'Miscellaneous Journal','account.move','list,form','jrnl-misc',      '{}', '[["move_type","=","entry"]]')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=EXCLUDED.domain
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

    // Level 1: Accounting app — direct links + section header
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (39, 'Dashboard',          10,  5, 86),
            (11, 'Journal Entries',    10, 10, 6),
            (46, 'Journals',           10, 12, NULL),
            (12, 'Customers',          10, 20, NULL),
            (13, 'Vendors',            10, 30, NULL),
            (23, 'Bank Reconciliation',10, 35, 63),
            (29, 'Assets',             10, 36, 76),
            -- NOTE: menu id 31 is taken by Settings ▸ Users; use 33.
            (33, 'Budgets',            10, 37, 78),
            (24, 'Reporting',          10, 38, NULL),
            (14, 'Configuration',      10, 40, NULL)
        ON CONFLICT (id) DO NOTHING
    )");

    // Level 2: Reporting dropdown — statutory financial statements
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (25, 'Financial Reports', 24, 10, 73)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Level 2: Customers dropdown (id=15 Invoices, id=16 Payments)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (15, 'Invoices',     12, 10, 32),
            (26, 'Credit Notes', 12, 15, 74),
            (16, 'Payments',     12, 20, 7),
            (42, 'Products',     12, 30, 90),
            (43, 'Customers',    12, 40, 88)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Level 2: Vendors dropdown (id=17 Bills, 27 Refunds, 28 Payments)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (17, 'Bills',    13, 10, 33),
            (27, 'Refunds',  13, 20, 75),
            (28, 'Payments', 13, 30, 7),
            (44, 'Products', 13, 40, 90),
            (45, 'Vendors',  13, 50, 89)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Level 2: Journals dropdown — entries filtered by journal type
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (47, 'Sales',          46, 10, 91),
            (48, 'Purchases',      46, 20, 92),
            (49, 'Bank and Cash',  46, 30, 62),
            (59, 'Miscellaneous',  46, 40, 93)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    // Level 2: Configuration dropdown (id=18, 19)
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (41, 'Settings',          14,  5, 87),
            (18, 'Chart of Accounts', 14, 10, 4),
            (19, 'Journals',          14, 20, 5),
            -- ids 20/21 belong to IrModule (the Contacts app root and its entry);
            -- claiming them removed the Contacts app from the home screen. (docs/089)
            (65, 'Analytic Accounts', 14, 30, 60),
            (66, 'Analytic Items',    14, 40, 61),
            (22, 'Bank Statements',   14, 50, 62),
            -- ids 30/32 belong to IrModule (the Settings app root and its
            -- Companies entry) — using them here removed the Settings app from
            -- the home screen. Own free ids instead. (docs/089)
            (63, 'Asset Types',       14, 60, 77),
            (64, 'Budgetary Positions',14, 70, 79),
            (34, 'Currencies',        14, 80, 80),
            (35, 'Account Types',     14, 90, 81),
            (36, 'Fiscal Positions',  14,100, 82),
            (37, 'Incoterms',         14,110, 83),
            (38, 'Journal Groups',    14,120, 84),
            (40, 'Bank Accounts',     14, 55, 85)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

    txn.commit();
}

} // namespace odoo::modules::account
