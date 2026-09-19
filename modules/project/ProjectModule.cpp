// =============================================================
// modules/project/ProjectModule.cpp — docs/100
// =============================================================
#include "ProjectModule.hpp"
#include "ProjectMigrations.hpp"
#include <drogon/drogon.h>          // LOG_INFO
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "MailHelpers.hpp"
#include "MigrationRunner.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cerp::modules::project {

using namespace cerp::infrastructure;
using namespace cerp::core;

// ── helpers ─────────────────────────────────────────────────
static int m2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    if (v.is_string()) { try { return std::stoi(v.get<std::string>()); } catch (...) {} }
    return 0;
}
static std::string jstr(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string{};
}
/// An empty date must reach SQL as NULL, not as '' — Postgres rejects the
/// latter with "invalid input syntax for type date" (docs/096).
static nlohmann::json dateOrNull(const std::string& s) {
    return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s);
}
/// "{1,2,3}" for an `= ANY($1::int[])` parameter.
static std::string intArray(const std::vector<int>& ids) {
    std::string s = "{";
    for (size_t i = 0; i < ids.size(); ++i) { if (i) s += ","; s += std::to_string(ids[i]); }
    return s + "}";
}
static std::string upperTrim(std::string s) {
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    s = (b == std::string::npos) ? std::string{} : s.substr(b, e - b + 1);
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
/// A ticket-key prefix: a letter, then letters or digits, 2-10 in all — the
/// same rule as the CHECK constraint in migration 1100.
static bool validPrefix(const std::string& p) {
    if (p.size() < 2 || p.size() > 10) return false;
    if (p[0] < 'A' || p[0] > 'Z') return false;
    for (char c : p) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}
/// priority arrives as 1 or "1" (a selection value is a string on the wire).
static bool priorityFrom(const nlohmann::json& v, int& out) {
    if (v.is_number_integer()) { out = v.get<int>(); return true; }
    if (v.is_string()) { try { out = std::stoi(v.get<std::string>()); return true; } catch (...) {} }
    return false;
}

// The issue vocabulary — one definition, used by the models, the write guard
// and the history lines.
static const std::vector<std::pair<std::string, std::string>> kIssueTypes = {
    {"task", "Task"}, {"bug", "Bug"}, {"feature", "Feature"}, {"chore", "Chore"}};
static const std::vector<std::pair<std::string, std::string>> kPriorities = {
    {"-1", "Low"}, {"0", "Normal"}, {"1", "High"}, {"2", "Urgent"}};
static std::string labelOf(const std::vector<std::pair<std::string, std::string>>& v,
                           const std::string& key) {
    for (const auto& [k, l] : v) if (k == key) return l;
    return key;
}

// ================================================================
// 1. MODELS
// ================================================================

class Project : public BaseModel<Project> {
public:
    static constexpr const char* MODEL_NAME = "project.project";
    static constexpr const char* TABLE_NAME = "project_project";

    std::string name, code, description, dateStart, dateEnd, taskPrefix;
    int  partnerId = 0, userId = 0, companyId = 0, sequence = 10, color = 0;
    bool allowTimesheets = true, active = true;

    explicit Project(std::shared_ptr<DbConnection> db) : BaseModel<Project>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        FieldType::Char,     "Project", true});
        fieldRegistry_.add({"code",        FieldType::Char,     "Reference"});
        // The "CERP" in CERP-12. Left empty, the database derives one from the
        // name (migration 1100); renaming it re-keys the project's tickets.
        fieldRegistry_.add({"task_prefix", FieldType::Char,     "Ticket Key Prefix"});
        fieldRegistry_.add({"description", FieldType::Text,     "Description"});
        fieldRegistry_.add({"partner_id",  FieldType::Many2one, "Customer", false, false, true, false, "res.partner"});
        fieldRegistry_.add({"user_id",     FieldType::Many2one, "Manager",  false, false, true, false, "res.users"});
        fieldRegistry_.add({"company_id",  FieldType::Many2one, "Company",  false, false, true, false, "res.company"});
        fieldRegistry_.add({"date_start",  FieldType::Date,     "Start Date"});
        fieldRegistry_.add({"date_end",    FieldType::Date,     "End Date"});
        fieldRegistry_.add({"sequence",    FieldType::Integer,  "Sequence"});
        fieldRegistry_.add({"color",       FieldType::Integer,  "Colour"});
        fieldRegistry_.add({"allow_timesheets", FieldType::Boolean, "Allow Timesheets"});
        fieldRegistry_.add({"active",      FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;  j["code"] = code;  j["description"] = description;
        j["task_prefix"] = taskPrefix;
        j["partner_id"] = partnerId > 0 ? nlohmann::json(partnerId) : nlohmann::json(false);
        j["user_id"]    = userId    > 0 ? nlohmann::json(userId)    : nlohmann::json(false);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["date_start"] = dateOrNull(dateStart);
        j["date_end"]   = dateOrNull(dateEnd);
        j["sequence"] = sequence;  j["color"] = color;
        j["allow_timesheets"] = allowTimesheets;  j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))        name        = jstr(j, "name");
        if (j.contains("code"))        code        = jstr(j, "code");
        if (j.contains("task_prefix")) taskPrefix  = upperTrim(jstr(j, "task_prefix"));
        if (j.contains("description")) description = jstr(j, "description");
        if (j.contains("date_start"))  dateStart   = jstr(j, "date_start");
        if (j.contains("date_end"))    dateEnd     = jstr(j, "date_end");
        if (j.contains("partner_id"))  partnerId   = m2oId(j["partner_id"]);
        if (j.contains("user_id"))     userId      = m2oId(j["user_id"]);
        if (j.contains("company_id"))  companyId   = m2oId(j["company_id"]);
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (j.contains("color")    && j["color"].is_number())    color    = j["color"].get<int>();
        if (j.contains("allow_timesheets") && j["allow_timesheets"].is_boolean())
            allowTimesheets = j["allow_timesheets"].get<bool>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = code.empty() ? name : (code + " — " + name);
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("name is required");
        if (!dateStart.empty() && !dateEnd.empty() && dateEnd < dateStart)
            e.push_back("End Date cannot be before Start Date");
        if (!taskPrefix.empty() && !validPrefix(taskPrefix))
            e.push_back("Ticket Key Prefix must be 2-10 letters or digits, starting with a letter (e.g. CERP)");
        return e;
    }
};

class ProjectTaskType : public BaseModel<ProjectTaskType> {
public:
    static constexpr const char* MODEL_NAME = "project.task.type";
    static constexpr const char* TABLE_NAME = "project_task_type";

    std::string name;
    // project_id = 0 means the stage is shared by every project. Per-project
    // stages are the exception, not the rule, so a global default set works
    // out of the box and a project can still diverge.
    int  projectId = 0, sequence = 10;
    bool fold = false, isClosed = false, active = true;

    explicit ProjectTaskType(std::shared_ptr<DbConnection> db) : BaseModel<ProjectTaskType>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",       FieldType::Char,     "Stage", true});
        fieldRegistry_.add({"project_id", FieldType::Many2one, "Project", false, false, true, false, "project.project"});
        fieldRegistry_.add({"sequence",   FieldType::Integer,  "Sequence"});
        fieldRegistry_.add({"fold",       FieldType::Boolean,  "Folded"});
        fieldRegistry_.add({"is_closed",  FieldType::Boolean,  "Closing Stage"});
        fieldRegistry_.add({"active",     FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;
        j["project_id"] = projectId > 0 ? nlohmann::json(projectId) : nlohmann::json(false);
        j["sequence"] = sequence;  j["fold"] = fold;
        j["is_closed"] = isClosed; j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))       name      = jstr(j, "name");
        if (j.contains("project_id")) projectId = m2oId(j["project_id"]);
        if (j.contains("sequence")  && j["sequence"].is_number())   sequence = j["sequence"].get<int>();
        if (j.contains("fold")      && j["fold"].is_boolean())      fold     = j["fold"].get<bool>();
        if (j.contains("is_closed") && j["is_closed"].is_boolean()) isClosed = j["is_closed"].get<bool>();
        if (j.contains("active")    && j["active"].is_boolean())    active   = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("name is required");
        return e;
    }
};

class ProjectTask : public BaseModel<ProjectTask> {
public:
    static constexpr const char* MODEL_NAME = "project.task";
    static constexpr const char* TABLE_NAME = "project_task";

    std::string name, description, dateDeadline, dateEnd, kanbanState = "normal",
                issueType = "task";
    int    projectId = 0, stageId = 0, userId = 0, partnerId = 0, parentId = 0,
           companyId = 0, sequence = 10, priority = 0, reporterId = 0;
    double plannedHours = 0.0;
    bool   active = true;

    explicit ProjectTask(std::shared_ptr<DbConnection> db) : BaseModel<ProjectTask>(std::move(db)) {}

