#pragma once
// =============================================================
// modules/account/AccountViews.hpp
//
// List + Form views for all 7 account models.
// Pattern matches AuthViews.hpp.
// =============================================================
#include "BaseView.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace odoo::modules::account {

// ================================================================
// account.account
// ================================================================
class AccountAccountListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.account.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"code\"/>"
               "<field name=\"name\"/>"
               "<field name=\"account_type\"/>"
               "<field name=\"active\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"code",         {{"type","char"},      {"string","Code"},         {"required",true}}},
            {"name",         {{"type","char"},      {"string","Account Name"}, {"required",true}}},
            {"account_type", {{"type","selection"}, {"string","Type"}}},
            {"active",       {{"type","boolean"},   {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountAccountFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.account.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"code\"/>"
               "<field name=\"name\"/>"
               "<field name=\"account_type\"/>"
               "<field name=\"currency_id\"/>"
               "<field name=\"reconcile\"/>"
               "<field name=\"active\"/>"
               "<field name=\"note\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"code",         {{"type","char"},      {"string","Code"},         {"required",true}}},
            {"name",         {{"type","char"},      {"string","Account Name"}, {"required",true}}},
            {"account_type", {{"type","selection"}, {"string","Type"}}},
            {"currency_id",  {{"type","many2one"},  {"string","Currency"},  {"relation","res.currency"}}},
            {"reconcile",    {{"type","boolean"},   {"string","Can Reconcile"}}},
            {"active",       {{"type","boolean"},   {"string","Active"}}},
            {"note",         {{"type","text"},      {"string","Notes"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// account.journal
// ================================================================
class AccountJournalListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.journal.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"code\"/>"
               "<field name=\"name\"/>"
               "<field name=\"type\"/>"
               "<field name=\"currency_id\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"code",        {{"type","char"},     {"string","Code"},      {"required",true}}},
            {"name",        {{"type","char"},     {"string","Name"},      {"required",true}}},
            {"type",        {{"type","selection"},{"string","Type"}}},
            {"currency_id", {{"type","many2one"}, {"string","Currency"},  {"relation","res.currency"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountJournalFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.journal.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"name\"/>"
               "<field name=\"code\"/>"
               "<field name=\"type\"/>"
               "<field name=\"currency_id\"/>"
               "<field name=\"default_account_id\"/>"
               "<field name=\"sequence\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",               {{"type","char"},     {"string","Journal Name"},   {"required",true}}},
            {"code",               {{"type","char"},     {"string","Short Code"},     {"required",true}}},
            {"type",               {{"type","selection"},{"string","Type"}}},
            {"currency_id",        {{"type","many2one"}, {"string","Currency"},       {"relation","res.currency"}}},
            {"default_account_id", {{"type","many2one"}, {"string","Default Account"},{"relation","account.account"}}},
            {"sequence",           {{"type","integer"},  {"string","Sequence"}}},
            {"active",             {{"type","boolean"},  {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// account.tax
// ================================================================
class AccountTaxListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.tax.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"amount\"/>"
               "<field name=\"amount_type\"/>"
               "<field name=\"type_tax_use\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",         {{"type","char"},      {"string","Tax Name"},  {"required",true}}},
            {"amount",       {{"type","float"},     {"string","Amount"}}},
            {"amount_type",  {{"type","selection"}, {"string","Computation"}}},
            {"type_tax_use", {{"type","selection"}, {"string","Tax Scope"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountTaxFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.tax.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"name\"/>"
               "<field name=\"amount\"/>"
               "<field name=\"amount_type\"/>"
               "<field name=\"type_tax_use\"/>"
               "<field name=\"price_include\"/>"
               "<field name=\"description\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",          {{"type","char"},     {"string","Tax Name"},        {"required",true}}},
            {"amount",        {{"type","float"},    {"string","Amount"}}},
            {"amount_type",   {{"type","selection"},{"string","Computation"}}},
            {"type_tax_use",  {{"type","selection"},{"string","Tax Scope"}}},
            {"price_include", {{"type","boolean"},  {"string","Price Included"}}},
            {"description",   {{"type","char"},     {"string","Label on Invoice"}}},
            {"active",        {{"type","boolean"},  {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// account.move
// ================================================================
class AccountMoveListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.move.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"date\"/>"
               "<field name=\"move_type\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"amount_total\"/>"
               "<field name=\"state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",         {{"type","char"},      {"string","Number"}}},
            {"date",         {{"type","date"},      {"string","Date"}}},
            {"move_type",    {{"type","selection"}, {"string","Type"}}},
            {"partner_id",   {{"type","many2one"},  {"string","Partner"},  {"relation","res.partner"}}},
            {"amount_total", {{"type","monetary"},  {"string","Total"}}},
            {"state",        {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountMoveFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.move.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"move_type\"/>"
               "<field name=\"date\"/>"
               "<field name=\"journal_id\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"ref\"/>"
               "<field name=\"narration\"/>"
               "<field name=\"state\" readonly=\"1\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"move_type",  {{"type","selection"}, {"string","Type"}}},
            {"date",       {{"type","date"},      {"string","Date"},    {"required",true}}},
            {"journal_id", {{"type","many2one"},  {"string","Journal"}, {"required",true}, {"relation","account.journal"}}},
            {"partner_id", {{"type","many2one"},  {"string","Partner"}, {"relation","res.partner"}}},
            {"ref",        {{"type","char"},      {"string","Reference"}}},
            {"narration",  {{"type","text"},      {"string","Notes"}}},
            {"state",      {{"type","selection"}, {"string","Status"},  {"readonly",true}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// account.move.line
// ================================================================
class AccountMoveLineListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.move.line.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"move_id\"/>"
               "<field name=\"account_id\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"name\"/>"
               "<field name=\"debit\"/>"
               "<field name=\"credit\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"move_id",    {{"type","many2one"}, {"string","Entry"},   {"relation","account.move"}}},
            {"account_id", {{"type","many2one"}, {"string","Account"}, {"relation","account.account"}}},
            {"partner_id", {{"type","many2one"}, {"string","Partner"}, {"relation","res.partner"}}},
            {"name",       {{"type","char"},     {"string","Label"}}},
            {"debit",      {{"type","monetary"}, {"string","Debit"}}},
            {"credit",     {{"type","monetary"}, {"string","Credit"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountMoveLineFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.move.line.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"move_id\"/>"
               "<field name=\"account_id\"/>"
               "<field name=\"date\"/>"
               "<field name=\"name\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"debit\"/>"
               "<field name=\"credit\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"move_id",    {{"type","many2one"}, {"string","Entry"},   {"required",true}, {"relation","account.move"}}},
            {"account_id", {{"type","many2one"}, {"string","Account"}, {"required",true}, {"relation","account.account"}}},
            {"date",       {{"type","date"},     {"string","Date"}}},
            {"name",       {{"type","char"},     {"string","Label"}}},
            {"partner_id", {{"type","many2one"}, {"string","Partner"}, {"relation","res.partner"}}},
            {"debit",      {{"type","monetary"}, {"string","Debit"}}},
            {"credit",     {{"type","monetary"}, {"string","Credit"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// account.payment
// ================================================================
class AccountPaymentListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.payment.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"date\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"amount\"/>"
               "<field name=\"payment_type\"/>"
               "<field name=\"state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",         {{"type","char"},      {"string","Reference"}}},
            {"date",         {{"type","date"},      {"string","Date"}}},
            {"partner_id",   {{"type","many2one"},  {"string","Partner"},  {"relation","res.partner"}}},
            {"amount",       {{"type","monetary"},  {"string","Amount"}}},
            {"payment_type", {{"type","selection"}, {"string","Type"}}},
            {"state",        {{"type","selection"}, {"string","Status"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountPaymentFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.payment.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"date\"/>"
               "<field name=\"payment_type\"/>"
               "<field name=\"partner_type\"/>"
               "<field name=\"journal_id\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"amount\"/>"
               "<field name=\"memo\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"date",         {{"type","date"},      {"string","Date"},         {"required",true}}},
            {"payment_type", {{"type","selection"}, {"string","Payment Type"}}},
            {"partner_type", {{"type","selection"}, {"string","Partner Type"}}},
            {"journal_id",   {{"type","many2one"},  {"string","Journal"},      {"required",true}, {"relation","account.journal"}}},
            {"partner_id",   {{"type","many2one"},  {"string","Partner"},      {"relation","res.partner"}}},
            {"amount",       {{"type","monetary"},  {"string","Amount"}}},
            {"memo",         {{"type","char"},      {"string","Memo"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// account.payment.term
// ================================================================
class AccountPaymentTermListView : public core::BaseView {
public:
    std::string viewName() const override { return "account.payment.term.list"; }
    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"active\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",   {{"type","char"},    {"string","Payment Term"}, {"required",true}}},
            {"active", {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

class AccountPaymentTermFormView : public core::BaseView {
public:
    std::string viewName() const override { return "account.payment.term.form"; }
    std::string arch() const override {
        return "<form>"
               "<field name=\"name\"/>"
               "<field name=\"note\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",   {{"type","char"},    {"string","Payment Term"}, {"required",true}}},
            {"note",   {{"type","text"},    {"string","Notes"}}},
            {"active", {{"type","boolean"}, {"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

} // namespace odoo::modules::account
