#pragma once
// =============================================================
// modules/account/AccountModule.hpp
//
// Phase 6 — minimal double-entry bookkeeping module.
//
// Models:  account.account, account.journal, account.tax,
//          account.move, account.move.line,
//          account.payment, account.payment.term
//
// initialize() creates tables, seeds a minimal chart of accounts,
// four journals (SAL/PUR/BNK/CSH), two taxes, two payment terms,
// and four ir_act_window / ir_ui_menu entries (idempotent).
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include "AccountViews.hpp"
#include "MailHelpers.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>

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
        fieldRegistry_.add({"reconciled",      core::FieldType::Boolean,  "Reconciled"});
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
        REGISTER_METHOD("create",          handleCreate)
        REGISTER_METHOD("write",           handleWrite)
        REGISTER_METHOD("unlink",          handleUnlink)
        REGISTER_METHOD("fields_get",      handleFieldsGet)
        REGISTER_METHOD("search_count",    handleSearchCount)
        REGISTER_METHOD("search",          handleSearch)
    }

    std::string modelName() const override { return TModel::MODEL_NAME; }

protected:
    std::shared_ptr<infrastructure::DbConnection> db_;

    nlohmann::json handleSearchRead(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchRead(call.domain(), call.fields(),
                                call.limit() > 0 ? call.limit() : 80,
                                call.offset(), "id ASC");
    }
    nlohmann::json handleRead(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.read(call.ids(), call.fields());
    }
    nlohmann::json handleCreate(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw std::runtime_error("create: args[0] must be a dict");
        TModel proto(db_);
        return proto.create(v);
    }
    nlohmann::json handleWrite(const core::CallKwArgs& call) {
        const auto v = call.arg(1);
        if (!v.is_object()) throw std::runtime_error("write: args[1] must be a dict");
        TModel proto(db_);
        return proto.write(call.ids(), v);
    }
    nlohmann::json handleUnlink(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.unlink(call.ids());
    }
    nlohmann::json handleFieldsGet(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.fieldsGet(call.fields());
    }
    nlohmann::json handleSearchCount(const core::CallKwArgs& call) {
        TModel proto(db_);
        return proto.searchCount(call.domain());
    }
    nlohmann::json handleSearch(const core::CallKwArgs& call) {
        TModel proto(db_);
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
        REGISTER_METHOD("action_reverse",           handleButtonCancel)  // simplified
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

            // Year from date
            std::string year = date.size() >= 4 ? date.substr(0, 4) : "2026";

            // Sequence: count of posted moves for this journal+year + 1
            auto cnt = txn.exec(
                "SELECT COUNT(*) FROM account_move "
                "WHERE journal_id = $1 AND state = 'posted' "
                "AND EXTRACT(YEAR FROM date::date) = $2::int",
                pqxx::params{jid, std::stoi(year)});
            int seq = cnt[0][0].as<int>() + 1;

            std::ostringstream ss;
            ss << jcode << "/" << year << "/"
               << std::setfill('0') << std::setw(4) << seq;

            txn.exec(
                "UPDATE account_move "
                "SET state = 'posted', name = $1, write_date = now() "
                "WHERE id = $2",
                pqxx::params{ss.str(), id});
            odoo::modules::mail::postLog(txn, "account.move", id, 0,
                "Invoice posted.", "log_note");
        }

        txn.commit();
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
        return true;
    }

    nlohmann::json handleRecomputeTotals(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            auto incRow = txn.exec(
                "SELECT COALESCE(SUM(credit),0) FROM account_move_line "
                "WHERE move_id=$1 AND credit>0 AND display_type=''",
                pqxx::params{id});
            double untaxed = incRow[0][0].as<double>();

            auto mvRow = txn.exec(
                "SELECT amount_tax, payment_state FROM account_move WHERE id=$1",
                pqxx::params{id});
            if (mvRow.empty()) continue;

            double tax      = mvRow[0][0].as<double>();
            std::string ps  = mvRow[0][1].c_str();
            double total    = untaxed + tax;
            double residual = (ps == "not_paid" || ps == "partial") ? total : 0.0;

            txn.exec(
                "UPDATE account_move "
                "SET amount_untaxed=$1, amount_total=$2, amount_residual=$3, write_date=now() "
                "WHERE id=$4",
                pqxx::params{untaxed, total, residual, id});

            // Update the AR/AP line (debit > 0) to match new total
            txn.exec(
                "UPDATE account_move_line SET debit=$1, write_date=now() "
                "WHERE move_id=$2 AND debit>0",
                pqxx::params{total, id});
        }

        txn.commit();
        return true;
    }

    nlohmann::json handleActionRegisterPayment(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) return true;

        // payment_date is passed as a kwarg from the frontend
        std::string payDate;
        if (call.kwargs.contains("payment_date") && call.kwargs["payment_date"].is_string())
            payDate = call.kwargs["payment_date"].get<std::string>();

        // Fallback: today
        if (payDate.empty()) {
            auto t  = std::time(nullptr);
            auto tm = *std::localtime(&t);
            char buf[11];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            payDate = buf;
        }

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        for (int id : ids) {
            txn.exec(
                "UPDATE account_move "
                "SET payment_state='paid', amount_residual=0, write_date=now() "
                "WHERE id=$1 AND state='posted'",
                pqxx::params{id});
            odoo::modules::mail::postLog(txn, "account.move", id, 0,
                "Payment registered on " + payDate + ".", "log_note");
        }

        txn.commit();
        return true;
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
        return true;
    }
};