    void registerFields() override {
        // key / number / display_name are written ONLY by the trigger in
        // migration 1100, so they are registered (readable, filterable,
        // sortable) but never serialized for an INSERT, and the view model
        // strips them from a client's write.
        fieldRegistry_.add({"key",           FieldType::Char,     "Key"});
        fieldRegistry_.add({"number",        FieldType::Integer,  "Number"});
        fieldRegistry_.add({"display_name",  FieldType::Char,     "Display Name"});
        fieldRegistry_.add({"name",          FieldType::Char,     "Task", true});
        {
            core::FieldDef it{"issue_type", FieldType::Selection, "Type"};
            it.selection = kIssueTypes;
            fieldRegistry_.add(it);
        }
        fieldRegistry_.add({"reporter_id",   FieldType::Many2one, "Reporter", false, false, true, false, "res.users"});
        fieldRegistry_.add({"description",   FieldType::Text,     "Description"});
        fieldRegistry_.add({"project_id",    FieldType::Many2one, "Project", true,  false, true, false, "project.project"});
        fieldRegistry_.add({"stage_id",      FieldType::Many2one, "Stage",   false, false, true, false, "project.task.type"});
        fieldRegistry_.add({"user_id",       FieldType::Many2one, "Assigned To", false, false, true, false, "res.users"});
        fieldRegistry_.add({"partner_id",    FieldType::Many2one, "Customer",    false, false, true, false, "res.partner"});
        fieldRegistry_.add({"parent_id",     FieldType::Many2one, "Parent Task", false, false, true, false, "project.task"});
        fieldRegistry_.add({"company_id",    FieldType::Many2one, "Company",     false, false, true, false, "res.company"});
        fieldRegistry_.add({"date_deadline", FieldType::Date,     "Deadline"});
        fieldRegistry_.add({"date_end",      FieldType::Date,     "Closed On"});
        fieldRegistry_.add({"kanban_state",  FieldType::Char,     "Kanban State"});
        fieldRegistry_.add({"sequence",      FieldType::Integer,  "Sequence"});
        {
            // Stored as an integer (-1..2) so sorting by it is sorting by
            // urgency; a selection so lists and filters show the word.
            core::FieldDef pr{"priority", FieldType::Selection, "Priority"};
            pr.selection = kPriorities;
            fieldRegistry_.add(pr);
        }
        fieldRegistry_.add({"planned_hours", FieldType::Float,    "Planned Hours"});
        fieldRegistry_.add({"active",        FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;  j["description"] = description;
        j["issue_type"]  = issueType.empty() ? "task" : issueType;
        j["reporter_id"] = reporterId > 0 ? nlohmann::json(reporterId) : nlohmann::json(false);
        j["project_id"] = projectId > 0 ? nlohmann::json(projectId) : nlohmann::json(false);
        j["stage_id"]   = stageId   > 0 ? nlohmann::json(stageId)   : nlohmann::json(false);
        j["user_id"]    = userId    > 0 ? nlohmann::json(userId)    : nlohmann::json(false);
        j["partner_id"] = partnerId > 0 ? nlohmann::json(partnerId) : nlohmann::json(false);
        j["parent_id"]  = parentId  > 0 ? nlohmann::json(parentId)  : nlohmann::json(false);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["date_deadline"] = dateOrNull(dateDeadline);
        j["date_end"]      = dateOrNull(dateEnd);
        j["kanban_state"] = kanbanState.empty() ? "normal" : kanbanState;
        j["sequence"] = sequence;  j["priority"] = priority;
        j["planned_hours"] = plannedHours;  j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))          name         = jstr(j, "name");
        if (j.contains("description"))   description  = jstr(j, "description");
        if (j.contains("date_deadline")) dateDeadline = jstr(j, "date_deadline");
        if (j.contains("date_end"))      dateEnd      = jstr(j, "date_end");
        if (j.contains("kanban_state"))  kanbanState  = jstr(j, "kanban_state");
        if (j.contains("project_id"))    projectId    = m2oId(j["project_id"]);
        if (j.contains("stage_id"))      stageId      = m2oId(j["stage_id"]);
        if (j.contains("user_id"))       userId       = m2oId(j["user_id"]);
        if (j.contains("partner_id"))    partnerId    = m2oId(j["partner_id"]);
        if (j.contains("parent_id"))     parentId     = m2oId(j["parent_id"]);
        if (j.contains("company_id"))    companyId    = m2oId(j["company_id"]);
        if (j.contains("sequence") && j["sequence"].is_number()) sequence = j["sequence"].get<int>();
        if (j.contains("priority")) priorityFrom(j["priority"], priority);
        if (j.contains("issue_type"))  issueType  = jstr(j, "issue_type");
        if (j.contains("reporter_id")) reporterId = m2oId(j["reporter_id"]);
        if (j.contains("planned_hours") && j["planned_hours"].is_number())
            plannedHours = j["planned_hours"].get<double>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())     e.push_back("name is required");
        if (projectId <= 0)   e.push_back("project_id is required");
        if (plannedHours < 0) e.push_back("Planned Hours cannot be negative");
        if (priority < -1 || priority > 2)
            e.push_back("priority must be -1 (Low), 0 (Normal), 1 (High) or 2 (Urgent)");
        if (!issueType.empty() && labelOf(kIssueTypes, issueType) == issueType)
            e.push_back("Type must be task, bug, feature or chore");
        static const std::set<std::string> kStates = {"normal", "done", "blocked"};
        if (!kanbanState.empty() && !kStates.count(kanbanState))
            e.push_back("kanban_state must be normal, done or blocked");
        return e;
    }
};

class ProjectTimesheet : public BaseModel<ProjectTimesheet> {
public:
    static constexpr const char* MODEL_NAME = "project.timesheet";
    static constexpr const char* TABLE_NAME = "project_timesheet";

    std::string name, date;
    int    projectId = 0, taskId = 0, employeeId = 0, userId = 0, companyId = 0;
    double unitAmount = 0.0;   // hours

    explicit ProjectTimesheet(std::shared_ptr<DbConnection> db) : BaseModel<ProjectTimesheet>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        FieldType::Char,     "Description"});
        fieldRegistry_.add({"date",        FieldType::Date,     "Date", true});
        fieldRegistry_.add({"project_id",  FieldType::Many2one, "Project", true,  false, true, false, "project.project"});
        fieldRegistry_.add({"task_id",     FieldType::Many2one, "Task",    false, false, true, false, "project.task"});
        fieldRegistry_.add({"employee_id", FieldType::Many2one, "Employee",false, false, true, false, "hr.employee"});
        fieldRegistry_.add({"user_id",     FieldType::Many2one, "User",    false, false, true, false, "res.users"});
        fieldRegistry_.add({"company_id",  FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"unit_amount", FieldType::Float,    "Hours"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;
        j["date"] = dateOrNull(date);
        j["project_id"]  = projectId  > 0 ? nlohmann::json(projectId)  : nlohmann::json(false);
        j["task_id"]     = taskId     > 0 ? nlohmann::json(taskId)     : nlohmann::json(false);
        j["employee_id"] = employeeId > 0 ? nlohmann::json(employeeId) : nlohmann::json(false);
        j["user_id"]     = userId     > 0 ? nlohmann::json(userId)     : nlohmann::json(false);
        j["company_id"]  = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["unit_amount"] = unitAmount;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")) name = jstr(j, "name");
        if (j.contains("date")) date = jstr(j, "date");
        if (j.contains("project_id"))  projectId  = m2oId(j["project_id"]);
        if (j.contains("task_id"))     taskId     = m2oId(j["task_id"]);
        if (j.contains("employee_id")) employeeId = m2oId(j["employee_id"]);
        if (j.contains("user_id"))     userId     = m2oId(j["user_id"]);
        if (j.contains("company_id"))  companyId  = m2oId(j["company_id"]);
        if (j.contains("unit_amount") && j["unit_amount"].is_number())
            unitAmount = j["unit_amount"].get<double>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = name.empty() ? (date + " (" + std::to_string(unitAmount) + "h)") : name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (date.empty())    e.push_back("date is required");
        if (projectId <= 0)  e.push_back("project_id is required");
        // A negative entry is always a mistake, and 24h in one day on one task
        // is almost always a slipped decimal point. Both are worth blocking at
        // the boundary rather than discovering in a payroll report.
        if (unitAmount < 0)  e.push_back("Hours cannot be negative");
        if (unitAmount > 24) e.push_back("A single entry cannot exceed 24 hours");
        return e;
    }
};

/// A label on a ticket ("ui", "billing", "regression"). Shared by every
/// project; names are unique ignoring case (migration 1101).
class ProjectTag : public BaseModel<ProjectTag> {
public:
    static constexpr const char* MODEL_NAME = "project.tag";
    static constexpr const char* TABLE_NAME = "project_tag";

    std::string name;
    int  color = 0;
    bool active = true;

    explicit ProjectTag(std::shared_ptr<DbConnection> db) : BaseModel<ProjectTag>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",   FieldType::Char,    "Label", true});
        fieldRegistry_.add({"color",  FieldType::Integer, "Colour"});
        fieldRegistry_.add({"active", FieldType::Boolean, "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name; j["color"] = color; j["active"] = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name"))   name = jstr(j, "name");
        if (j.contains("color")  && j["color"].is_number())   color  = j["color"].get<int>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())      e.push_back("A label needs a name");
        if (name.size() > 40)  e.push_back("A label is at most 40 characters");
        return e;
    }
};

// ================================================================
// 2. VIEWMODELS
// ================================================================

class ProjectViewModel : public GenericViewModel<Project> {
public:
    explicit ProjectViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<Project>(db), db_(std::move(db)) {
        REGISTER_METHOD("stats", handleStats)
        REGISTER_MUTATOR("create", handleProjectCreate)
        REGISTER_MUTATOR("write",  handleProjectWrite)
    }
private:
    std::shared_ptr<DbConnection> db_;

    // The prefix is checked here, not left to the database: the unique index
    // and the CHECK would both surface as an internal error, and "CERP is
    // already used by c-erp" is something a user can act on.
    void checkPrefix_(const nlohmann::json& v, const std::vector<int>& selfIds) {
        if (!v.is_object() || !v.contains("task_prefix") || !v["task_prefix"].is_string()) return;
        const std::string p = upperTrim(v["task_prefix"].get<std::string>());
        if (p.empty()) return;   // the database derives one
        if (!validPrefix(p))
            throw ValidationError("Ticket Key Prefix must be 2-10 letters or digits, "
                                  "starting with a letter (e.g. CERP).");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT name FROM project_project "
                          "WHERE upper(task_prefix) = $1 AND NOT (id = ANY($2::int[]))",
                          pqxx::params{p, intArray(selfIds)});
        if (!r.empty())
            throw ValidationError("The ticket key prefix " + p + " is already used by project \"" +
                                  std::string(r[0][0].c_str()) + "\".");
    }
    nlohmann::json handleProjectCreate(const core::CallKwArgs& call) {
        checkPrefix_(call.arg(0), {});
        return handleCreate(call);
    }
    nlohmann::json handleProjectWrite(const core::CallKwArgs& call) {
        checkPrefix_(call.arg(1), call.ids());
        return handleWrite(call);
    }

    /// Per-project rollup for the board header: open/closed counts and the
    /// planned-vs-logged hours that tell you whether an estimate held.
    nlohmann::json handleStats(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int pid = (v.is_object() && v.contains("project_id")) ? m2oId(v["project_id"]) : 0;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::string where = "WHERE t.active";
        pqxx::params p;
        if (pid > 0) { where += " AND t.project_id=$1"; p.append(pid); }

        const std::string sql =
            "SELECT count(*) AS total, "
            "       count(*) FILTER (WHERE COALESCE(s.is_closed,false)) AS closed, "
            "       count(*) FILTER (WHERE t.kanban_state='blocked') AS blocked, "
            "       COALESCE(sum(t.planned_hours),0) AS planned "
            "FROM project_task t LEFT JOIN project_task_type s ON s.id=t.stage_id " + where;
        auto r = pid > 0 ? txn.exec(sql, p) : txn.exec(sql);

        const std::string tsSql =
            "SELECT COALESCE(sum(unit_amount),0) FROM project_timesheet ts "
            + std::string(pid > 0 ? "WHERE ts.project_id=$1" : "");
        auto ts = pid > 0 ? txn.exec(tsSql, p) : txn.exec(tsSql);

        const long total  = r[0]["total"].as<long>(0);
        const long closed = r[0]["closed"].as<long>(0);
        return {{"total", total}, {"closed", closed}, {"open", total - closed},
                {"blocked", r[0]["blocked"].as<long>(0)},
                {"planned_hours", r[0]["planned"].as<double>(0.0)},
                {"logged_hours",  ts[0][0].as<double>(0.0)}};
    }
};

