#pragma once
#include "BaseView.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace odoo::modules::auth {

// ================================================================
// UsersFormView — res.users.form
// ================================================================
class UsersFormView : public core::BaseView {
public:
    std::string viewName() const override { return "res.users.form"; }

    std::string arch() const override {
        return "<form>"
               "<field name=\"login\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"company_id\"/>"
               "<field name=\"lang\"/>"
               "<field name=\"tz\"/>"
               "<field name=\"active\"/>"
               "</form>";
    }

    nlohmann::json fields() const override {
        return {
            {"login",      {{"type","char"},    {"string","Login"},    {"required",true}}},
            {"partner_id", {{"type","many2one"},{"string","Partner"},  {"required",true}, {"relation","res.partner"}}},
            {"company_id", {{"type","many2one"},{"string","Company"},  {"required",true}, {"relation","res.company"}}},
            {"lang",       {{"type","char"},    {"string","Language"}}},
            {"tz",         {{"type","char"},    {"string","Timezone"}}},
            {"active",     {{"type","boolean"}, {"string","Active"}}},
            {"share",      {{"type","boolean"}, {"string","Share User"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {
            {"arch",   arch()},
            {"fields", fields()},
            {"record", record},
        };
    }
};

// ================================================================
// CompanyFormView — res.company.form
// ================================================================
class CompanyFormView : public core::BaseView {
public:
    std::string viewName() const override { return "res.company.form"; }

    std::string arch() const override {
        return "<form>"
               "<field name=\"name\"/>"
               "<field name=\"email\"/>"
               "<field name=\"phone\"/>"
               "<field name=\"website\"/>"
               "<field name=\"vat\"/>"
               "<field name=\"parent_id\"/>"
               "</form>";
    }

    nlohmann::json fields() const override {
        return {
            {"name",      {{"type","char"},    {"string","Company Name"}, {"required",true}}},
            {"email",     {{"type","char"},    {"string","Email"}}},
            {"phone",     {{"type","char"},    {"string","Phone"}}},
            {"website",   {{"type","char"},    {"string","Website"}}},
            {"vat",       {{"type","char"},    {"string","Tax ID"}}},
            {"parent_id", {{"type","many2one"},{"string","Parent Company"}, {"relation","res.company"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// UsersListView — res.users.list
// ================================================================
class UsersListView : public core::BaseView {
public:
    std::string viewName() const override { return "res.users.list"; }

    std::string arch() const override {
        return "<list>"
               "<field name=\"login\"/>"
               "<field name=\"partner_id\"/>"
               "<field name=\"company_id\"/>"
               "<field name=\"active\"/>"
               "</list>";
    }

    nlohmann::json fields() const override {
        return {
            {"login",      {{"type","char"},    {"string","Login"}}},
            {"partner_id", {{"type","many2one"},{"string","Partner"},  {"relation","res.partner"}}},
            {"company_id", {{"type","many2one"},{"string","Company"},  {"relation","res.company"}}},
            {"active",     {{"type","boolean"}, {"string","Active"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// CompanyListView — res.company.list
// ================================================================
class CompanyListView : public core::BaseView {
public:
    std::string viewName() const override { return "res.company.list"; }

    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"email\"/>"
               "<field name=\"phone\"/>"
               "</list>";
    }

    nlohmann::json fields() const override {
        return {
            {"name",  {{"type","char"}, {"string","Company Name"}, {"required",true}}},
            {"email", {{"type","char"}, {"string","Email"}}},
            {"phone", {{"type","char"}, {"string","Phone"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

// ================================================================
// GroupsListView — res.groups.list
// ================================================================
class GroupsListView : public core::BaseView {
public:
    std::string viewName() const override { return "res.groups.list"; }

    std::string arch() const override {
        return "<list>"
               "<field name=\"name\"/>"
               "<field name=\"full_name\"/>"
               "<field name=\"share\"/>"
               "</list>";
    }

    nlohmann::json fields() const override {
        return {
            {"name",      {{"type","char"},    {"string","Name"}}},
            {"full_name", {{"type","char"},    {"string","Full Name"}}},
            {"share",     {{"type","boolean"}, {"string","Share Group"}}},
        };
    }

    nlohmann::json render(const nlohmann::json& record) const override {
        return {{"arch", arch()}, {"fields", fields()}, {"record", record}};
    }
};

} // namespace odoo::modules::auth
