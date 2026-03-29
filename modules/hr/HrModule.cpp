// =============================================================
// modules/hr/HrModule.cpp  — full implementation
// =============================================================
#include "HrModule.hpp"
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::hr {

using namespace odoo::infrastructure;
using namespace odoo::core;

// ----------------------------------------------------------------
// helper
// ----------------------------------------------------------------
namespace {
inline int hrM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer())
        return v[0].get<int>();
    return 0;
}
} // anonymous namespace

// ================================================================
// 1. MODELS
// ================================================================

class ResourceCalendar : public BaseModel<ResourceCalendar> {
public:
    static constexpr const char* MODEL_NAME = "resource.calendar";
    static constexpr const char* TABLE_NAME = "resource_calendar";

    explicit ResourceCalendar(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db)) {}

    std::string name;
    double      hoursPerDay = 8.0;
    int         companyId   = 1;
    bool        active      = true;

    void registerFields() {
        fieldRegistry_.add({"name",          FieldType::Char,    "Working Schedule", true});
        fieldRegistry_.add({"hours_per_day", FieldType::Float,   "Hours per Day"});
        fieldRegistry_.add({"company_id",    FieldType::Many2one,"Company",          false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",        FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]          = name;
        j["hours_per_day"] = hoursPerDay;
        j["company_id"]    = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]        = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")          && j["name"].is_string())          name        = j["name"].get<std::string>();
        if (j.contains("hours_per_day") && j["hours_per_day"].is_number()) hoursPerDay = j["hours_per_day"].get<double>();
        if (j.contains("company_id"))    companyId  = hrM2oId(j["company_id"]);
        if (j.contains("active")        && j["active"].is_boolean())       active      = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j;
        serializeFields(j);
        j["id"]           = getId();
        j["display_name"] = name;
        return j;
    }

    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Name is required");
        return e;
    }
};

class HrDepartment : public BaseModel<HrDepartment> {
public:
    static constexpr const char* MODEL_NAME = "hr.department";
    static constexpr const char* TABLE_NAME = "hr_department";

    explicit HrDepartment(std::shared_ptr<DbConnection> db)
        : BaseModel(std::move(db)) {}

    std::string name;
    int         parentId  = 0;
    int         managerId = 0;
    int         companyId = 1;
    bool        active    = true;

    void registerFields() {
        fieldRegistry_.add({"name",       FieldType::Char,    "Department Name",   true});
        fieldRegistry_.add({"parent_id",  FieldType::Many2one,"Parent Department", false, false, true, false, "hr.department"});
        fieldRegistry_.add({"manager_id", FieldType::Many2one,"Manager",           false, false, true, false, "hr.employee"});
        fieldRegistry_.add({"company_id", FieldType::Many2one,"Company",           false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",     FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]       = name;
        j["parent_id"]  = parentId  > 0 ? nlohmann::json::array({parentId,  ""}) : nlohmann::json(false);
        j["manager_id"] = managerId > 0 ? nlohmann::json::array({managerId, ""}) : nlohmann::json(false);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId)              : nlohmann::json(false);
        j["active"]     = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")      && j["name"].is_string()) name      = j["name"].get<std::string>();
        if (j.contains("parent_id"))  parentId  = hrM2oId(j["parent_id"]);
        if (j.contains("manager_id")) managerId = hrM2oId(j["manager_id"]);
        if (j.contains("company_id")) companyId = hrM2oId(j["company_id"]);
        if (j.contains("active")    && j["active"].is_boolean()) active = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Department name is required");
        return e;
    }
};

class HrJob : public BaseModel<HrJob> {
public:
    static constexpr const char* MODEL_NAME = "hr.job";
    static constexpr const char* TABLE_NAME = "hr_job";