class ProjectTaskViewModel : public GenericViewModel<ProjectTask> {
public:
    explicit ProjectTaskViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<ProjectTask>(db), db_(std::move(db)) {
        REGISTER_METHOD("board",        handleBoard)
        REGISTER_MUTATOR("move_stage",   handleMoveStage)
        // The ticket (issue tracker, migrations 1100-1101).
        REGISTER_MUTATOR("create",         handleTaskCreate)
        REGISTER_MUTATOR("write",          handleTaskWrite)
        REGISTER_MUTATOR("unlink",         handleTaskUnlink)
        REGISTER_METHOD("task_detail",     handleTaskDetail)
        REGISTER_METHOD("activity",        handleActivity)
        REGISTER_MUTATOR("post_comment",   handlePostComment)
        REGISTER_MUTATOR("edit_comment",   handleEditComment)
        REGISTER_MUTATOR("delete_comment", handleDeleteComment)
        REGISTER_MUTATOR("set_tags",       handleSetTags)
        REGISTER_MUTATOR("set_watchers",   handleSetWatchers)
        REGISTER_MUTATOR("watch",          handleWatch)
    }
private:
    std::shared_ptr<DbConnection> db_;

    static constexpr const char* kModel = "project.task";
    static constexpr size_t kMaxComment = 20000;

    // A person as shown on a ticket: their contact's name, else their login.
    static constexpr const char* kUserName =
        "COALESCE(NULLIF(%p.name,''), %u.login, '')";
    static std::string userName(const std::string& u, const std::string& p) {
        std::string s = kUserName;
        for (auto pos = s.find("%p"); pos != std::string::npos; pos = s.find("%p")) s.replace(pos, 2, p);
        for (auto pos = s.find("%u"); pos != std::string::npos; pos = s.find("%u")) s.replace(pos, 2, u);
        return s;
    }

    /// Only the trigger writes these (migration 1100). Dropped rather than
    /// refused so a client that round-trips a whole record is not punished.
    static void stripComputed_(nlohmann::json& v) {
        if (!v.is_object()) return;
        v.erase("key"); v.erase("number"); v.erase("display_name");
    }

    /// The ticket as this user may see it — through the ORM, so record rules
    /// and the company boundary (docs/094) apply exactly as they do to read.
    /// Every raw-SQL method below starts here.
    void requireTask_(const core::CallKwArgs& call, int taskId) {
        if (taskId <= 0) throw ValidationError("task_id is required.");
        ProjectTask proto(db_);
        proto.setUserContext(extractContext_(call));
        const auto r = proto.read({taskId}, {"id"});
        if (!r.is_array() || r.empty()) throw ValidationError("No such task.");
    }

    static void addWatcher_(pqxx::work& txn, int taskId, int userId) {
        if (taskId <= 0 || userId <= 0) return;
        txn.exec("INSERT INTO project_task_watcher_rel (task_id, user_id) VALUES ($1,$2) "
                 "ON CONFLICT DO NOTHING", pqxx::params{taskId, userId});
    }
    static void touch_(pqxx::work& txn, int taskId) {
        txn.exec("UPDATE project_task SET write_date = now() WHERE id = $1", pqxx::params{taskId});
    }

    // ---- history ----------------------------------------------------------
    // What a person reads in the activity feed: "Status: New → In Progress".
    // Values are captured as LABELS (the stage's name, the assignee's name),
    // not ids, so the history still reads correctly after the stage is renamed
    // or the user leaves.
    struct Tracked { const char* col; const char* label; };
    static const std::vector<Tracked>& tracked_() {
        static const std::vector<Tracked> t = {
            {"name", "Title"}, {"stage", "Status"}, {"assignee", "Assignee"},
            {"reporter", "Reporter"}, {"issue_type", "Type"}, {"priority", "Priority"},
            {"blocked", "Blocked"}, {"project", "Project"}, {"parent", "Parent"},
            {"deadline", "Due date"}, {"estimate", "Estimate (h)"},
            {"descr", "Description"}, {"active", "Archived"}};
        return t;
    }
    using Snap = std::map<int, std::map<std::string, std::string>>;
    Snap snapshot_(pqxx::work& txn, const std::vector<int>& ids) {
        Snap out;
        if (ids.empty()) return out;
        auto r = txn.exec(
            "SELECT t.id, t.name, COALESCE(s.name,'') AS stage, "
            "       " + userName("u", "up") + " AS assignee, "
            "       " + userName("r", "rp") + " AS reporter, "
            "       COALESCE(t.issue_type,'task') AS issue_type, t.priority::text AS priority, "
            "       CASE WHEN t.kanban_state = 'blocked' THEN 'yes' ELSE 'no' END AS blocked, "
            "       COALESCE(p.name,'') AS project, "
            "       COALESCE(par.key || ' ' || par.name, '') AS parent, "
            "       COALESCE(to_char(t.date_deadline,'YYYY-MM-DD'),'') AS deadline, "
            "       COALESCE(t.planned_hours,0)::float8::text AS estimate, "
            "       md5(COALESCE(t.description,'')) AS descr, "
            "       CASE WHEN t.active THEN 'no' ELSE 'yes' END AS active "
            "FROM project_task t "
            "LEFT JOIN project_task_type s ON s.id = t.stage_id "
            "LEFT JOIN res_users u   ON u.id = t.user_id     LEFT JOIN res_partner up ON up.id = u.partner_id "
            "LEFT JOIN res_users r   ON r.id = t.reporter_id LEFT JOIN res_partner rp ON rp.id = r.partner_id "
            "LEFT JOIN project_project p ON p.id = t.project_id "
            "LEFT JOIN project_task par  ON par.id = t.parent_id "
            "WHERE t.id = ANY($1::int[])", pqxx::params{intArray(ids)});
        for (const auto& row : r) {
            auto& m = out[row["id"].as<int>()];
            for (const auto& t : tracked_())
                m[t.col] = row[t.col].is_null() ? std::string{} : std::string(row[t.col].c_str());
            m["issue_type"] = labelOf(kIssueTypes, m["issue_type"]);
            m["priority"]   = labelOf(kPriorities, m["priority"]);
        }
        return out;
    }
    /// One history entry per change, one line per field that changed.
    static std::string diff_(const std::map<std::string, std::string>& a,
                             const std::map<std::string, std::string>& b) {
        std::string body;
        for (const auto& t : tracked_()) {
            const auto ia = a.find(t.col), ib = b.find(t.col);
            const std::string va = ia == a.end() ? "" : ia->second;
            const std::string vb = ib == b.end() ? "" : ib->second;
            if (va == vb) continue;
            if (!body.empty()) body += "\n";
            if (std::string(t.col) == "descr") { body += "Description updated"; continue; }
            body += std::string(t.label) + ": " + (va.empty() ? "—" : va) + " → " + (vb.empty() ? "—" : vb);
        }
        return body;
    }
    static void track_(pqxx::work& txn, int taskId, int uid, const std::string& body) {
        if (!body.empty()) mail::postLog(txn, kModel, taskId, uid, body, "tracking");
    }

    /// Labels on a ticket as "a, b" (for the history line).
    static std::string tagList_(pqxx::work& txn, int taskId) {
        auto r = txn.exec("SELECT COALESCE(string_agg(g.name, ', ' ORDER BY lower(g.name)), '') "
                          "FROM project_task_tag_rel x JOIN project_tag g ON g.id = x.tag_id "
                          "WHERE x.task_id = $1", pqxx::params{taskId});
        return r[0][0].c_str();
    }
    static std::vector<int> intsOf_(const nlohmann::json& a) {
        std::vector<int> out;
        if (!a.is_array()) return out;
        for (const auto& e : a) { const int id = m2oId(e); if (id > 0) out.push_back(id); }
        return out;
    }