// ================================================================
// 3. MODULE
// ================================================================

class AccountModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "account"; }

    explicit AccountModule(core::ModelFactory&     modelFactory,
                           core::ServiceFactory&   serviceFactory,
                           core::ViewModelFactory& viewModelFactory,
                           core::ViewFactory&      viewFactory)
        : models_    (modelFactory)
        , services_  (serviceFactory)
        , viewModels_(viewModelFactory)
        , views_     (viewFactory)
    {}

    std::string              moduleName()   const override { return "account"; }
    std::string              version()      const override { return "19.0.1.0.0"; }
    std::vector<std::string> dependencies() const override { return {"ir"}; }

    void registerModels() override {
        auto db = services_.db();
        models_.registerCreator("account.account",      [db]{ return std::make_shared<AccountAccount>(db); });
        models_.registerCreator("account.journal",      [db]{ return std::make_shared<AccountJournal>(db); });
        models_.registerCreator("account.tax",          [db]{ return std::make_shared<AccountTax>(db); });
        models_.registerCreator("account.move",         [db]{ return std::make_shared<AccountMove>(db); });
        models_.registerCreator("account.move.line",    [db]{ return std::make_shared<AccountMoveLine>(db); });
        models_.registerCreator("account.payment",      [db]{ return std::make_shared<AccountPayment>(db); });
        models_.registerCreator("account.payment.term", [db]{ return std::make_shared<AccountPaymentTerm>(db); });
    }

    void registerServices() override {}
    void registerRoutes()   override {}

    void registerViews() override {
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
    }

    void registerViewModels() override {
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
    }

    void initialize() override {
        ensureSchema_();
        seedChartOfAccounts_();
        seedJournals_();
        seedTaxes_();
        seedPaymentTerms_();
        seedMenus_();
    }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    // ----------------------------------------------------------
    // Schema
    // ----------------------------------------------------------
    void ensureSchema_() {
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

        // account_payment_term
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

        txn.commit();
    }

    // ----------------------------------------------------------
    // Seeds
    // ----------------------------------------------------------
    void seedChartOfAccounts_() {
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

    void seedJournals_() {
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

    void seedTaxes_() {
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

    void seedPaymentTerms_() {
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
    void seedMenus_() {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};

        txn.exec(R"(
            INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context, domain) VALUES
                (4,  'Chart of Accounts',  'account.account', 'list,form', 'accounts',       '{}', NULL),
                (5,  'Journals',           'account.journal', 'list,form', 'journals',        '{}', NULL),
                (6,  'Journal Entries',    'account.move',    'list,form', 'moves',           '{}', NULL),
                (7,  'Payments',           'account.payment', 'list,form', 'payments',        '{}', NULL),
                (32, 'Customer Invoices',  'account.move',    'list,form', 'out-invoices',    '{}', '[["move_type","=","out_invoice"]]'),
                (33, 'Vendor Bills',       'account.move',    'list,form', 'in-invoices',     '{}', '[["move_type","=","in_invoice"]]')
            ON CONFLICT (id) DO UPDATE
                SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                    view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=EXCLUDED.domain
        )");
        txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

        // Level 1: Accounting app — direct links + section header
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (11, 'Journal Entries', 10, 10, 6),
                (12, 'Customers',       10, 20, NULL),
                (13, 'Vendors',         10, 30, NULL),
                (14, 'Configuration',   10, 40, NULL)
            ON CONFLICT (id) DO NOTHING
        )");

        // Level 2: Customers dropdown (id=15 Invoices, id=16 Payments)
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (15, 'Invoices', 12, 10, 32),
                (16, 'Payments', 12, 20, 7)
            ON CONFLICT (id) DO UPDATE
                SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                    sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
        )");

        // Level 2: Vendors dropdown (id=17 Bills)
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (17, 'Bills', 13, 10, 33)
            ON CONFLICT (id) DO UPDATE
                SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                    sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
        )");

        // Level 2: Configuration dropdown (id=18, 19)
        txn.exec(R"(
            INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
                (18, 'Chart of Accounts', 14, 10, 4),
                (19, 'Journals',          14, 20, 5)
            ON CONFLICT (id) DO UPDATE
                SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                    sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
        )");
        txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

        txn.commit();
    }
};

} // namespace odoo::modules::account