    explicit HrJob(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string name;
    std::string description;
    int         departmentId = 0;
    int         companyId    = 1;
    bool        active       = true;

    void registerFields() {
        fieldRegistry_.add({"name",          FieldType::Char,    "Job Position",   true});
        fieldRegistry_.add({"description",   FieldType::Text,    "Job Description"});
        fieldRegistry_.add({"department_id", FieldType::Many2one,"Department",     false, false, true, false, "hr.department"});
        fieldRegistry_.add({"company_id",    FieldType::Many2one,"Company",        false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",        FieldType::Boolean, "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]          = name;
        j["description"]   = description.empty() ? nlohmann::json(false) : nlohmann::json(description);
        j["department_id"] = departmentId > 0 ? nlohmann::json::array({departmentId, ""}) : nlohmann::json(false);
        j["company_id"]    = companyId    > 0 ? nlohmann::json(companyId)                 : nlohmann::json(false);
        j["active"]        = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")          && j["name"].is_string())        name        = j["name"].get<std::string>();
        if (j.contains("description")   && j["description"].is_string()) description = j["description"].get<std::string>();
        if (j.contains("department_id"))  departmentId = hrM2oId(j["department_id"]);
        if (j.contains("company_id"))     companyId    = hrM2oId(j["company_id"]);
        if (j.contains("active")        && j["active"].is_boolean())     active      = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Job position name is required");
        return e;
    }
};

class HrEmployee : public BaseModel<HrEmployee> {
public:
    static constexpr const char* MODEL_NAME = "hr.employee";
    static constexpr const char* TABLE_NAME = "hr_employee";

    explicit HrEmployee(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string name;
    int         jobId              = 0;
    int         departmentId       = 0;
    int         parentId           = 0;
    int         coachId            = 0;
    std::string workEmail;
    std::string workPhone;
    std::string mobilePhone;
    int         resourceCalendarId = 0;
    int         companyId          = 1;
    int         userId             = 0;
    int         addressId          = 0;
    std::string gender;
    std::string marital;
    std::string birthday;
    std::string identificationId;
    std::string privateEmail;
    bool        active             = true;

    void registerFields() {
        fieldRegistry_.add({"name",                 FieldType::Char,     "Employee Name",   true});
        fieldRegistry_.add({"job_id",               FieldType::Many2one, "Job Position",    false, false, true, false, "hr.job"});
        fieldRegistry_.add({"department_id",        FieldType::Many2one, "Department",      false, false, true, false, "hr.department"});
        fieldRegistry_.add({"parent_id",            FieldType::Many2one, "Manager",         false, false, true, false, "hr.employee"});
        fieldRegistry_.add({"coach_id",             FieldType::Many2one, "Coach",           false, false, true, false, "hr.employee"});
        fieldRegistry_.add({"work_email",           FieldType::Char,     "Work Email"});
        fieldRegistry_.add({"work_phone",           FieldType::Char,     "Work Phone"});
        fieldRegistry_.add({"mobile_phone",         FieldType::Char,     "Work Mobile"});
        fieldRegistry_.add({"resource_calendar_id", FieldType::Many2one, "Working Hours",   false, false, true, false, "resource.calendar"});
        fieldRegistry_.add({"company_id",           FieldType::Many2one, "Company",         false, false, true, false, "res.company"});
        fieldRegistry_.add({"user_id",              FieldType::Many2one, "Related User",    false, false, true, false, "res.users"});
        fieldRegistry_.add({"address_id",           FieldType::Many2one, "Work Address",    false, false, true, false, "res.partner"});
        fieldRegistry_.add({"gender",               FieldType::Selection,"Gender"});
        fieldRegistry_.add({"marital",              FieldType::Selection,"Marital Status"});
        fieldRegistry_.add({"birthday",             FieldType::Date,     "Date of Birth"});
        fieldRegistry_.add({"identification_id",    FieldType::Char,     "Identification No"});
        fieldRegistry_.add({"private_email",        FieldType::Char,     "Private Email"});
        fieldRegistry_.add({"active",               FieldType::Boolean,  "Active"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]                 = name;
        j["job_id"]               = jobId              > 0 ? nlohmann::json::array({jobId,              ""}) : nlohmann::json(false);
        j["department_id"]        = departmentId       > 0 ? nlohmann::json::array({departmentId,       ""}) : nlohmann::json(false);
        j["parent_id"]            = parentId           > 0 ? nlohmann::json::array({parentId,           ""}) : nlohmann::json(false);
        j["coach_id"]             = coachId            > 0 ? nlohmann::json::array({coachId,            ""}) : nlohmann::json(false);
        j["work_email"]           = workEmail.empty()   ? nlohmann::json(false) : nlohmann::json(workEmail);
        j["work_phone"]           = workPhone.empty()   ? nlohmann::json(false) : nlohmann::json(workPhone);
        j["mobile_phone"]         = mobilePhone.empty() ? nlohmann::json(false) : nlohmann::json(mobilePhone);
        j["resource_calendar_id"] = resourceCalendarId > 0 ? nlohmann::json::array({resourceCalendarId, ""}) : nlohmann::json(false);
        j["company_id"]           = companyId          > 0 ? nlohmann::json(companyId)                      : nlohmann::json(false);
        j["user_id"]              = userId             > 0 ? nlohmann::json(userId)                         : nlohmann::json(false);
        j["address_id"]           = addressId          > 0 ? nlohmann::json::array({addressId,          ""}) : nlohmann::json(false);
        j["gender"]               = gender.empty()      ? nlohmann::json(false) : nlohmann::json(gender);
        j["marital"]              = marital.empty()     ? nlohmann::json(false) : nlohmann::json(marital);
        j["birthday"]             = birthday.empty()    ? nlohmann::json(false) : nlohmann::json(birthday);
        j["identification_id"]    = identificationId.empty() ? nlohmann::json(false) : nlohmann::json(identificationId);
        j["private_email"]        = privateEmail.empty()     ? nlohmann::json(false) : nlohmann::json(privateEmail);
        j["active"]               = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")                 && j["name"].is_string())             name              = j["name"].get<std::string>();
        if (j.contains("job_id"))               jobId              = hrM2oId(j["job_id"]);
        if (j.contains("department_id"))        departmentId       = hrM2oId(j["department_id"]);
        if (j.contains("parent_id"))            parentId           = hrM2oId(j["parent_id"]);
        if (j.contains("coach_id"))             coachId            = hrM2oId(j["coach_id"]);
        if (j.contains("work_email")           && j["work_email"].is_string())       workEmail         = j["work_email"].get<std::string>();
        if (j.contains("work_phone")           && j["work_phone"].is_string())       workPhone         = j["work_phone"].get<std::string>();
        if (j.contains("mobile_phone")         && j["mobile_phone"].is_string())     mobilePhone       = j["mobile_phone"].get<std::string>();
        if (j.contains("resource_calendar_id")) resourceCalendarId = hrM2oId(j["resource_calendar_id"]);
        if (j.contains("company_id"))           companyId          = hrM2oId(j["company_id"]);
        if (j.contains("user_id"))              userId             = hrM2oId(j["user_id"]);
        if (j.contains("address_id"))           addressId          = hrM2oId(j["address_id"]);
        if (j.contains("gender")              && j["gender"].is_string())            gender            = j["gender"].get<std::string>();
        if (j.contains("marital")             && j["marital"].is_string())           marital           = j["marital"].get<std::string>();
        if (j.contains("birthday")            && j["birthday"].is_string())          birthday          = j["birthday"].get<std::string>();
        if (j.contains("identification_id")   && j["identification_id"].is_string()) identificationId  = j["identification_id"].get<std::string>();
        if (j.contains("private_email")       && j["private_email"].is_string())     privateEmail      = j["private_email"].get<std::string>();
        if (j.contains("active")              && j["active"].is_boolean())           active            = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Employee name is required");
        return e;
    }
};

// ================================================================
// 2. VIEWS
// ================================================================

class ResourceCalendarListView : public core::BaseView {
public:
    std::string viewName()  const override { return "resource.calendar.list"; }
    std::string modelName() const override { return "resource.calendar"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Working Schedules\"><field name=\"name\"/>"
               "<field name=\"hours_per_day\"/><field name=\"company_id\"/>"
               "<field name=\"active\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Working Schedule"}}},
                {"hours_per_day",{{"type","float"},{"string","Hours/Day"}}},
                {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class ResourceCalendarFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "resource.calendar.form"; }
    std::string modelName() const override { return "resource.calendar"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Working Schedule\"><field name=\"name\"/>"
               "<field name=\"hours_per_day\"/><field name=\"company_id\"/>"
               "<field name=\"active\"/></form>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Working Schedule"}}},
                {"hours_per_day",{{"type","float"},{"string","Hours/Day"}}},
                {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrDepartmentListView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.department.list"; }
    std::string modelName() const override { return "hr.department"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Departments\"><field name=\"name\"/><field name=\"parent_id\"/>"
               "<field name=\"manager_id\"/><field name=\"company_id\"/><field name=\"active\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Department"}}},
                {"parent_id",{{"type","many2one"},{"string","Parent Department"},{"relation","hr.department"}}},
                {"manager_id",{{"type","many2one"},{"string","Manager"},{"relation","hr.employee"}}},
                {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrDepartmentFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.department.form"; }
    std::string modelName() const override { return "hr.department"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Department\"><field name=\"name\"/><field name=\"parent_id\"/>"
               "<field name=\"manager_id\"/><field name=\"company_id\"/><field name=\"active\"/></form>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Department"}}},
                {"parent_id",{{"type","many2one"},{"string","Parent Department"},{"relation","hr.department"}}},
                {"manager_id",{{"type","many2one"},{"string","Manager"},{"relation","hr.employee"}}},
                {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrJobListView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.job.list"; }
    std::string modelName() const override { return "hr.job"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Job Positions\"><field name=\"name\"/>"
               "<field name=\"department_id\"/><field name=\"company_id\"/><field name=\"active\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Job Position"}}},
                {"department_id",{{"type","many2one"},{"string","Department"},{"relation","hr.department"}}},
                {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrJobFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.job.form"; }
    std::string modelName() const override { return "hr.job"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Job Position\"><field name=\"name\"/>"
               "<field name=\"department_id\"/><field name=\"company_id\"/>"
               "<field name=\"description\"/><field name=\"active\"/></form>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Job Position"}}},
                {"department_id",{{"type","many2one"},{"string","Department"},{"relation","hr.department"}}},
                {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
                {"description",{{"type","text"},{"string","Job Description"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrEmployeeListView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.employee.list"; }
    std::string modelName() const override { return "hr.employee"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Employees\"><field name=\"name\"/><field name=\"job_id\"/>"
               "<field name=\"department_id\"/><field name=\"work_email\"/>"
               "<field name=\"work_phone\"/><field name=\"active\"/></list>";
    }
    nlohmann::json fields() const override {
        return {{"name",{{"type","char"},{"string","Employee Name"}}},
                {"job_id",{{"type","many2one"},{"string","Job Position"},{"relation","hr.job"}}},
                {"department_id",{{"type","many2one"},{"string","Department"},{"relation","hr.department"}}},
                {"work_email",{{"type","char"},{"string","Work Email"}}},
                {"work_phone",{{"type","char"},{"string","Work Phone"}}},
                {"active",{{"type","boolean"},{"string","Active"}}}};
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrEmployeeFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.employee.form"; }
    std::string modelName() const override { return "hr.employee"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Employee\">"
               "<field name=\"name\"/><field name=\"job_id\"/><field name=\"department_id\"/>"
               "<field name=\"parent_id\"/><field name=\"coach_id\"/><field name=\"work_email\"/>"
               "<field name=\"work_phone\"/><field name=\"mobile_phone\"/>"
               "<field name=\"resource_calendar_id\"/><field name=\"address_id\"/>"
               "<field name=\"company_id\"/><field name=\"user_id\"/>"
               "<field name=\"gender\"/><field name=\"marital\"/><field name=\"birthday\"/>"
               "<field name=\"identification_id\"/><field name=\"private_email\"/>"
               "<field name=\"active\"/></form>";
    }
    nlohmann::json fields() const override {
        return {
            {"name",{{"type","char"},{"string","Employee Name"}}},
            {"job_id",{{"type","many2one"},{"string","Job Position"},{"relation","hr.job"}}},
            {"department_id",{{"type","many2one"},{"string","Department"},{"relation","hr.department"}}},
            {"parent_id",{{"type","many2one"},{"string","Manager"},{"relation","hr.employee"}}},
            {"coach_id",{{"type","many2one"},{"string","Coach"},{"relation","hr.employee"}}},
            {"work_email",{{"type","char"},{"string","Work Email"}}},
            {"work_phone",{{"type","char"},{"string","Work Phone"}}},
            {"mobile_phone",{{"type","char"},{"string","Work Mobile"}}},
            {"resource_calendar_id",{{"type","many2one"},{"string","Working Hours"},{"relation","resource.calendar"}}},
            {"address_id",{{"type","many2one"},{"string","Work Address"},{"relation","res.partner"}}},
            {"company_id",{{"type","many2one"},{"string","Company"},{"relation","res.company"}}},
            {"user_id",{{"type","many2one"},{"string","Related User"},{"relation","res.users"}}},
            {"gender",{{"type","selection"},{"string","Gender"}}},
            {"marital",{{"type","selection"},{"string","Marital Status"}}},
            {"birthday",{{"type","date"},{"string","Date of Birth"}}},
            {"identification_id",{{"type","char"},{"string","Identification No"}}},
            {"private_email",{{"type","char"},{"string","Private Email"}}},
            {"active",{{"type","boolean"},{"string","Active"}}},
        };
    }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

// ================================================================
// 3. MODULE
// ================================================================

HrModule::HrModule(core::ModelFactory&     modelFactory,
                   core::ServiceFactory&   serviceFactory,
                   core::ViewModelFactory& viewModelFactory,
                   core::ViewFactory&      viewFactory)
    : models_    (modelFactory)
    , services_  (serviceFactory)
    , viewModels_(viewModelFactory)
    , views_     (viewFactory)
{}

std::string              HrModule::moduleName()   const { return "hr"; }
std::string              HrModule::version()      const { return "19.0.1.0.0"; }
std::vector<std::string> HrModule::dependencies() const { return {"base", "auth"}; }

void HrModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("resource.calendar", [db]{ return std::make_shared<ResourceCalendar>(db); });
    models_.registerCreator("hr.department",     [db]{ return std::make_shared<HrDepartment>(db); });
    models_.registerCreator("hr.job",            [db]{ return std::make_shared<HrJob>(db); });
    models_.registerCreator("hr.employee",       [db]{ return std::make_shared<HrEmployee>(db); });
}

void HrModule::registerServices()   {}
void HrModule::registerRoutes()     {}

void HrModule::registerViews() {
    views_.registerView<ResourceCalendarListView>("resource.calendar.list");
    views_.registerView<ResourceCalendarFormView>("resource.calendar.form");
    views_.registerView<HrDepartmentListView>    ("hr.department.list");
    views_.registerView<HrDepartmentFormView>    ("hr.department.form");
    views_.registerView<HrJobListView>           ("hr.job.list");
    views_.registerView<HrJobFormView>           ("hr.job.form");
    views_.registerView<HrEmployeeListView>      ("hr.employee.list");
    views_.registerView<HrEmployeeFormView>      ("hr.employee.form");
}

void HrModule::registerViewModels() {
    auto db = services_.db();
    viewModels_.registerCreator("resource.calendar", [db]{
        return std::make_shared<GenericViewModel<ResourceCalendar>>(db);
    });
    viewModels_.registerCreator("hr.department", [db]{
        return std::make_shared<GenericViewModel<HrDepartment>>(db);
    });
    viewModels_.registerCreator("hr.job", [db]{
        return std::make_shared<GenericViewModel<HrJob>>(db);
    });
    viewModels_.registerCreator("hr.employee", [db]{
        return std::make_shared<GenericViewModel<HrEmployee>>(db);
    });
}

void HrModule::initialize() {
    ensureSchema_();
    seedDefaults_();
    seedMenus_();
}

void HrModule::ensureSchema_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS resource_calendar (
            id            SERIAL PRIMARY KEY,
            name          VARCHAR NOT NULL,
            hours_per_day NUMERIC(4,2) NOT NULL DEFAULT 8.0,
            company_id    INTEGER REFERENCES res_company(id),
            active        BOOLEAN NOT NULL DEFAULT TRUE,
            create_date   TIMESTAMP DEFAULT now(),
            write_date    TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_department (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            parent_id   INTEGER REFERENCES hr_department(id),
            company_id  INTEGER REFERENCES res_company(id),
            active      BOOLEAN NOT NULL DEFAULT TRUE,
            create_date TIMESTAMP DEFAULT now(),
            write_date  TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_job (
            id            SERIAL PRIMARY KEY,
            name          VARCHAR NOT NULL,
            description   TEXT,
            department_id INTEGER REFERENCES hr_department(id),
            company_id    INTEGER REFERENCES res_company(id),
            active        BOOLEAN NOT NULL DEFAULT TRUE,
            create_date   TIMESTAMP DEFAULT now(),
            write_date    TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_employee (
            id                   SERIAL PRIMARY KEY,
            name                 VARCHAR NOT NULL,
            job_id               INTEGER REFERENCES hr_job(id),
            department_id        INTEGER REFERENCES hr_department(id),
            parent_id            INTEGER REFERENCES hr_employee(id),
            coach_id             INTEGER REFERENCES hr_employee(id),
            work_email           VARCHAR,
            work_phone           VARCHAR,
            mobile_phone         VARCHAR,
            resource_calendar_id INTEGER REFERENCES resource_calendar(id),
            company_id           INTEGER REFERENCES res_company(id),
            user_id              INTEGER REFERENCES res_users(id),
            address_id           INTEGER REFERENCES res_partner(id),
            gender               VARCHAR,
            marital              VARCHAR,
            birthday             DATE,
            identification_id    VARCHAR,
            private_email        VARCHAR,
            active               BOOLEAN NOT NULL DEFAULT TRUE,
            create_date          TIMESTAMP DEFAULT now(),
            write_date           TIMESTAMP DEFAULT now()
        )
    )");

    txn.exec(R"(
        ALTER TABLE hr_department
            ADD COLUMN IF NOT EXISTS manager_id INTEGER REFERENCES hr_employee(id)
    )");

    txn.commit();
}

void HrModule::seedDefaults_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        INSERT INTO resource_calendar (id, name, hours_per_day, company_id, active)
        VALUES (1, 'Standard 40 hours/week', 8.0, 1, TRUE)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.commit();
}

void HrModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (13, 'Employees', 'hr.employee', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (14, 'Departments', 'hr.department', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (15, 'Job Positions', 'hr.job', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (16, 'Working Schedules', 'resource.calendar', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO NOTHING
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon)
        VALUES (80, 'Employees', NULL, 60, NULL, 'hr')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (81, 'Employees', 80, 10, 13)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (82, 'Departments', 80, 20, 14)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (83, 'Configuration', 80, 30, NULL)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (84, 'Job Positions', 83, 10, 15)
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (85, 'Working Schedules', 83, 20, 16)
        ON CONFLICT (id) DO NOTHING
    )");

    txn.commit();
}

} // namespace odoo::modules::hr