    // ---- create / write / unlink ----------------------------------------
    nlohmann::json handleTaskCreate(const core::CallKwArgs& call) {
        auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("create: args[0] must be a dict");
        stripComputed_(v);
        const auto ctx = extractContext_(call);
        // The person who filed it, unless they say otherwise.
        if ((!v.contains("reporter_id") || m2oId(v["reporter_id"]) <= 0) && ctx.uid > 0)
            v["reporter_id"] = ctx.uid;
        // A ticket with no stage is on no column of the board — it exists and
        // cannot be seen. Put it in the project's first open stage.
        const int pid = v.contains("project_id") ? m2oId(v["project_id"]) : 0;
        if (pid > 0 && (!v.contains("stage_id") || m2oId(v["stage_id"]) <= 0)) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            auto s = txn.exec("SELECT id FROM project_task_type "
                              "WHERE active AND NOT COALESCE(is_closed,false) "
                              "  AND (project_id IS NULL OR project_id = $1) "
                              "ORDER BY sequence, id LIMIT 1", pqxx::params{pid});
            if (!s.empty()) v["stage_id"] = s[0][0].as<int>();
        }
        core::CallKwArgs c2 = call;
        c2.args[0] = v;
        const auto res = handleCreate(c2);
        const int id = res.is_number_integer() ? res.get<int>() : 0;
        if (id > 0) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            addWatcher_(txn, id, m2oId(v["reporter_id"]));
            if (v.contains("user_id")) addWatcher_(txn, id, m2oId(v["user_id"]));
            track_(txn, id, ctx.uid, "Created this task");
            txn.commit();
        }
        return res;
    }

    nlohmann::json handleTaskWrite(const core::CallKwArgs& call) {
        auto v = call.arg(1);
        if (!v.is_object()) throw ValidationError("write: args[1] must be a dict");
        stripComputed_(v);
        // write() does not run validate(), and a CHECK violation would reach
        // the user as an internal error — so the vocabulary is checked here.
        if (v.contains("issue_type")) {
            const std::string t = jstr(v, "issue_type");
            if (labelOf(kIssueTypes, t) == t)
                throw ValidationError("Type must be task, bug, feature or chore.");
        }
        if (v.contains("priority")) {
            int p = 0;
            if (!priorityFrom(v["priority"], p) || p < -1 || p > 2)
                throw ValidationError("Priority must be Low, Normal, High or Urgent (-1..2).");
            v["priority"] = p;
        }
        const auto ctx = extractContext_(call);
        const auto ids = call.ids();

        Snap before;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            before = snapshot_(txn, ids);
            txn.commit();
        }
        core::CallKwArgs c2 = call;
        c2.args[1] = v;
        const auto res = handleWrite(c2);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        // A stage change made from the form closes and reopens exactly as a
        // drag on the board does (handleMoveStage).
        if (v.contains("stage_id"))
            txn.exec("UPDATE project_task t SET date_end = CASE "
                     "  WHEN COALESCE((SELECT is_closed FROM project_task_type s "
                     "                  WHERE s.id = t.stage_id), false) "
                     "  THEN COALESCE(t.date_end, CURRENT_DATE) ELSE NULL END "
                     "WHERE t.id = ANY($1::int[])", pqxx::params{intArray(ids)});
        const int assignee = v.contains("user_id") ? m2oId(v["user_id"]) : 0;
        const Snap after = snapshot_(txn, ids);
        for (const int id : ids) {
            const auto a = before.find(id);
            const auto b = after.find(id);
            if (a == before.end() || b == after.end()) continue;
            track_(txn, id, ctx.uid, diff_(a->second, b->second));
            addWatcher_(txn, id, assignee);
        }
        txn.commit();
        return res;
    }

    /// Comments and history are keyed by (model, id) with no foreign key, so
    /// they would outlive the ticket; they go with it.
    nlohmann::json handleTaskUnlink(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        const auto res = handleUnlink(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("DELETE FROM mail_message WHERE res_model = $1 AND res_id = ANY($2::int[])",
                 pqxx::params{std::string(kModel), intArray(ids)});
        txn.commit();
        return res;
    }

    // ---- the ticket screen -------------------------------------------------
    nlohmann::json handleTaskDetail(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int id = v.is_object() && v.contains("id") ? m2oId(v["id"]) : m2oId(v);
        requireTask_(call, id);
        const auto ctx = extractContext_(call);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec(
            "SELECT t.id, t.key, t.number, t.name, COALESCE(t.description,'') AS description, "
            "       COALESCE(t.issue_type,'task') AS issue_type, t.priority, "
            "       COALESCE(t.kanban_state,'normal') AS kanban_state, t.active, "
            "       t.project_id, COALESCE(p.name,'') AS project_name, "
            "       COALESCE(p.task_prefix,'') AS project_prefix, "
            "       t.stage_id, COALESCE(s.is_closed,false) AS closed, "
            "       t.user_id, " + userName("u", "up") + " AS user_name, "
            "       t.reporter_id, " + userName("r", "rp") + " AS reporter_name, "
            "       t.parent_id, COALESCE(par.key,'') AS parent_key, COALESCE(par.name,'') AS parent_name, "
            "       t.partner_id, COALESCE(cp.display_name, cp.name, '') AS partner_name, "
            "       COALESCE(to_char(t.date_deadline,'YYYY-MM-DD'),'') AS date_deadline, "
            "       COALESCE(to_char(t.date_end,'YYYY-MM-DD'),'') AS date_end, "
            "       COALESCE(t.planned_hours,0)::float8 AS planned_hours, "
            "       COALESCE((SELECT sum(ts.unit_amount) FROM project_timesheet ts "
            "                  WHERE ts.task_id = t.id),0)::float8 AS logged_hours, "
            "       to_char(t.create_date,'YYYY-MM-DD\"T\"HH24:MI:SS') AS create_date, "
            "       to_char(t.write_date, 'YYYY-MM-DD\"T\"HH24:MI:SS') AS write_date "
            "FROM project_task t "
            "LEFT JOIN project_project p ON p.id = t.project_id "
            "LEFT JOIN project_task_type s ON s.id = t.stage_id "
            "LEFT JOIN res_users u ON u.id = t.user_id     LEFT JOIN res_partner up ON up.id = u.partner_id "
            "LEFT JOIN res_users r ON r.id = t.reporter_id LEFT JOIN res_partner rp ON rp.id = r.partner_id "
            "LEFT JOIN project_task par ON par.id = t.parent_id "
            "LEFT JOIN res_partner cp ON cp.id = t.partner_id "
            "WHERE t.id = $1", pqxx::params{id});
        if (r.empty()) throw ValidationError("No such task.");
        const auto& row = r[0];
        auto idOr0 = [](const pqxx::field& f) { return f.is_null() ? 0 : f.as<int>(0); };
        const int pid = idOr0(row["project_id"]);

        nlohmann::json j = {
            {"id", id}, {"key", row["key"].is_null() ? "" : row["key"].c_str()},
            {"number", idOr0(row["number"])},
            {"name", row["name"].c_str()}, {"description", row["description"].c_str()},
            {"issue_type", row["issue_type"].c_str()}, {"priority", row["priority"].as<int>(0)},
            {"kanban_state", row["kanban_state"].c_str()}, {"active", row["active"].as<bool>(true)},
            {"project_id", pid}, {"project_name", row["project_name"].c_str()},
            {"project_prefix", row["project_prefix"].c_str()},
            {"stage_id", idOr0(row["stage_id"])}, {"closed", row["closed"].as<bool>(false)},
            {"user_id", idOr0(row["user_id"])}, {"user_name", row["user_name"].c_str()},
            {"reporter_id", idOr0(row["reporter_id"])}, {"reporter_name", row["reporter_name"].c_str()},
            {"parent_id", idOr0(row["parent_id"])}, {"parent_key", row["parent_key"].c_str()},
            {"parent_name", row["parent_name"].c_str()},
            {"partner_id", idOr0(row["partner_id"])}, {"partner_name", row["partner_name"].c_str()},
            {"date_deadline", row["date_deadline"].c_str()}, {"date_end", row["date_end"].c_str()},
            {"planned_hours", row["planned_hours"].as<double>(0.0)},
            {"logged_hours", row["logged_hours"].as<double>(0.0)},
            {"create_date", row["create_date"].is_null() ? "" : row["create_date"].c_str()},
            {"write_date", row["write_date"].is_null() ? "" : row["write_date"].c_str()}};

        // The status choices are this project's board columns.
        nlohmann::json stages = nlohmann::json::array();
        for (const auto& s : txn.exec(
                "SELECT id, name, COALESCE(is_closed,false) AS closed FROM project_task_type "
                "WHERE active AND (project_id IS NULL OR project_id = $1) ORDER BY sequence, id",
                pqxx::params{pid}))
            stages.push_back({{"id", s["id"].as<int>()}, {"name", s["name"].c_str()},
                              {"closed", s["closed"].as<bool>(false)}});
        j["stages"] = stages;

        nlohmann::json tags = nlohmann::json::array();
        for (const auto& g : txn.exec(
                "SELECT g.id, g.name, g.color FROM project_task_tag_rel x "
                "JOIN project_tag g ON g.id = x.tag_id WHERE x.task_id = $1 ORDER BY lower(g.name)",
                pqxx::params{id}))
            tags.push_back({{"id", g["id"].as<int>()}, {"name", g["name"].c_str()},
                            {"color", g["color"].as<int>(0)}});
        j["tags"] = tags;

        nlohmann::json watchers = nlohmann::json::array();
        bool watching = false;
        for (const auto& w : txn.exec(
                "SELECT u.id, " + userName("u", "up") + " AS name, u.login "
                "FROM project_task_watcher_rel x JOIN res_users u ON u.id = x.user_id "
                "LEFT JOIN res_partner up ON up.id = u.partner_id "
                "WHERE x.task_id = $1 ORDER BY 2", pqxx::params{id})) {
            const int wid = w["id"].as<int>();
            if (wid == ctx.uid) watching = true;
            watchers.push_back({{"id", wid}, {"name", w["name"].c_str()}, {"login", w["login"].c_str()}});
        }
        j["watchers"] = watchers;
        j["watching"] = watching;

        nlohmann::json subtasks = nlohmann::json::array();
        for (const auto& s : txn.exec(
                "SELECT t.id, t.key, t.name, COALESCE(st.name,'') AS stage, "
                "       COALESCE(st.is_closed,false) AS closed, COALESCE(t.issue_type,'task') AS issue_type "
                "FROM project_task t LEFT JOIN project_task_type st ON st.id = t.stage_id "
                "WHERE t.parent_id = $1 AND t.active ORDER BY t.number, t.id", pqxx::params{id}))
            subtasks.push_back({{"id", s["id"].as<int>()}, {"key", s["key"].is_null() ? "" : s["key"].c_str()},
                                {"name", s["name"].c_str()}, {"stage", s["stage"].c_str()},
                                {"closed", s["closed"].as<bool>(false)},
                                {"issue_type", s["issue_type"].c_str()}});
        j["subtasks"] = subtasks;

        auto cnt = txn.exec(
            "SELECT (SELECT count(*) FROM ir_attachment WHERE res_model = $1 AND res_id = $2) AS files, "
            "       (SELECT count(*) FROM mail_message  WHERE res_model = $1 AND res_id = $2 "
            "                                             AND subtype = 'comment') AS comments",
            pqxx::params{std::string(kModel), id});
        j["attachment_count"] = cnt[0]["files"].as<long>(0);
        j["comment_count"]    = cnt[0]["comments"].as<long>(0);
        return j;
    }

    /// Comments and history, oldest first — the order a ticket is read in.
    nlohmann::json handleActivity(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int id = v.is_object() && v.contains("task_id") ? m2oId(v["task_id"]) : m2oId(v);
        requireTask_(call, id);
        const auto ctx = extractContext_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json out = nlohmann::json::array();
        for (const auto& m : txn.exec(
                "SELECT m.id, m.subtype, m.body, COALESCE(m.author_id, 0) AS author_id, "
                "       COALESCE(NULLIF(up.name,''), u.login, 'System') AS author_name, "
                "       to_char(m.date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS date, "
                "       (m.write_date IS NOT NULL) AS edited "
                "FROM mail_message m "
                "LEFT JOIN res_users u ON u.id = m.author_id "
                "LEFT JOIN res_partner up ON up.id = u.partner_id "
                "WHERE m.res_model = $1 AND m.res_id = $2 ORDER BY m.date, m.id LIMIT 1000",
                pqxx::params{std::string(kModel), id})) {
            const int author = m["author_id"].as<int>(0);
            const std::string sub = m["subtype"].c_str();
            out.push_back({{"id", m["id"].as<int>()}, {"subtype", sub},
                           {"body", m["body"].c_str()}, {"author_id", author},
                           {"author_name", m["author_name"].c_str()}, {"date", m["date"].c_str()},
                           {"edited", m["edited"].as<bool>(false)},
                           {"can_edit", sub == "comment" && (author == ctx.uid || ctx.isAdmin)}});
        }
        return out;
    }

    // ---- comments ------------------------------------------------------------
    // Written here, not through mail.message create, because the author is
    // the SESSION's user. mail.message create takes author_id from the client,
    // which would let anyone comment as anyone.
    static std::string commentBody_(const nlohmann::json& v) {
        std::string body = jstr(v, "body");
        const auto b = body.find_first_not_of(" \t\r\n");
        const auto e = body.find_last_not_of(" \t\r\n");
        body = (b == std::string::npos) ? std::string{} : body.substr(b, e - b + 1);
        if (body.empty()) throw ValidationError("A comment cannot be empty.");
        if (body.size() > kMaxComment) throw ValidationError("A comment is at most 20,000 characters.");
        return body;
    }
    nlohmann::json handlePostComment(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("post_comment expects an object.");
        const int id = m2oId(v.value("task_id", nlohmann::json(0)));
        requireTask_(call, id);
        const std::string body = commentBody_(v);
        const auto ctx = extractContext_(call);
        if (ctx.uid <= 0) throw ValidationError("Sign in to comment.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("INSERT INTO mail_message (res_model, res_id, author_id, body, subtype, date) "
                          "VALUES ($1, $2, $3, $4, 'comment', now()) RETURNING id",
                          pqxx::params{std::string(kModel), id, ctx.uid, body});
        addWatcher_(txn, id, ctx.uid);   // you commented, so you care
        touch_(txn, id);
        txn.commit();
        return r[0][0].as<int>();
    }
    /// The comment, if this user may change it: their own, or any if admin.
    int ownComment_(pqxx::work& txn, const core::CallKwArgs& call, int messageId) {
        const auto ctx = extractContext_(call);
        auto r = txn.exec("SELECT res_id, COALESCE(author_id,0) FROM mail_message "
                          "WHERE id = $1 AND res_model = $2 AND subtype = 'comment'",
                          pqxx::params{messageId, std::string(kModel)});
        if (r.empty()) throw ValidationError("No such comment.");
        const int taskId = r[0][0].as<int>();
        requireTask_(call, taskId);
        if (r[0][1].as<int>() != ctx.uid && !ctx.isAdmin)
            throw ValidationError("Only the author can change a comment.");
        return taskId;
    }
    nlohmann::json handleEditComment(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("edit_comment expects an object.");
        const int mid = m2oId(v.value("message_id", nlohmann::json(0)));
        const std::string body = commentBody_(v);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        ownComment_(txn, call, mid);
        txn.exec("UPDATE mail_message SET body = $1, write_date = now() WHERE id = $2",
                 pqxx::params{body, mid});
        txn.commit();
        return true;
    }
    nlohmann::json handleDeleteComment(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int mid = v.is_object() ? m2oId(v.value("message_id", nlohmann::json(0))) : m2oId(v);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        ownComment_(txn, call, mid);
        txn.exec("DELETE FROM mail_message WHERE id = $1", pqxx::params{mid});
        txn.commit();
        return true;
    }

    // ---- labels and watchers ---------------------------------------------------
    /// Replace the labels. Each entry is an id or a NAME; a name that does not
    /// exist yet becomes a label, so typing "regression" and pressing Enter is
    /// all it takes.
    nlohmann::json handleSetTags(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("set_tags expects an object.");
        const int id = m2oId(v.value("task_id", nlohmann::json(0)));
        requireTask_(call, id);
        const auto ctx = extractContext_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::vector<int> tagIds;
        for (const auto& t : v.value("tags", nlohmann::json::array())) {
            if (t.is_string()) {
                std::string n = t.get<std::string>();
                const auto b = n.find_first_not_of(" \t"), e = n.find_last_not_of(" \t");
                n = (b == std::string::npos) ? std::string{} : n.substr(b, e - b + 1);
                if (n.empty()) continue;
                if (n.size() > 40) throw ValidationError("A label is at most 40 characters.");
                txn.exec("INSERT INTO project_tag (name) VALUES ($1) "
                         "ON CONFLICT ((lower(name))) DO NOTHING", pqxx::params{n});
                auto r = txn.exec("SELECT id FROM project_tag WHERE lower(name) = lower($1)",
                                  pqxx::params{n});
                if (!r.empty()) tagIds.push_back(r[0][0].as<int>());
            } else {
                const int tid = m2oId(t);
                if (tid > 0) tagIds.push_back(tid);
            }
        }
        const std::string before = tagList_(txn, id);
        txn.exec("DELETE FROM project_task_tag_rel WHERE task_id = $1", pqxx::params{id});
        for (const int tid : tagIds)
            txn.exec("INSERT INTO project_task_tag_rel (task_id, tag_id) "
                     "SELECT $1, id FROM project_tag WHERE id = $2 ON CONFLICT DO NOTHING",
                     pqxx::params{id, tid});
        const std::string after = tagList_(txn, id);
        if (before != after) {
            track_(txn, id, ctx.uid, "Labels: " + (before.empty() ? "—" : before) + " → " +
                                     (after.empty() ? "—" : after));
            touch_(txn, id);
        }
        txn.commit();
        return true;
    }
    nlohmann::json handleSetWatchers(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("set_watchers expects an object.");
        const int id = m2oId(v.value("task_id", nlohmann::json(0)));
        requireTask_(call, id);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("DELETE FROM project_task_watcher_rel WHERE task_id = $1", pqxx::params{id});
        for (const int uid : intsOf_(v.value("user_ids", nlohmann::json::array())))
            txn.exec("INSERT INTO project_task_watcher_rel (task_id, user_id) "
                     "SELECT $1, id FROM res_users WHERE id = $2 ON CONFLICT DO NOTHING",
                     pqxx::params{id, uid});
        txn.commit();
        return true;
    }
    /// Watch or stop watching, as the signed-in user.
    nlohmann::json handleWatch(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("watch expects an object.");
        const int id = m2oId(v.value("task_id", nlohmann::json(0)));
        requireTask_(call, id);
        const auto ctx = extractContext_(call);
        const bool on = !v.contains("watch") || !v["watch"].is_boolean() || v["watch"].get<bool>();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        if (on) addWatcher_(txn, id, ctx.uid);
        else txn.exec("DELETE FROM project_task_watcher_rel WHERE task_id = $1 AND user_id = $2",
                      pqxx::params{id, ctx.uid});
        txn.commit();
        return on;
    }

    /// The whole board in one call: the stage columns and their cards.
    ///
    /// Deliberately one round trip rather than a read_group plus a search per
    /// column — a board with eight stages would otherwise be nine requests
    /// every time a card moves, and the columns could render from a different
    /// instant than the cards in them.
    nlohmann::json handleBoard(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const int pid = (v.is_object() && v.contains("project_id")) ? m2oId(v["project_id"]) : 0;
        const int uid = (v.is_object() && v.contains("user_id"))    ? m2oId(v["user_id"])    : 0;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        // Stages: this project's own, else the shared set.
        nlohmann::json stages = nlohmann::json::array();
        std::string stageSql =
            "SELECT id, name, sequence, COALESCE(fold,false) AS fold, "
            "       COALESCE(is_closed,false) AS is_closed "
            "FROM project_task_type WHERE active AND (project_id IS NULL OR project_id=0";
        pqxx::params sp;
        if (pid > 0) { stageSql += " OR project_id=$1"; sp.append(pid); }
        stageSql += ") ORDER BY sequence, id";
        auto sres = pid > 0 ? txn.exec(stageSql, sp) : txn.exec(stageSql);
        for (const auto& row : sres)
            stages.push_back({{"id", row["id"].as<int>()},
                              {"name", row["name"].c_str()},
                              {"sequence", row["sequence"].as<int>(0)},
                              {"fold", row["fold"].as<bool>(false)},
                              {"is_closed", row["is_closed"].as<bool>(false)}});

        std::string where = "WHERE t.active";
        pqxx::params p; int n = 0;
        if (pid > 0) { where += " AND t.project_id=$" + std::to_string(++n); p.append(pid); }
        if (uid > 0) { where += " AND t.user_id=$"    + std::to_string(++n); p.append(uid); }
        // Ticket filters: type, one label, and free text over key and title.
        const std::string type = v.is_object() ? jstr(v, "issue_type") : std::string{};
        if (!type.empty()) { where += " AND t.issue_type=$" + std::to_string(++n); p.append(type); }
        const int tag = (v.is_object() && v.contains("tag_id")) ? m2oId(v["tag_id"]) : 0;
        if (tag > 0) {
            where += " AND EXISTS (SELECT 1 FROM project_task_tag_rel x "
                     "WHERE x.task_id = t.id AND x.tag_id=$" + std::to_string(++n) + ")";
            p.append(tag);
        }
        std::string q = v.is_object() ? jstr(v, "q") : std::string{};
        if (!q.empty()) {
            // Typed text is matched literally: % and _ are not wildcards here.
            std::string esc;
            for (char c : q) { if (c == '%' || c == '_' || c == '\\') esc += '\\'; esc += c; }
            const std::string ph = "$" + std::to_string(++n);
            where += " AND (t.key ILIKE " + ph + " OR t.name ILIKE " + ph + ")";
            p.append("%" + esc + "%");
        }

        const std::string sql =
            "SELECT t.id, t.key, t.name, t.stage_id, t.sequence, t.priority, "
            "       COALESCE(t.issue_type,'task') AS issue_type, "
            "       COALESCE(t.kanban_state,'normal') AS kanban_state, "
            "       t.date_deadline, COALESCE(t.planned_hours,0) AS planned_hours, "
            "       COALESCE(p.name,'') AS project_name, t.project_id, "
            "       COALESCE(u.login,'') AS user_login, t.user_id, "
            "       COALESCE(NULLIF(up.name,''), u.login, '') AS user_name, "
            "       COALESCE((SELECT sum(ts.unit_amount) FROM project_timesheet ts "
            "                  WHERE ts.task_id = t.id),0) AS logged_hours, "
            "       (SELECT count(*) FROM ir_attachment a "
            "         WHERE a.res_model = 'project.task' AND a.res_id = t.id) AS attachment_count, "
            "       (SELECT count(*) FROM mail_message m WHERE m.res_model = 'project.task' "
            "           AND m.res_id = t.id AND m.subtype = 'comment') AS comment_count, "
            "       COALESCE((SELECT json_agg(json_build_object('id', g.id, 'name', g.name, "
            "                                  'color', g.color) ORDER BY lower(g.name)) "
            "                  FROM project_task_tag_rel x JOIN project_tag g ON g.id = x.tag_id "
            "                 WHERE x.task_id = t.id), '[]')::text AS tags "
            "FROM project_task t "
            "LEFT JOIN project_project p ON p.id = t.project_id "
            "LEFT JOIN res_users u ON u.id = t.user_id "
            "LEFT JOIN res_partner up ON up.id = u.partner_id "
            + where + " ORDER BY t.sequence, t.id";
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);

        nlohmann::json tasks = nlohmann::json::array();
        for (const auto& row : res)
            tasks.push_back({
                {"id", row["id"].as<int>()},
                {"key", row["key"].is_null() ? "" : row["key"].c_str()},
                {"issue_type", row["issue_type"].c_str()},
                {"user_name", row["user_name"].c_str()},
                {"attachment_count", row["attachment_count"].as<long>(0)},
                {"comment_count", row["comment_count"].as<long>(0)},
                {"tags", nlohmann::json::parse(row["tags"].c_str(), nullptr, false)},
                {"name", row["name"].c_str()},
                {"stage_id", row["stage_id"].is_null() ? 0 : row["stage_id"].as<int>(0)},
                {"sequence", row["sequence"].as<int>(0)},
                {"priority", row["priority"].as<int>(0)},
                {"kanban_state", row["kanban_state"].c_str()},
                {"date_deadline", row["date_deadline"].is_null()
                                    ? nlohmann::json(nullptr)
                                    : nlohmann::json(row["date_deadline"].c_str())},
                {"planned_hours", row["planned_hours"].as<double>(0.0)},
                {"logged_hours",  row["logged_hours"].as<double>(0.0)},
                {"project_id", row["project_id"].is_null() ? 0 : row["project_id"].as<int>(0)},
                {"project_name", row["project_name"].c_str()},
                {"user_id", row["user_id"].is_null() ? 0 : row["user_id"].as<int>(0)},
                {"user_login", row["user_login"].c_str()}});

        return {{"stages", stages}, {"tasks", tasks}};
    }

    /// Drop a card into a stage, at a position. Sequence is renumbered for the
    /// target column only, which keeps the write small and the order stable.
    nlohmann::json handleMoveStage(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("move_stage expects an object.");
        const int taskId  = m2oId(v.value("task_id",  nlohmann::json(0)));
        const int stageId = m2oId(v.value("stage_id", nlohmann::json(0)));
        const int index   = (v.contains("index") && v["index"].is_number_integer())
                          ? std::max(0, v["index"].get<int>()) : -1;
        if (taskId <= 0)  throw ValidationError("task_id is required.");
        if (stageId <= 0) throw ValidationError("stage_id is required.");

        requireTask_(call, taskId);
        const auto ctx = extractContext_(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto cur = txn.exec("SELECT COALESCE(s.name,'') FROM project_task t "
                            "LEFT JOIN project_task_type s ON s.id = t.stage_id "
                            "WHERE t.id=$1 AND t.active", pqxx::params{taskId});
        if (cur.empty()) throw ValidationError("No such task.");
        const std::string fromStage = cur[0][0].c_str();
        auto st = txn.exec("SELECT COALESCE(is_closed,false), name FROM project_task_type WHERE id=$1 AND active",
                           pqxx::params{stageId});
        if (st.empty()) throw ValidationError("No such stage.");
        const bool closing = st[0][0].as<bool>(false);
        const std::string toStage = st[0][1].c_str();
        // A drag is a status change like any other, so it lands in the history.
        if (fromStage != toStage)
            track_(txn, taskId, ctx.uid, "Status: " + (fromStage.empty() ? std::string("—") : fromStage) +
                                         " → " + toStage);

        // Reaching a closing stage stamps the completion date; leaving one
        // clears it, so a card dragged back out is genuinely reopened.
        txn.exec("UPDATE project_task SET stage_id=$1, write_date=now(), "
                 "       date_end = CASE WHEN $2 THEN COALESCE(date_end, CURRENT_DATE) ELSE NULL END "
                 " WHERE id=$3",
                 pqxx::params{stageId, closing, taskId});

        if (index >= 0) {
            // Renumber the target column with the moved card inserted at `index`.
            std::vector<int> ids;
            for (const auto& row : txn.exec(
                    "SELECT id FROM project_task WHERE active AND stage_id=$1 AND id<>$2 "
                    "ORDER BY sequence, id", pqxx::params{stageId, taskId}))
                ids.push_back(row[0].as<int>());
            ids.insert(ids.begin() + std::min<size_t>(index, ids.size()), taskId);
            for (size_t i = 0; i < ids.size(); ++i)
                txn.exec("UPDATE project_task SET sequence=$1 WHERE id=$2",
                         pqxx::params{static_cast<int>((i + 1) * 10), ids[i]});
        }
        txn.commit();
        return {{"ok", true}, {"task_id", taskId}, {"stage_id", stageId}, {"closed", closing}};
    }
};

