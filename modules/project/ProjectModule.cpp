// =============================================================
// modules/project/ProjectModule.cpp — docs/100
// =============================================================
#include "ProjectModule.hpp"
#include <drogon/drogon.h>          // LOG_INFO
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "BaseViewModel.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::project {

using namespace odoo::infrastructure;
using namespace odoo::core;

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

// ================================================================
// 1. MODELS
// ================================================================

class Project : public BaseModel<Project> {
public:
    static constexpr const char* MODEL_NAME = "project.project";
    static constexpr const char* TABLE_NAME = "project_project";

    std::string name, code, description, dateStart, dateEnd;
    int  partnerId = 0, userId = 0, companyId = 0, sequence = 10, color = 0;
    bool allowTimesheets = true, active = true;

    explicit Project(std::shared_ptr<DbConnection> db) : BaseModel<Project>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",        FieldType::Char,     "Project", true});
        fieldRegistry_.add({"code",        FieldType::Char,     "Reference"});
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

    std::string name, description, dateDeadline, dateEnd, kanbanState = "normal";
    int    projectId = 0, stageId = 0, userId = 0, partnerId = 0, parentId = 0,
           companyId = 0, sequence = 10, priority = 0;
    double plannedHours = 0.0;
    bool   active = true;

    explicit ProjectTask(std::shared_ptr<DbConnection> db) : BaseModel<ProjectTask>(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",          FieldType::Char,     "Task", true});
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
        fieldRegistry_.add({"priority",      FieldType::Integer,  "Priority"});
        fieldRegistry_.add({"planned_hours", FieldType::Float,    "Planned Hours"});
        fieldRegistry_.add({"active",        FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;  j["description"] = description;
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
        if (j.contains("priority") && j["priority"].is_number()) priority = j["priority"].get<int>();
        if (j.contains("planned_hours") && j["planned_hours"].is_number())
            plannedHours = j["planned_hours"].get<double>();
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty())     e.push_back("name is required");
        if (projectId <= 0)   e.push_back("project_id is required");
        if (plannedHours < 0) e.push_back("Planned Hours cannot be negative");
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

// ================================================================
// 2. VIEWMODELS
// ================================================================

class ProjectViewModel : public GenericViewModel<Project> {
public:
    explicit ProjectViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<Project>(db), db_(std::move(db)) {
        REGISTER_METHOD("stats", handleStats)
    }
private:
    std::shared_ptr<DbConnection> db_;

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
    }
private:
    std::shared_ptr<DbConnection> db_;

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

        const std::string sql =
            "SELECT t.id, t.name, t.stage_id, t.sequence, t.priority, "
            "       COALESCE(t.kanban_state,'normal') AS kanban_state, "
            "       t.date_deadline, COALESCE(t.planned_hours,0) AS planned_hours, "
            "       COALESCE(p.name,'') AS project_name, t.project_id, "
            "       COALESCE(u.login,'') AS user_login, t.user_id, "
            "       COALESCE((SELECT sum(ts.unit_amount) FROM project_timesheet ts "
            "                  WHERE ts.task_id = t.id),0) AS logged_hours "
            "FROM project_task t "
            "LEFT JOIN project_project p ON p.id = t.project_id "
            "LEFT JOIN res_users u ON u.id = t.user_id "
            + where + " ORDER BY t.sequence, t.id";
        auto res = n ? txn.exec(sql, p) : txn.exec(sql);

        nlohmann::json tasks = nlohmann::json::array();
        for (const auto& row : res)
            tasks.push_back({
                {"id", row["id"].as<int>()},
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

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        if (txn.exec("SELECT 1 FROM project_task WHERE id=$1 AND active",
                     pqxx::params{taskId}).empty())
            throw ValidationError("No such task.");
        auto st = txn.exec("SELECT COALESCE(is_closed,false) FROM project_task_type WHERE id=$1 AND active",
                           pqxx::params{stageId});
        if (st.empty()) throw ValidationError("No such stage.");
        const bool closing = st[0][0].as<bool>(false);

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
               "<field name=\"name\"/><field name=\"code\"/><field name=\"user_id\"/>"
               "<field name=\"partner_id\"/><field name=\"date_start\"/><field name=\"date_end\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"name",       {{"type","char"},     {"string","Project"}}},
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
               "<group><field name=\"name\"/><field name=\"code\"/><field name=\"user_id\"/>"
               "<field name=\"partner_id\"/><field name=\"date_start\"/><field name=\"date_end\"/>"
               "<field name=\"allow_timesheets\"/><field name=\"active\"/></group>"
               "<field name=\"description\"/>"
               "</form>";
    }
    nlohmann::json fields() const override {
        return {{"name",        {{"type","char"},     {"string","Project"}}},
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
               "<field name=\"name\"/><field name=\"project_id\"/><field name=\"stage_id\"/>"
               "<field name=\"user_id\"/><field name=\"date_deadline\"/>"
               "<field name=\"planned_hours\"/><field name=\"kanban_state\"/>"
               "</list>";
    }
    nlohmann::json fields() const override {
        return {{"name",          {{"type","char"},     {"string","Task"}}},
                {"project_id",    {{"type","many2one"}, {"string","Project"}, {"relation","project.project"}}},
                {"stage_id",      {{"type","many2one"}, {"string","Stage"},   {"relation","project.task.type"}}},
                {"user_id",       {{"type","many2one"}, {"string","Assigned To"}, {"relation","res.users"}}},
                {"date_deadline", {{"type","date"},     {"string","Deadline"}}},
                {"planned_hours", {{"type","float"},    {"string","Planned Hours"}}},
                {"kanban_state",  {{"type","char"},     {"string","State"}}}};
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

// ================================================================
// 4. MODULE
// ================================================================

ProjectModule::ProjectModule(core::ModelFactory& models, core::ServiceFactory& services,
                             core::ViewModelFactory& viewModels, core::ViewFactory& views)
    : models_(models), services_(services), viewModels_(viewModels), views_(views) {}

std::string              ProjectModule::moduleName()   const { return "project"; }
std::string              ProjectModule::version()      const { return "1.0"; }
std::vector<std::string> ProjectModule::dependencies() const { return {"base"}; }

void ProjectModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("project.project",   [db]{ return std::make_shared<Project>(db); });
    models_.registerCreator("project.task.type", [db]{ return std::make_shared<ProjectTaskType>(db); });
    models_.registerCreator("project.task",      [db]{ return std::make_shared<ProjectTask>(db); });
    models_.registerCreator("project.timesheet", [db]{ return std::make_shared<ProjectTimesheet>(db); });
}

void ProjectModule::registerServices() {}

void ProjectModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("project.project",   [db]{ return std::make_shared<ProjectViewModel>(db); });
    viewModels_.registerCreator("project.task.type", [db]{ return std::make_shared<GenericViewModel<ProjectTaskType>>(db); });
    viewModels_.registerCreator("project.task",      [db]{ return std::make_shared<ProjectTaskViewModel>(db); });
    viewModels_.registerCreator("project.timesheet", [db]{ return std::make_shared<ProjectTimesheetViewModel>(db); });
}

void ProjectModule::registerViews() {
    views_.registerCreator("project.project.list",   []{ return std::make_shared<ProjectListView>(); });
    views_.registerCreator("project.project.form",   []{ return std::make_shared<ProjectFormView>(); });
    views_.registerCreator("project.task.list",      []{ return std::make_shared<ProjectTaskListView>(); });
    views_.registerCreator("project.task.form",      []{ return std::make_shared<ProjectTaskFormView>(); });
    views_.registerCreator("project.timesheet.list", []{ return std::make_shared<ProjectTimesheetListView>(); });
    views_.registerCreator("project.task.type.list", []{ return std::make_shared<ProjectTaskTypeListView>(); });
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
            (113, 'Task Stages',       'project.task.type', 'list,form', 'task-stages',   '{}')
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
            (142, 'Task Stages',       130, 60, 113)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");

    txn.commit();
}

} // namespace odoo::modules::project
