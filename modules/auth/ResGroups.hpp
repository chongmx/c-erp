#pragma once
#include "BaseModel.hpp"
#include "FieldRegistry.hpp"

namespace odoo::modules::auth {

// ================================================================
// ResGroups — res.groups
// ================================================================
/**
 * Security groups. Users belong to groups; groups grant access rights.
 * Many2many relationship with res.users via res_groups_users_rel table.
 *
 * Standard built-in groups (seeded in AuthModule::seed()):
 *   base.group_public        — id=1  anonymous / portal
 *   base.group_user          — id=2  internal user
 *   base.group_system        — id=3  system administrator
 */
class ResGroups : public core::BaseModel<ResGroups> {
public:
    ODOO_MODEL("res.groups", "res_groups")

    std::string name;
    std::string fullName;   // "Application / Group Name"
    bool        share = false;  // portal/share group flag

    explicit ResGroups(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseModel<ResGroups>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({ "name",      core::FieldType::Char,    "Name",       true });
        fieldRegistry_.add({ "full_name", core::FieldType::Char,    "Full Name" });
        fieldRegistry_.add({ "share",     core::FieldType::Boolean, "Share Group" });
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]      = name;
        j["full_name"] = fullName;
        j["share"]     = share;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")      && j["name"].is_string())
            name      = j["name"].get<std::string>();
        if (j.contains("full_name") && j["full_name"].is_string())
            fullName  = j["full_name"].get<std::string>();
        if (j.contains("share")     && j["share"].is_boolean())
            share     = j["share"].get<bool>();
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> errors;
        if (name.empty()) errors.push_back("Group name is required");
        return errors;
    }
};

} // namespace odoo::modules::auth