class ProjectTimesheetViewModel : public GenericViewModel<ProjectTimesheet> {
public:
    explicit ProjectTimesheetViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<ProjectTimesheet>(db), db_(std::move(db)) {
        REGISTER_METHOD("grid",      handleGrid)
        REGISTER_MUTATOR("set_cell",  handleSetCell)
        REGISTER_METHOD("summary",   handleSummary)
    }
private:
    std::shared_ptr<DbConnection> db_;

    static std::string weekStartOf(pqxx::work& txn, const std::string& date) {
        // Monday-based, computed by the database so the week boundary matches
        // whatever the server's locale would do in SQL elsewhere.
        auto r = date.empty()
            ? txn.exec("SELECT to_char(date_trunc('week', CURRENT_DATE),'YYYY-MM-DD')")
            : txn.exec("SELECT to_char(date_trunc('week', $1::date),'YYYY-MM-DD')",
                       pqxx::params{date});
        return r[0][0].c_str();
    }

    /// One week of timesheets as a grid: a row per (project, task), a column
    /// per day. Empty rows are included when the task was worked on earlier in
    /// the week, so a row does not vanish when its last hour is deleted.
    nlohmann::json handleGrid(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        const std::string anchor = (v.is_object() && v.contains("date") && v["date"].is_string())
                                 ? v["date"].get<std::string>() : std::string{};
        const std::string start = weekStartOf(txn, anchor);
        const int uid = (v.is_object() && v.contains("user_id")) ? m2oId(v["user_id"]) : 0;
        const int pid = (v.is_object() && v.contains("project_id")) ? m2oId(v["project_id"]) : 0;

        nlohmann::json days = nlohmann::json::array();
        for (const auto& row : txn.exec(
                "SELECT to_char(d,'YYYY-MM-DD') AS iso, to_char(d,'Dy') AS dow, "
                "       to_char(d,'DD Mon') AS label, "
                "       (d::date = CURRENT_DATE) AS is_today, "
                "       EXTRACT(ISODOW FROM d) >= 6 AS is_weekend "
                "FROM generate_series($1::date, $1::date + 6, '1 day') d",
                pqxx::params{start}))
            days.push_back({{"date", row["iso"].c_str()}, {"dow", row["dow"].c_str()},
                            {"label", row["label"].c_str()},
                            {"is_today", row["is_today"].as<bool>(false)},
                            {"is_weekend", row["is_weekend"].as<bool>(false)}});

        std::string where = "WHERE ts.date >= $1::date AND ts.date < $1::date + 7";
        pqxx::params p; p.append(start); int n = 1;
        if (uid > 0) { where += " AND ts.user_id=$"    + std::to_string(++n); p.append(uid); }
        if (pid > 0) { where += " AND ts.project_id=$" + std::to_string(++n); p.append(pid); }

        auto res = txn.exec(
            "SELECT ts.project_id, COALESCE(p.name,'') AS project_name, "
            "       COALESCE(ts.task_id,0) AS task_id, COALESCE(t.name,'') AS task_name, "
            "       to_char(ts.date,'YYYY-MM-DD') AS iso, sum(ts.unit_amount) AS hours "
            "FROM project_timesheet ts "
            "LEFT JOIN project_project p ON p.id = ts.project_id "
            "LEFT JOIN project_task t ON t.id = ts.task_id " + where +
            " GROUP BY 1,2,3,4,5 ORDER BY 2,4,5", p);

        // Rows are keyed by (project, task) so the same task never splits.
        std::map<std::pair<int,int>, nlohmann::json> rows;
        std::vector<std::pair<int,int>> order;
        for (const auto& row : res) {
            const int rpid = row["project_id"].is_null() ? 0 : row["project_id"].as<int>(0);
            const int rtid = row["task_id"].as<int>(0);
            const auto key = std::make_pair(rpid, rtid);
            if (!rows.count(key)) {
                rows[key] = {{"project_id", rpid}, {"project_name", row["project_name"].c_str()},
                             {"task_id", rtid},    {"task_name", row["task_name"].c_str()},
                             {"cells", nlohmann::json::object()}, {"total", 0.0}};
                order.push_back(key);
            }
            const double h = row["hours"].as<double>(0.0);
            rows[key]["cells"][row["iso"].c_str()] = h;
            rows[key]["total"] = rows[key]["total"].get<double>() + h;
        }

        nlohmann::json outRows = nlohmann::json::array();
        for (const auto& key : order) outRows.push_back(rows[key]);

        // Column totals, so the grid can show a day's load without the client
        // re-adding numbers the server already has.
        nlohmann::json colTotals = nlohmann::json::object();
        double grand = 0.0;
        for (const auto& d : days) {
            double sum = 0.0;
            for (const auto& r : outRows) {
                const std::string iso = d["date"].get<std::string>();
                if (r["cells"].contains(iso)) sum += r["cells"][iso].get<double>();
            }
            colTotals[d["date"].get<std::string>()] = sum;
            grand += sum;
        }

        return {{"week_start", start}, {"days", days}, {"rows", outRows},
                {"col_totals", colTotals}, {"total", grand}};
    }

    /// Set one cell to an absolute value. Idempotent by design: the grid sends
    /// what the cell should now read, not a delta, so a double-submit or a
    /// retry cannot silently double someone's day.
    nlohmann::json handleSetCell(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        if (!v.is_object()) throw ValidationError("set_cell expects an object.");
        const int projectId = m2oId(v.value("project_id", nlohmann::json(0)));
        const int taskId    = m2oId(v.value("task_id",    nlohmann::json(0)));
        const int userId    = m2oId(v.value("user_id",    nlohmann::json(0)));
        const std::string date = jstr(v, "date");
        double hours = 0.0;
        if (v.contains("hours")) {
            if (v["hours"].is_number()) hours = v["hours"].get<double>();
            else if (v["hours"].is_string()) {
                const std::string s = v["hours"].get<std::string>();
                if (!s.empty()) { try { hours = std::stod(s); } catch (...) {
                    throw ValidationError("Hours must be a number."); } }
            }
        }
        if (projectId <= 0) throw ValidationError("project_id is required.");
        if (date.empty())   throw ValidationError("date is required.");
        if (hours < 0)      throw ValidationError("Hours cannot be negative.");
        if (hours > 24)     throw ValidationError("A single entry cannot exceed 24 hours.");

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        if (txn.exec("SELECT 1 FROM project_project WHERE id=$1 AND active",
                     pqxx::params{projectId}).empty())
            throw ValidationError("No such project.");

        // Collapse the cell to a single row: several existing entries for the
        // same day become one, so what the grid shows is what is stored.
        auto existing = txn.exec(
            "SELECT id FROM project_timesheet "
            " WHERE project_id=$1 AND COALESCE(task_id,0)=$2 AND COALESCE(user_id,0)=$3 "
            "   AND date=$4::date ORDER BY id",
            pqxx::params{projectId, taskId, userId, date});

        std::vector<int> ids;
        for (const auto& row : existing) ids.push_back(row[0].as<int>());

        if (hours <= 0) {
            for (const int id : ids)
                txn.exec("DELETE FROM project_timesheet WHERE id=$1", pqxx::params{id});
            txn.commit();
            return {{"ok", true}, {"hours", 0.0}, {"deleted", static_cast<int>(ids.size())}};
        }
        if (ids.empty()) {
            txn.exec("INSERT INTO project_timesheet "
                     "  (name, date, project_id, task_id, user_id, unit_amount) "
                     "VALUES ($1, $2::date, $3, NULLIF($4,0), NULLIF($5,0), $6)",
                     pqxx::params{jstr(v, "name"), date, projectId, taskId, userId, hours});
        } else {
            txn.exec("UPDATE project_timesheet SET unit_amount=$1, write_date=now() WHERE id=$2",
                     pqxx::params{hours, ids[0]});
            for (size_t i = 1; i < ids.size(); ++i)
                txn.exec("DELETE FROM project_timesheet WHERE id=$1", pqxx::params{ids[i]});
        }
        txn.commit();
        return {{"ok", true}, {"hours", hours}};
    }

    /// Hours grouped by project (and optionally task) over a date range.
    nlohmann::json handleSummary(const core::CallKwArgs& call) {
        const auto v = call.arg(0);
        const std::string from = jstr(v, "date_from"), to = jstr(v, "date_to");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        std::string where = "WHERE TRUE";
        pqxx::params p; int n = 0;
        if (!from.empty()) { where += " AND ts.date >= $" + std::to_string(++n) + "::date"; p.append(from); }
        if (!to.empty())   { where += " AND ts.date <= $" + std::to_string(++n) + "::date"; p.append(to); }

        const std::string sql =
            "SELECT ts.project_id, COALESCE(p.name,'') AS project_name, "
            "       sum(ts.unit_amount) AS hours, count(*) AS entries "
            "FROM project_timesheet ts LEFT JOIN project_project p ON p.id=ts.project_id "
            + where + " GROUP BY 1,2 ORDER BY 3 DESC";
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);

        nlohmann::json arr = nlohmann::json::array();
        double grand = 0.0;
        for (const auto& row : res) {
            const double h = row["hours"].as<double>(0.0);
            grand += h;
            arr.push_back({{"project_id", row["project_id"].is_null() ? 0 : row["project_id"].as<int>(0)},
                           {"project_name", row["project_name"].c_str()},
                           {"hours", h}, {"entries", row["entries"].as<long>(0)}});
        }
        return {{"rows", arr}, {"total", grand}};
    }
};

// ================================================================
// 3. VIEWS
// ================================================================

class ProjectListView : public core::BaseView {
public:
    std::string viewName() const override { return "project.project.list"; }
    std::string modelName() const override { return "project.project"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Projects\">"
               "<field name=\"name\"/><field name=\"task_prefix\"/><field name=\"code\"/>"
               "<field name=\"user_id\"/>"
               "<field name=\"partner_id\"/><field name=\"date_start\"/><field name=\"date_end\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"name",       {{"type","char"},     {"string","Project"}}},
                {"task_prefix",{{"type","char"},     {"string","Key"}}},
                {"code",       {{"type","char"},     {"string","Reference"}}},
                {"user_id",    {{"type","many2one"}, {"string","Manager"},  {"relation","res.users"}}},
                {"partner_id", {{"type","many2one"}, {"string","Customer"}, {"relation","res.partner"}}},
                {"date_start", {{"type","date"},     {"string","Start"}}},
                {"date_end",   {{"type","date"},     {"string","End"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProjectFormView : public core::BaseView {
public:
    std::string viewName() const override { return "project.project.form"; }
    std::string modelName() const override { return "project.project"; }
    std::string viewType() const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Project\">"
               "<group><field name=\"name\"/><field name=\"task_prefix\"/>"
               "<field name=\"code\"/><field name=\"user_id\"/>"
               "<field name=\"partner_id\"/><field name=\"date_start\"/><field name=\"date_end\"/>"
               "<field name=\"allow_timesheets\"/><field name=\"active\"/></group>"
               "<field name=\"description\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {{"name",        {{"type","char"},     {"string","Project"}}},
                {"task_prefix", {{"type","char"},     {"string","Ticket Key Prefix"}}},
                {"code",        {{"type","char"},     {"string","Reference"}}},
                {"user_id",     {{"type","many2one"}, {"string","Manager"},  {"relation","res.users"}}},
                {"partner_id",  {{"type","many2one"}, {"string","Customer"}, {"relation","res.partner"}}},
                {"date_start",  {{"type","date"},     {"string","Start"}}},
                {"date_end",    {{"type","date"},     {"string","End"}}},
                {"allow_timesheets", {{"type","boolean"}, {"string","Allow Timesheets"}}},
                {"active",      {{"type","boolean"},  {"string","Active"}}},
                {"description", {{"type","text"},     {"string","Description"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProjectTaskListView : public core::BaseView {
public:
    std::string viewName() const override { return "project.task.list"; }
    std::string modelName() const override { return "project.task"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Tasks\">"
               "<field name=\"key\"/><field name=\"name\"/><field name=\"issue_type\"/>"
               "<field name=\"priority\"/><field name=\"project_id\"/><field name=\"stage_id\"/>"
               "<field name=\"user_id\"/><field name=\"date_deadline\"/>"
               "<field name=\"planned_hours\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"key",           {{"type","char"},     {"string","Key"}}},
                {"name",          {{"type","char"},     {"string","Task"}}},
                {"issue_type",    {{"type","selection"},{"string","Type"}}},
                {"priority",      {{"type","selection"},{"string","Priority"}}},
                {"project_id",    {{"type","many2one"}, {"string","Project"}, {"relation","project.project"}}},
                {"stage_id",      {{"type","many2one"}, {"string","Status"},  {"relation","project.task.type"}}},
                {"user_id",       {{"type","many2one"}, {"string","Assignee"}, {"relation","res.users"}}},
                {"date_deadline", {{"type","date"},     {"string","Due"}}},
                {"planned_hours", {{"type","float"},    {"string","Estimate (h)"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProjectTaskFormView : public core::BaseView {
public:
    std::string viewName() const override { return "project.task.form"; }
    std::string modelName() const override { return "project.task"; }
    std::string viewType() const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Task\">"
               "<group><field name=\"name\"/><field name=\"project_id\"/><field name=\"stage_id\"/>"
               "<field name=\"user_id\"/><field name=\"partner_id\"/><field name=\"date_deadline\"/>"
               "<field name=\"planned_hours\"/><field name=\"priority\"/>"
               "<field name=\"kanban_state\"/><field name=\"active\"/></group>"
               "<field name=\"description\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {{"name",          {{"type","char"},     {"string","Task"}}},
                {"project_id",    {{"type","many2one"}, {"string","Project"}, {"relation","project.project"}}},
                {"stage_id",      {{"type","many2one"}, {"string","Stage"},   {"relation","project.task.type"}}},
                {"user_id",       {{"type","many2one"}, {"string","Assigned To"}, {"relation","res.users"}}},
                {"partner_id",    {{"type","many2one"}, {"string","Customer"}, {"relation","res.partner"}}},
                {"date_deadline", {{"type","date"},     {"string","Deadline"}}},
                {"planned_hours", {{"type","float"},    {"string","Planned Hours"}}},
                {"priority",      {{"type","integer"},  {"string","Priority"}}},
                {"kanban_state",  {{"type","char"},     {"string","State"}}},
                {"active",        {{"type","boolean"},  {"string","Active"}}},
                {"description",   {{"type","text"},     {"string","Description"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProjectTimesheetListView : public core::BaseView {
public:
    std::string viewName() const override { return "project.timesheet.list"; }
    std::string modelName() const override { return "project.timesheet"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Timesheets\">"
               "<field name=\"date\"/><field name=\"project_id\"/><field name=\"task_id\"/>"
               "<field name=\"user_id\"/><field name=\"name\"/><field name=\"unit_amount\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"date",        {{"type","date"},     {"string","Date"}}},
                {"project_id",  {{"type","many2one"}, {"string","Project"}, {"relation","project.project"}}},
                {"task_id",     {{"type","many2one"}, {"string","Task"},    {"relation","project.task"}}},
                {"user_id",     {{"type","many2one"}, {"string","User"},    {"relation","res.users"}}},
                {"name",        {{"type","char"},     {"string","Description"}}},
                {"unit_amount", {{"type","float"},    {"string","Hours"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProjectTaskTypeListView : public core::BaseView {
public:
    std::string viewName() const override { return "project.task.type.list"; }
    std::string modelName() const override { return "project.task.type"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Stages\">"
               "<field name=\"sequence\"/><field name=\"name\"/><field name=\"project_id\"/>"
               "<field name=\"is_closed\"/><field name=\"fold\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"sequence",   {{"type","integer"},  {"string","Sequence"}}},
                {"name",       {{"type","char"},     {"string","Stage"}}},
                {"project_id", {{"type","many2one"}, {"string","Project"}, {"relation","project.project"}}},
                {"is_closed",  {{"type","boolean"},  {"string","Closing Stage"}}},
                {"fold",       {{"type","boolean"},  {"string","Folded"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ProjectTagListView : public core::BaseView {
public:
    std::string viewName() const override { return "project.tag.list"; }
    std::string modelName() const override { return "project.tag"; }
    std::string viewType() const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Labels\"><field name=\"name\"/><field name=\"active\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"name",   {{"type","char"},    {"string","Label"}}},
                {"active", {{"type","boolean"}, {"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// 4. MODULE
// ================================================================

ProjectModule::ProjectModule(core::ModelFactory& models, core::ServiceFactory& services,
                             core::ViewModelFactory& viewModels, core::ViewFactory& views)
    : models_(models), services_(services), viewModels_(viewModels), views_(views) {}

std::string              ProjectModule::moduleName()   const { return "project"; }
std::string              ProjectModule::version()      const { return "1.0"; }
std::vector<std::string> ProjectModule::dependencies() const { return {"base", "mail"}; }

void ProjectModule::registerMigrations(cerp::infrastructure::MigrationRunner& runner) {
    registerProjectMigrations(runner);
}

void ProjectModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("project.project",   [db]{ return std::make_shared<Project>(db); });
    models_.registerCreator("project.task.type", [db]{ return std::make_shared<ProjectTaskType>(db); });
    models_.registerCreator("project.task",      [db]{ return std::make_shared<ProjectTask>(db); });
    models_.registerCreator("project.timesheet", [db]{ return std::make_shared<ProjectTimesheet>(db); });
    models_.registerCreator("project.tag",       [db]{ return std::make_shared<ProjectTag>(db); });
}

void ProjectModule::registerServices() {}

void ProjectModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("project.project",   [db]{ return std::make_shared<ProjectViewModel>(db); });
    viewModels_.registerCreator("project.task.type", [db]{ return std::make_shared<GenericViewModel<ProjectTaskType>>(db); });
    viewModels_.registerCreator("project.task",      [db]{ return std::make_shared<ProjectTaskViewModel>(db); });
    viewModels_.registerCreator("project.timesheet", [db]{ return std::make_shared<ProjectTimesheetViewModel>(db); });
    viewModels_.registerCreator("project.tag",       [db]{ return std::make_shared<GenericViewModel<ProjectTag>>(db); });
}

void ProjectModule::registerViews() {
    views_.registerCreator("project.project.list",   []{ return std::make_shared<ProjectListView>(); });
    views_.registerCreator("project.project.form",   []{ return std::make_shared<ProjectFormView>(); });
    views_.registerCreator("project.task.list",      []{ return std::make_shared<ProjectTaskListView>(); });
    views_.registerCreator("project.task.form",      []{ return std::make_shared<ProjectTaskFormView>(); });
    views_.registerCreator("project.timesheet.list", []{ return std::make_shared<ProjectTimesheetListView>(); });
    views_.registerCreator("project.task.type.list", []{ return std::make_shared<ProjectTaskTypeListView>(); });
    views_.registerCreator("project.tag.list",       []{ return std::make_shared<ProjectTagListView>(); });
}

void ProjectModule::registerRoutes() {}

void ProjectModule::initialize() {
    ensureSchema_();
    seedStages_();
    seedMenus_();
}

void ProjectModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS project_project (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            code        VARCHAR,
            description TEXT,
            partner_id  INTEGER REFERENCES res_partner(id) ON DELETE SET NULL,
            user_id     INTEGER REFERENCES res_users(id)   ON DELETE SET NULL,
            company_id  INTEGER REFERENCES res_company(id),
            date_start  DATE,
            date_end    DATE,
            sequence    INTEGER NOT NULL DEFAULT 10,
            color       INTEGER NOT NULL DEFAULT 0,
            allow_timesheets BOOLEAN NOT NULL DEFAULT TRUE,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS project_task_type (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            project_id  INTEGER REFERENCES project_project(id) ON DELETE CASCADE,
            sequence    INTEGER NOT NULL DEFAULT 10,
            fold        BOOLEAN NOT NULL DEFAULT FALSE,
            is_closed   BOOLEAN NOT NULL DEFAULT FALSE,
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS project_task (
            id            SERIAL PRIMARY KEY,
            name          VARCHAR NOT NULL,
            description   TEXT,
            project_id    INTEGER NOT NULL REFERENCES project_project(id) ON DELETE CASCADE,
            stage_id      INTEGER REFERENCES project_task_type(id) ON DELETE SET NULL,
            user_id       INTEGER REFERENCES res_users(id)   ON DELETE SET NULL,
            partner_id    INTEGER REFERENCES res_partner(id) ON DELETE SET NULL,
            parent_id     INTEGER REFERENCES project_task(id) ON DELETE SET NULL,
            company_id    INTEGER REFERENCES res_company(id),
            date_deadline DATE,
            date_end      DATE,
            kanban_state  VARCHAR NOT NULL DEFAULT 'normal',
            sequence      INTEGER NOT NULL DEFAULT 10,
            priority      INTEGER NOT NULL DEFAULT 0,
            planned_hours NUMERIC(10,2) NOT NULL DEFAULT 0,
            active        BOOLEAN NOT NULL DEFAULT TRUE,
            create_date   TIMESTAMP DEFAULT now(),
            write_date    TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS project_timesheet (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR,
            date        DATE NOT NULL DEFAULT CURRENT_DATE,
            project_id  INTEGER NOT NULL REFERENCES project_project(id) ON DELETE CASCADE,
            task_id     INTEGER REFERENCES project_task(id) ON DELETE CASCADE,
            employee_id INTEGER REFERENCES hr_employee(id)  ON DELETE SET NULL,
            user_id     INTEGER REFERENCES res_users(id)    ON DELETE SET NULL,
            company_id  INTEGER REFERENCES res_company(id),
            unit_amount NUMERIC(10,2) NOT NULL DEFAULT 0,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    // The board reads by (project, stage, sequence); the grid reads a week of
    // one user's lines. Both are the hot path for their screen.
    txn.exec("CREATE INDEX IF NOT EXISTS idx_project_task_board "
             "ON project_task (project_id, stage_id, sequence)");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_project_timesheet_week "
             "ON project_timesheet (user_id, date)");
    txn.exec("CREATE INDEX IF NOT EXISTS idx_project_timesheet_task "
             "ON project_timesheet (task_id)");

    txn.commit();
}

void ProjectModule::seedStages_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // Shared stages (project_id NULL). A board with no stages cannot render a
    // single column, so a usable default set matters more here than in most
    // seeds. Seeded once — a renamed or deleted stage is not restored, because
    // that would fight the user every restart.
    if (txn.exec("SELECT 1 FROM project_task_type WHERE project_id IS NULL LIMIT 1").empty()) {
        struct S { const char* name; int seq; bool closed; bool fold; };
        static const S kStages[] = {
            {"New",         10, false, false},
            {"In Progress", 20, false, false},
            {"Review",      30, false, false},
            {"Done",        40, true,  false},
            {"Cancelled",   50, true,  true},
        };
        for (const auto& s : kStages)
            txn.exec("INSERT INTO project_task_type (name, sequence, is_closed, fold) "
                     "VALUES ($1,$2,$3,$4)",
                     pqxx::params{s.name, s.seq, s.closed, s.fold});
        LOG_INFO << "[project] seeded 5 default task stages";
    }
    txn.commit();
}

void ProjectModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, path, context) VALUES
            (108, 'Task Board',        'project.board',     'list',      'task-board',    '{}'),
            (109, 'Projects',          'project.project',   'list,form', 'projects',      '{}'),
            (110, 'Tasks',             'project.task',      'list,form', 'tasks',         '{}'),
            (111, 'Timesheets',        'project.timegrid',  'list',      'timesheet',     '{}'),
            (112, 'Timesheet Entries', 'project.timesheet', 'list,form', 'timesheet-list','{}'),
            (113, 'Task Stages',       'project.task.type', 'list,form', 'task-stages',   '{}'),
            (128, 'Labels',            'project.tag',       'list,form', 'task-labels',   '{}')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode, path=EXCLUDED.path, domain=NULL
    )");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");

    // Level 0: the app tile. DO NOTHING, never DO UPDATE — overwriting an app
    // root removes it from the home screen (see verify_menu_ids.sh).
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (130, 'Project', NULL, 55, NULL, 'project')
        ON CONFLICT (id) DO NOTHING
    )");

    // 131 and 132 belong to ReportModule; an earlier build of this module
    // seeded them and 133-136 by mistake. Remove only rows that point at THIS
    // module's actions, so ReportModule's own 131/132 are left alone.
    txn.exec("DELETE FROM ir_ui_menu WHERE id BETWEEN 131 AND 136 "
             "  AND action_id BETWEEN 108 AND 113");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (137, 'Task Board',        130, 10, 108),
            (138, 'Projects',          130, 20, 109),
            (139, 'Tasks',             130, 30, 110),
            (140, 'Timesheets',        130, 40, 111),
            (141, 'Timesheet Entries', 130, 50, 112),
            (142, 'Task Stages',       130, 60, 113),
            (143, 'Labels',            130, 70, 128)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

    txn.commit();
}

} // namespace cerp::modules::project
