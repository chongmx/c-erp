// =============================================================
// modules/hr/HrModule.cpp  — full implementation
// =============================================================
#include "HrModule.hpp"
#include "HrAttendance.hpp"
#include "HrLeave.hpp"
#include "HrKiosk.hpp"
#include "Errors.hpp"
#include "IModule.hpp"
#include "Factories.hpp"
#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "IrSequence.hpp"
#include "Money.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::hr {

using namespace cerp::infrastructure;
using namespace cerp::core;

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
// 2b. EMPLOYEE EXPENSES — hr.expense / hr.expense.sheet
//
// An employee spends their own money (or the company card), records the
// expense, groups expenses into a report, and the report is approved and
// posted to the ledger. Two models, because the approval and the journal
// entry belong to the report, not to each individual receipt.
//
// Malaysian treatment of tax: SST is a single-stage sales/service tax, not a
// VAT — a business cannot reclaim SST it pays on an expense. So tax_amount is
// recorded for the record and for the vendor's invoice trail, but the FULL
// tax-inclusive amount is debited to the expense account. Booking the tax to a
// recoverable input-tax account (the GST habit) would overstate assets and
// understate cost.
// ================================================================

class HrExpense : public BaseModel<HrExpense> {
public:
    ODOO_MODEL("hr.expense", "hr_expense")

    std::string name, date, reference, paymentMode = "own_account", state = "draft";
    int    employeeId = 0, sheetId = 0, productId = 0, accountId = 0, taxId = 0, companyId = 1;
    double quantity = 1.0, unitAmount = 0.0, totalAmount = 0.0, taxAmount = 0.0;

    explicit HrExpense(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",         FieldType::Char,     "Description", true});
        fieldRegistry_.add({"employee_id",  FieldType::Many2one, "Employee", false, false, true, false, "hr.employee"});
        fieldRegistry_.add({"sheet_id",     FieldType::Many2one, "Expense Report", false, false, true, false, "hr.expense.sheet"});
        fieldRegistry_.add({"date",         FieldType::Date,     "Date"});
        fieldRegistry_.add({"product_id",   FieldType::Many2one, "Product", false, false, true, false, "product.product"});
        fieldRegistry_.add({"account_id",   FieldType::Many2one, "Expense Account", false, false, true, false, "account.account"});
        fieldRegistry_.add({"quantity",     FieldType::Float,    "Quantity"});
        fieldRegistry_.add({"unit_amount",  FieldType::Monetary, "Unit Price"});
        fieldRegistry_.add({.name="total_amount", .type=FieldType::Monetary, .string="Total", .readonly=true});
        fieldRegistry_.add({"tax_id",       FieldType::Many2one, "Tax", false, false, true, false, "account.tax"});
        fieldRegistry_.add({.name="tax_amount", .type=FieldType::Monetary, .string="Tax", .readonly=true});
        fieldRegistry_.add({"payment_mode", FieldType::Selection,"Paid By"});
        fieldRegistry_.add({"reference",    FieldType::Char,     "Bill Reference"});
        fieldRegistry_.add({.name="state",  .type=FieldType::Selection, .string="Status", .readonly=true});
        fieldRegistry_.add({"company_id",   FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.markScaled({"quantity", "unit_amount", "total_amount", "tax_amount"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]         = name;
        j["employee_id"]  = employeeId > 0 ? nlohmann::json(employeeId) : nlohmann::json(false);
        j["sheet_id"]     = sheetId    > 0 ? nlohmann::json(sheetId)    : nlohmann::json(false);
        j["date"]         = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["product_id"]   = productId  > 0 ? nlohmann::json(productId)  : nlohmann::json(false);
        j["account_id"]   = accountId  > 0 ? nlohmann::json(accountId)  : nlohmann::json(false);
        j["quantity"]     = quantity;
        j["unit_amount"]  = unitAmount;
        j["total_amount"] = totalAmount;
        j["tax_id"]       = taxId      > 0 ? nlohmann::json(taxId)      : nlohmann::json(false);
        j["tax_amount"]   = taxAmount;
        j["payment_mode"] = paymentMode;
        j["reference"]    = reference.empty() ? nlohmann::json(false) : nlohmann::json(reference);
        j["state"]        = state;
        j["company_id"]   = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")         && j["name"].is_string())         name        = j["name"].get<std::string>();
        if (j.contains("employee_id"))                                   employeeId  = hrM2oId(j["employee_id"]);
        if (j.contains("sheet_id"))                                      sheetId     = hrM2oId(j["sheet_id"]);
        if (j.contains("date")         && j["date"].is_string())         date        = j["date"].get<std::string>();
        if (j.contains("product_id"))                                    productId   = hrM2oId(j["product_id"]);
        if (j.contains("account_id"))                                    accountId   = hrM2oId(j["account_id"]);
        if (j.contains("quantity")     && j["quantity"].is_number())     quantity    = j["quantity"].get<double>();
        if (j.contains("unit_amount")  && j["unit_amount"].is_number())  unitAmount  = j["unit_amount"].get<double>();
        if (j.contains("total_amount") && j["total_amount"].is_number()) totalAmount = j["total_amount"].get<double>();
        if (j.contains("tax_id"))                                        taxId       = hrM2oId(j["tax_id"]);
        if (j.contains("tax_amount")   && j["tax_amount"].is_number())   taxAmount   = j["tax_amount"].get<double>();
        if (j.contains("payment_mode") && j["payment_mode"].is_string()) paymentMode = j["payment_mode"].get<std::string>();
        if (j.contains("reference")    && j["reference"].is_string())    reference   = j["reference"].get<std::string>();
        if (j.contains("state")        && j["state"].is_string())        state       = j["state"].get<std::string>();
        if (j.contains("company_id"))                                    companyId   = hrM2oId(j["company_id"]);
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("A description is required");
        if (paymentMode != "own_account" && paymentMode != "company_account")
            e.push_back("Paid By must be either the employee or the company");
        return e;
    }
};

class HrExpenseSheet : public BaseModel<HrExpenseSheet> {
public:
    ODOO_MODEL("hr.expense.sheet", "hr_expense_sheet")

    std::string name, date, note, paymentMode = "own_account", state = "draft";
    int    employeeId = 0, journalId = 0, moveId = 0, paymentMoveId = 0, companyId = 1;
    double totalAmount = 0.0;

    explicit HrExpenseSheet(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() override {
        fieldRegistry_.add({"name",         FieldType::Char,     "Report Name", true});
        fieldRegistry_.add({"employee_id",  FieldType::Many2one, "Employee", false, false, true, false, "hr.employee"});
        fieldRegistry_.add({"date",         FieldType::Date,     "Date"});
        fieldRegistry_.add({.name="total_amount", .type=FieldType::Monetary, .string="Total", .readonly=true});
        fieldRegistry_.add({"payment_mode", FieldType::Selection,"Paid By"});
        fieldRegistry_.add({.name="state",  .type=FieldType::Selection, .string="Status", .readonly=true});
        fieldRegistry_.add({"journal_id",   FieldType::Many2one, "Journal", false, false, true, false, "account.journal"});
        fieldRegistry_.add({.name="move_id", .type=FieldType::Many2one, .string="Journal Entry", .readonly=true, .relation="account.move"});
        fieldRegistry_.add({.name="payment_move_id", .type=FieldType::Many2one, .string="Payment Entry", .readonly=true, .relation="account.move"});
        fieldRegistry_.add({"note",         FieldType::Text,     "Notes"});
        fieldRegistry_.add({"company_id",   FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.markScaled({"total_amount"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]            = name;
        j["employee_id"]     = employeeId    > 0 ? nlohmann::json(employeeId)    : nlohmann::json(false);
        j["date"]            = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["total_amount"]    = totalAmount;
        j["payment_mode"]    = paymentMode;
        j["state"]           = state;
        j["journal_id"]      = journalId     > 0 ? nlohmann::json(journalId)     : nlohmann::json(false);
        j["move_id"]         = moveId        > 0 ? nlohmann::json(moveId)        : nlohmann::json(false);
        j["payment_move_id"] = paymentMoveId > 0 ? nlohmann::json(paymentMoveId) : nlohmann::json(false);
        j["note"]            = note.empty() ? nlohmann::json(false) : nlohmann::json(note);
        j["company_id"]      = companyId     > 0 ? nlohmann::json(companyId)     : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")         && j["name"].is_string())         name        = j["name"].get<std::string>();
        if (j.contains("employee_id"))                                   employeeId  = hrM2oId(j["employee_id"]);
        if (j.contains("date")         && j["date"].is_string())         date        = j["date"].get<std::string>();
        if (j.contains("total_amount") && j["total_amount"].is_number()) totalAmount = j["total_amount"].get<double>();
        if (j.contains("payment_mode") && j["payment_mode"].is_string()) paymentMode = j["payment_mode"].get<std::string>();
        if (j.contains("state")        && j["state"].is_string())        state       = j["state"].get<std::string>();
        if (j.contains("journal_id"))                                    journalId   = hrM2oId(j["journal_id"]);
        if (j.contains("note")         && j["note"].is_string())         note        = j["note"].get<std::string>();
        if (j.contains("company_id"))                                    companyId   = hrM2oId(j["company_id"]);
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("A report name is required");
        return e;
    }
};

// ----------------------------------------------------------------
// Shared helpers for the expense workflow.
// ----------------------------------------------------------------
namespace {

// total = quantity x unit price, tax computed on top (or extracted, when the
// tax is price-included). Everything is in micros, so the product of two
// micro values must be divided back down once.
void hrRecomputeExpense(pqxx::work& txn, int expenseId) {
    auto r = txn.exec(
        "SELECT e.quantity, e.unit_amount, t.amount AS rate, t.amount_type, t.price_include "
        "FROM hr_expense e LEFT JOIN account_tax t ON t.id = e.tax_id WHERE e.id=$1",
        pqxx::params{expenseId});
    if (r.empty()) return;
    const long long qty  = r[0]["quantity"].as<long long>(0);
    const long long unit = r[0]["unit_amount"].as<long long>(0);
    long long gross = static_cast<long long>(
        (static_cast<__int128>(qty) * unit) / 1000000);

    long long tax = 0;
    if (!r[0]["rate"].is_null()) {
        // account_tax.amount is a plain NUMERIC — a percentage for
        // amount_type='percent', a currency amount for 'fixed'. It is NOT in
        // micros like the money columns are; reading it as one silently
        // multiplies every tax by a million.
        const double rate = r[0]["rate"].as<double>(0.0);
        const std::string type = r[0]["amount_type"].is_null() ? "percent"
                                 : std::string(r[0]["amount_type"].c_str());
        const bool incl = !r[0]["price_include"].is_null() && r[0]["price_include"].as<bool>();
        if (type == "fixed") {
            if (!incl) { tax = std::llround(rate * 1000000.0); gross += tax; }
        } else if (rate != 0.0) {
            if (incl) {
                // gross already contains the tax: extract it.
                const long long net = std::llround(gross / (1.0 + rate / 100.0));
                tax = gross - net;
            } else {
                tax = std::llround(gross * rate / 100.0);
                gross += tax;
            }
        }
    }
    txn.exec("UPDATE hr_expense SET total_amount=$2, tax_amount=$3 WHERE id=$1",
             pqxx::params{expenseId, gross, tax});
}

void hrRecomputeSheet(pqxx::work& txn, int sheetId) {
    if (sheetId <= 0) return;
    txn.exec(
        "UPDATE hr_expense_sheet s SET total_amount = COALESCE("
        "  (SELECT SUM(e.total_amount) FROM hr_expense e "
        "   WHERE e.sheet_id = s.id AND e.state <> 'refused'), 0) "
        "WHERE s.id = $1", pqxx::params{sheetId});
}

// The account the reimbursement is owed from / paid out of.
//   own_account     — the employee is out of pocket: credit a payable.
//   company_account — the company already paid: credit the journal's account.
int hrCreditAccount(pqxx::work& txn, const std::string& paymentMode, int journalId) {
    if (paymentMode == "company_account") {
        if (journalId > 0) {
            auto j = txn.exec("SELECT default_account_id FROM account_journal WHERE id=$1",
                              pqxx::params{journalId});
            if (!j.empty() && !j[0][0].is_null() && j[0][0].as<int>(0) > 0) return j[0][0].as<int>();
        }
        auto c = txn.exec("SELECT id FROM account_account WHERE account_type='asset_cash' ORDER BY id LIMIT 1");
        if (!c.empty()) return c[0][0].as<int>();
    }
    // Configurable, because which payable an employee reimbursement sits in is
    // a chart-of-accounts decision, not something this module should dictate.
    auto p = txn.exec("SELECT value FROM ir_config_parameter WHERE key='hr.expense.payable_account_id'");
    if (!p.empty() && !p[0][0].is_null()) {
        try {
            const int id = std::stoi(p[0][0].c_str());
            if (id > 0 && !txn.exec("SELECT 1 FROM account_account WHERE id=$1", pqxx::params{id}).empty())
                return id;
        } catch (...) {}
    }
    auto a = txn.exec("SELECT id FROM account_account WHERE account_type='liability_payable' ORDER BY id LIMIT 1");
    if (!a.empty()) return a[0][0].as<int>();
    return 0;
}

int hrDefaultJournal(pqxx::work& txn, int journalId) {
    if (journalId > 0) return journalId;
    auto j = txn.exec("SELECT id FROM account_journal WHERE type='purchase' ORDER BY id LIMIT 1");
    if (!j.empty()) return j[0][0].as<int>();
    auto k = txn.exec("SELECT id FROM account_journal ORDER BY id LIMIT 1");
    return k.empty() ? 0 : k[0][0].as<int>();
}

std::string hrNextMoveName(pqxx::work& txn, int journalId) {
    std::string jcode = "MISC";
    auto j = txn.exec("SELECT code FROM account_journal WHERE id=$1", pqxx::params{journalId});
    if (!j.empty() && !j[0][0].is_null()) jcode = j[0][0].c_str();
    const std::string seqCode = "account.move." + jcode;
    txn.exec("INSERT INTO ir_sequence (code,name,prefix,padding,reset_policy) "
             "VALUES ($1,$2,$3,4,'yearly') ON CONFLICT (code) WHERE company_id IS NULL DO NOTHING",
             pqxx::params{seqCode, "Journal — " + jcode, jcode + "/%(year)s/"});
    return core::IrSequence::instance().nextByCode(txn, seqCode);
}

} // anonymous namespace

// hr.expense — CRUD plus the derived totals. An expense on its own has no
// workflow buttons: it is the report that is submitted and approved.
class HrExpenseViewModel : public GenericViewModel<HrExpense> {
public:
    explicit HrExpenseViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HrExpense>(db)
    {
        // Re-register the three write paths over the generic ones (the later
        // registration wins) so total and tax can never drift from
        // quantity x unit price — the figure an approval is based on.
        REGISTER_MUTATOR("create", handleCreateExp)
        REGISTER_MUTATOR("write",  handleWriteExp)
        REGISTER_MUTATOR("unlink", handleUnlinkExp)
    }

    std::string modelName() const override { return "hr.expense"; }

private:
    // Sheets whose stored total must be refreshed after touching `ids`.
    void refresh_(const std::vector<int>& ids, bool recomputeLines) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        std::vector<int> sheets;
        for (int id : ids) {
            auto s = txn.exec("SELECT sheet_id FROM hr_expense WHERE id=$1", pqxx::params{id});
            if (!s.empty() && !s[0][0].is_null()) sheets.push_back(s[0][0].as<int>());
            if (recomputeLines) hrRecomputeExpense(txn, id);
        }
        for (int sid : sheets) hrRecomputeSheet(txn, sid);
        txn.commit();
    }

    nlohmann::json handleCreateExp(const core::CallKwArgs& call) {
        auto res = handleCreate(call);
        if (res.is_number_integer()) refresh_({res.get<int>()}, true);
        return res;
    }
    nlohmann::json handleWriteExp(const core::CallKwArgs& call) {
        auto res = handleWrite(call);
        refresh_(call.ids(), true);
        return res;
    }
    nlohmann::json handleUnlinkExp(const core::CallKwArgs& call) {
        // The sheet ids must be read while the expenses still exist.
        std::vector<int> sheets;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (int id : call.ids()) {
                auto s = txn.exec("SELECT sheet_id FROM hr_expense WHERE id=$1", pqxx::params{id});
                if (!s.empty() && !s[0][0].is_null()) sheets.push_back(s[0][0].as<int>());
            }
        }
        auto res = handleUnlink(call);
        if (!sheets.empty()) {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (int sid : sheets) hrRecomputeSheet(txn, sid);
            txn.commit();
        }
        return res;
    }
};

// hr.employee — plus kiosk PIN management (docs/113 §3a).
//
// The PIN is write-only from the outside: an admin can SET one and CLEAR one,
// and nothing returns it or its hash. `has_pin` is the only readable fact,
// because the screen has to show whether a PIN exists without showing what it
// is.
class HrEmployeeViewModel : public GenericViewModel<HrEmployee> {
public:
    explicit HrEmployeeViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HrEmployee>(std::move(db))
    {
        REGISTER_METHOD("set_pin",   handleSetPin)
        REGISTER_METHOD("clear_pin", handleClearPin)
        REGISTER_METHOD("has_pin",   handleHasPin)
    }
    std::string modelName() const override { return "hr.employee"; }

private:
    nlohmann::json handleSetPin(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) throw ValidationError("set_pin: no employee given.");
        const std::string pin = call.kwargs.value("pin", std::string{});

        // A 4-digit PIN is already a small secret; anything shorter is not one
        // at all. The kiosk's rate limiter is what makes 4 digits defensible,
        // so the floor here and the limiter there are a pair.
        if (pin.size() < 4)
            throw ValidationError("A PIN must be at least 4 digits.");
        if (pin.size() > 32)
            throw ValidationError("That PIN is too long.");
        for (char c : pin)
            if (c < '0' || c > '9')
                throw ValidationError("A PIN must be digits only.");

        const std::string hash = HrKiosk::hashPin(pin);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids)
            txn.exec("UPDATE hr_employee SET pin_hash=$2, write_date=now() WHERE id=$1",
                     pqxx::params{id, hash});
        txn.commit();
        return true;
    }

    nlohmann::json handleClearPin(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) throw ValidationError("clear_pin: no employee given.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids)
            txn.exec("UPDATE hr_employee SET pin_hash=NULL, write_date=now() WHERE id=$1",
                     pqxx::params{id});
        txn.commit();
        return true;
    }

    // Whether a PIN exists — never the PIN, never the hash.
    nlohmann::json handleHasPin(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        if (ids.empty()) throw ValidationError("has_pin: no employee given.");
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        nlohmann::json out = nlohmann::json::object();
        for (int id : ids) {
            auto r = txn.exec("SELECT (pin_hash IS NOT NULL) FROM hr_employee WHERE id=$1",
                              pqxx::params{id});
            out[std::to_string(id)] = !r.empty() && r[0][0].as<bool>(false);
        }
        return out;
    }
};

// hr.expense.sheet — the approval and posting workflow.
class HrExpenseSheetViewModel : public GenericViewModel<HrExpenseSheet> {
public:
    explicit HrExpenseSheetViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HrExpenseSheet>(std::move(db))
    {
        REGISTER_METHOD("action_submit",           handleSubmit)
        REGISTER_METHOD("action_approve",          handleApprove)
        REGISTER_METHOD("action_refuse",           handleRefuse)
        REGISTER_METHOD("action_reset_draft",      handleResetDraft)
        REGISTER_METHOD("action_post",             handlePost)
        REGISTER_METHOD("action_register_payment", handleRegisterPayment)
    }
    std::string modelName() const override { return "hr.expense.sheet"; }

private:


    // Move the sheet and its expenses through one workflow step, refusing any
    // transition that is not legal from the current state. Guarding here rather
    // than in the UI is what stops a stale browser tab approving twice.
    nlohmann::json step_(const core::CallKwArgs& call,
                         const std::vector<std::string>& from,
                         const std::string& to,
                         const std::string& expenseState,
                         const std::string& refusal) {
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            auto s = txn.exec("SELECT state FROM hr_expense_sheet WHERE id=$1", pqxx::params{id});
            if (s.empty()) throw ValidationError("Expense report not found.");
            const std::string cur = s[0][0].c_str();
            if (std::find(from.begin(), from.end(), cur) == from.end())
                throw ValidationError(refusal);
            if (to == "submit") {
                const int n = txn.exec("SELECT count(*) FROM hr_expense WHERE sheet_id=$1",
                                       pqxx::params{id})[0][0].as<int>(0);
                if (n == 0) throw ValidationError("Add at least one expense before submitting this report.");
            }
            hrRecomputeSheet(txn, id);
            txn.exec("UPDATE hr_expense_sheet SET state=$2, write_date=now() WHERE id=$1",
                     pqxx::params{id, to});
            txn.exec("UPDATE hr_expense SET state=$2, write_date=now() WHERE sheet_id=$1",
                     pqxx::params{id, expenseState});
        }
        txn.commit();
        return true;
    }

    nlohmann::json handleSubmit(const core::CallKwArgs& call) {
        return step_(call, {"draft"}, "submit", "reported",
                     "Only a draft expense report can be submitted.");
    }
    nlohmann::json handleApprove(const core::CallKwArgs& call) {
        return step_(call, {"submit"}, "approve", "approved",
                     "Only a submitted expense report can be approved.");
    }
    nlohmann::json handleRefuse(const core::CallKwArgs& call) {
        return step_(call, {"submit", "approve"}, "cancel", "refused",
                     "Only a submitted or approved report can be refused.");
    }
    nlohmann::json handleResetDraft(const core::CallKwArgs& call) {
        // A posted report is not reopened here: its journal entry is already in
        // the ledger, and unwinding that belongs to a reversal, not a button.
        return step_(call, {"submit", "cancel"}, "draft", "draft",
                     "A posted expense report cannot be reset to draft.");
    }

    // Post the report: Dr each expense account, Cr the payable (employee to be
    // reimbursed) or the company's cash/bank account (company already paid).
    nlohmann::json handlePost(const core::CallKwArgs& call) {
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            auto s = txn.exec(
                "SELECT state, name, employee_id, journal_id, company_id, payment_mode, "
                "       to_char(COALESCE(date, CURRENT_DATE),'YYYY-MM-DD') AS dt "
                "FROM hr_expense_sheet WHERE id=$1", pqxx::params{id});
            if (s.empty()) throw ValidationError("Expense report not found.");
            if (std::string(s[0]["state"].c_str()) != "approve")
                throw ValidationError("Approve the expense report before posting it.");

            const std::string sname = s[0]["name"].c_str();
            const std::string dt    = s[0]["dt"].c_str();
            const std::string mode  = s[0]["payment_mode"].is_null() ? "own_account"
                                      : std::string(s[0]["payment_mode"].c_str());
            const int companyId = s[0]["company_id"].is_null() ? 1 : s[0]["company_id"].as<int>();
            const int journalId = hrDefaultJournal(txn, s[0]["journal_id"].is_null() ? 0 : s[0]["journal_id"].as<int>());
            if (journalId <= 0) throw ValidationError("No accounting journal is configured.");

            hrRecomputeSheet(txn, id);
            auto lines = txn.exec(
                "SELECT e.id, e.name, e.total_amount, e.account_id "
                "FROM hr_expense e WHERE e.sheet_id=$1 AND e.state <> 'refused' ORDER BY e.id",
                pqxx::params{id});
            if (lines.empty()) throw ValidationError("This expense report has nothing to post.");

            const int credit = hrCreditAccount(txn, mode, journalId);
            if (credit <= 0)
                throw ValidationError("No payable account is configured for employee reimbursements.");
            // Fallback expense account, used only when a line has none set.
            int fallback = 0;
            { auto f = txn.exec("SELECT id FROM account_account WHERE account_type='expense' ORDER BY id LIMIT 1");
              if (!f.empty()) fallback = f[0][0].as<int>(); }

            long long total = 0;
            for (const auto& l : lines) total += l["total_amount"].as<long long>(0);
            if (total <= 0) throw ValidationError("An expense report must total more than zero to be posted.");

            const std::string num = hrNextMoveName(txn, journalId);
            const int moveId = txn.exec(
                "INSERT INTO account_move (name, move_type, state, date, journal_id, company_id, "
                " amount_untaxed, amount_tax, amount_total, amount_residual, ref) "
                "VALUES ($1,'entry','posted',$2::date,$3,$4,$5,0,$5,0,$6) RETURNING id",
                pqxx::params{num, dt, journalId, companyId, total,
                             "Expenses — " + sname})[0][0].as<int>();

            for (const auto& l : lines) {
                const long long amt = l["total_amount"].as<long long>(0);
                if (amt == 0) continue;
                int acc = l["account_id"].is_null() ? 0 : l["account_id"].as<int>();
                if (acc <= 0) acc = fallback;
                if (acc <= 0) throw ValidationError("Set an expense account on every expense line first.");
                txn.exec(
                    "INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, date, name, debit, credit) "
                    "VALUES ($1,$2,$3,$4,$5::date,$6,$7,0)",
                    pqxx::params{moveId, acc, journalId, companyId, dt,
                                 std::string(l["name"].c_str()), amt});
            }
            txn.exec(
                "INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, date, name, debit, credit) "
                "VALUES ($1,$2,$3,$4,$5::date,$6,0,$7)",
                pqxx::params{moveId, credit, journalId, companyId, dt,
                             "Expenses — " + sname, total});

            txn.exec("UPDATE hr_expense_sheet SET state='post', move_id=$2, journal_id=$3 WHERE id=$1",
                     pqxx::params{id, moveId, journalId});
            txn.exec("UPDATE hr_expense SET state='done' WHERE sheet_id=$1 AND state <> 'refused'",
                     pqxx::params{id});
        }
        txn.commit();
        if (AuditService::ready())
            AuditService::instance().log("hr.expense.sheet", "action_post", ids, extractContext_(call).uid);
        return true;
    }

    // Reimburse the employee: Dr the payable, Cr bank/cash. Only meaningful for
    // own_account — a company-paid expense was already settled when it was
    // posted, so there is nothing left to pay.
    nlohmann::json handleRegisterPayment(const core::CallKwArgs& call) {
        int payJournal = 0;
        if (call.kwargs.contains("journal_id")) payJournal = hrM2oId(call.kwargs["journal_id"]);
        const auto ids = call.ids();
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            auto s = txn.exec(
                "SELECT state, name, company_id, payment_mode, total_amount, journal_id, "
                "       to_char(CURRENT_DATE,'YYYY-MM-DD') AS dt "
                "FROM hr_expense_sheet WHERE id=$1", pqxx::params{id});
            if (s.empty()) throw ValidationError("Expense report not found.");
            if (std::string(s[0]["state"].c_str()) != "post")
                throw ValidationError("Post the expense report before registering its payment.");
            const std::string mode = s[0]["payment_mode"].is_null() ? "own_account"
                                     : std::string(s[0]["payment_mode"].c_str());
            if (mode == "company_account")
                throw ValidationError("This report was paid by the company — there is nothing to reimburse.");

            const std::string sname = s[0]["name"].c_str();
            const std::string dt    = s[0]["dt"].c_str();
            const int companyId = s[0]["company_id"].is_null() ? 1 : s[0]["company_id"].as<int>();
            const long long total = s[0]["total_amount"].as<long long>(0);
            if (total <= 0) throw ValidationError("Nothing to pay on this report.");

            int jrn = payJournal;
            if (jrn <= 0) {
                auto b = txn.exec("SELECT id FROM account_journal WHERE type IN ('bank','cash') ORDER BY id LIMIT 1");
                if (b.empty()) throw ValidationError("No bank or cash journal is configured.");
                jrn = b[0][0].as<int>();
            }
            auto ja = txn.exec("SELECT default_account_id FROM account_journal WHERE id=$1", pqxx::params{jrn});
            int cashAcc = (!ja.empty() && !ja[0][0].is_null()) ? ja[0][0].as<int>(0) : 0;
            if (cashAcc <= 0) {
                auto c = txn.exec("SELECT id FROM account_account WHERE account_type='asset_cash' ORDER BY id LIMIT 1");
                if (c.empty()) throw ValidationError("No cash or bank account is configured.");
                cashAcc = c[0][0].as<int>();
            }
            const int payable = hrCreditAccount(txn, "own_account", 0);
            if (payable <= 0) throw ValidationError("No payable account is configured for employee reimbursements.");

            const std::string num = hrNextMoveName(txn, jrn);
            const int moveId = txn.exec(
                "INSERT INTO account_move (name, move_type, state, date, journal_id, company_id, "
                " amount_untaxed, amount_tax, amount_total, amount_residual, ref) "
                "VALUES ($1,'entry','posted',$2::date,$3,$4,$5,0,$5,0,$6) RETURNING id",
                pqxx::params{num, dt, jrn, companyId, total,
                             "Expense reimbursement — " + sname})[0][0].as<int>();
            txn.exec(
                "INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, date, name, debit, credit) "
                "VALUES ($1,$2,$3,$4,$5::date,$6,$7,0), ($1,$8,$3,$4,$5::date,$6,0,$7)",
                pqxx::params{moveId, payable, jrn, companyId, dt,
                             "Expense reimbursement — " + sname, total, cashAcc});
            txn.exec("UPDATE hr_expense_sheet SET state='done', payment_move_id=$2 WHERE id=$1",
                     pqxx::params{id, moveId});
        }
        txn.commit();
        if (AuditService::ready())
            AuditService::instance().log("hr.expense.sheet", "action_register_payment", ids, extractContext_(call).uid);
        return true;
    }
};

class HrExpenseListView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.expense.list"; }
    std::string modelName() const override { return "hr.expense"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Employee Expenses\"><field name=\"date\"/><field name=\"name\"/>"
               "<field name=\"employee_id\"/><field name=\"total_amount\"/>"
               "<field name=\"payment_mode\"/><field name=\"sheet_id\"/><field name=\"state\"/></list>";
    }
    nlohmann::json fields() const override { return expenseFields(); }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }

    static nlohmann::json expenseFields() {
        return {
            {"name",         {{"type","char"},     {"string","Description"}}},
            {"employee_id",  {{"type","many2one"}, {"string","Employee"},       {"relation","hr.employee"}}},
            {"sheet_id",     {{"type","many2one"}, {"string","Expense Report"}, {"relation","hr.expense.sheet"}}},
            {"date",         {{"type","date"},     {"string","Date"}}},
            {"product_id",   {{"type","many2one"}, {"string","Product"},        {"relation","product.product"}}},
            {"account_id",   {{"type","many2one"}, {"string","Expense Account"},{"relation","account.account"}}},
            {"quantity",     {{"type","float"},    {"string","Quantity"}}},
            {"unit_amount",  {{"type","monetary"}, {"string","Unit Price"}}},
            {"total_amount", {{"type","monetary"}, {"string","Total"}}},
            {"tax_id",       {{"type","many2one"}, {"string","Tax"},            {"relation","account.tax"}}},
            {"tax_amount",   {{"type","monetary"}, {"string","Tax"}}},
            {"payment_mode", {{"type","selection"},{"string","Paid By"},
                              {"selection", nlohmann::json::array({
                                  nlohmann::json::array({"own_account","Employee (to reimburse)"}),
                                  nlohmann::json::array({"company_account","Company"})})}}},
            {"reference",    {{"type","char"},     {"string","Bill Reference"}}},
            {"state",        {{"type","selection"},{"string","Status"},
                              {"selection", nlohmann::json::array({
                                  nlohmann::json::array({"draft","To Report"}),
                                  nlohmann::json::array({"reported","Submitted"}),
                                  nlohmann::json::array({"approved","Approved"}),
                                  nlohmann::json::array({"done","Posted"}),
                                  nlohmann::json::array({"refused","Refused"})})}}},
            {"company_id",   {{"type","many2one"}, {"string","Company"},        {"relation","res.company"}}},
        };
    }
};

class HrExpenseFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.expense.form"; }
    std::string modelName() const override { return "hr.expense"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Expense\"><field name=\"name\"/><field name=\"employee_id\"/>"
               "<field name=\"date\"/><field name=\"product_id\"/><field name=\"account_id\"/>"
               "<field name=\"quantity\"/><field name=\"unit_amount\"/><field name=\"tax_id\"/>"
               "<field name=\"total_amount\"/><field name=\"payment_mode\"/>"
               "<field name=\"reference\"/><field name=\"sheet_id\"/><field name=\"state\"/></form>";
    }
    nlohmann::json fields() const override { return HrExpenseListView::expenseFields(); }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }
};

class HrExpenseSheetListView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.expense.sheet.list"; }
    std::string modelName() const override { return "hr.expense.sheet"; }
    std::string viewType()  const override { return "list"; }
    std::string arch() const override {
        return "<list string=\"Expense Reports\"><field name=\"name\"/><field name=\"employee_id\"/>"
               "<field name=\"date\"/><field name=\"total_amount\"/>"
               "<field name=\"payment_mode\"/><field name=\"state\"/></list>";
    }
    nlohmann::json fields() const override { return sheetFields(); }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }

    static nlohmann::json sheetFields() {
        return {
            {"name",            {{"type","char"},     {"string","Report Name"}}},
            {"employee_id",     {{"type","many2one"}, {"string","Employee"},      {"relation","hr.employee"}}},
            {"date",            {{"type","date"},     {"string","Date"}}},
            {"total_amount",    {{"type","monetary"}, {"string","Total"}}},
            {"payment_mode",    {{"type","selection"},{"string","Paid By"},
                                 {"selection", nlohmann::json::array({
                                     nlohmann::json::array({"own_account","Employee (to reimburse)"}),
                                     nlohmann::json::array({"company_account","Company"})})}}},
            {"state",           {{"type","selection"},{"string","Status"},
                                 {"selection", nlohmann::json::array({
                                     nlohmann::json::array({"draft","Draft"}),
                                     nlohmann::json::array({"submit","Submitted"}),
                                     nlohmann::json::array({"approve","Approved"}),
                                     nlohmann::json::array({"post","Posted"}),
                                     nlohmann::json::array({"done","Paid"}),
                                     nlohmann::json::array({"cancel","Refused"})})}}},
            {"journal_id",      {{"type","many2one"}, {"string","Journal"},       {"relation","account.journal"}}},
            {"move_id",         {{"type","many2one"}, {"string","Journal Entry"}, {"relation","account.move"}}},
            {"payment_move_id", {{"type","many2one"}, {"string","Payment Entry"}, {"relation","account.move"}}},
            {"note",            {{"type","text"},     {"string","Notes"}}},
            {"company_id",      {{"type","many2one"}, {"string","Company"},       {"relation","res.company"}}},
        };
    }
};

class HrExpenseSheetFormView : public core::BaseView {
public:
    std::string viewName()  const override { return "hr.expense.sheet.form"; }
    std::string modelName() const override { return "hr.expense.sheet"; }
    std::string viewType()  const override { return "form"; }
    std::string arch() const override {
        return "<form string=\"Expense Report\"><field name=\"name\"/><field name=\"employee_id\"/>"
               "<field name=\"date\"/><field name=\"payment_mode\"/><field name=\"journal_id\"/>"
               "<field name=\"total_amount\"/><field name=\"state\"/><field name=\"move_id\"/>"
               "<field name=\"payment_move_id\"/><field name=\"note\"/></form>";
    }
    nlohmann::json fields() const override { return HrExpenseSheetListView::sheetFields(); }
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
// "account" is a real dependency now: expense reports post journal entries, so
// the chart of accounts and the journals must exist before this module's
// schema references them.
std::vector<std::string> HrModule::dependencies() const { return {"base", "auth", "account"}; }

void HrModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("resource.calendar", [db]{ return std::make_shared<ResourceCalendar>(db); });
    models_.registerCreator("hr.department",     [db]{ return std::make_shared<HrDepartment>(db); });
    models_.registerCreator("hr.job",            [db]{ return std::make_shared<HrJob>(db); });
    models_.registerCreator("hr.employee",       [db]{ return std::make_shared<HrEmployee>(db); });
    models_.registerCreator("hr.expense",        [db]{ return std::make_shared<HrExpense>(db); });
    models_.registerCreator("hr.expense.sheet",  [db]{ return std::make_shared<HrExpenseSheet>(db); });
    // Attendance and time off live in their own translation units (docs/113).
    HrAttendance::registerModels(models_, db);
    HrLeave::registerModels(models_, db);
}

void HrModule::registerServices()   {}

void HrModule::registerRoutes() {
    // The staff kiosk (docs/113 §3a) — the only unauthenticated surface this
    // module exposes, and deliberately capable of exactly one action.
    HrKiosk::registerRoutes(services_.db(), services_.devMode(),
                            services_.trustedProxies());
}

void HrModule::registerViews() {
    views_.registerView<ResourceCalendarListView>("resource.calendar.list");
    views_.registerView<ResourceCalendarFormView>("resource.calendar.form");
    views_.registerView<HrDepartmentListView>    ("hr.department.list");
    views_.registerView<HrDepartmentFormView>    ("hr.department.form");
    views_.registerView<HrJobListView>           ("hr.job.list");
    views_.registerView<HrJobFormView>           ("hr.job.form");
    views_.registerView<HrEmployeeListView>      ("hr.employee.list");
    views_.registerView<HrEmployeeFormView>      ("hr.employee.form");
    views_.registerView<HrExpenseListView>       ("hr.expense.list");
    views_.registerView<HrExpenseFormView>       ("hr.expense.form");
    views_.registerView<HrExpenseSheetListView>  ("hr.expense.sheet.list");
    views_.registerView<HrExpenseSheetFormView>  ("hr.expense.sheet.form");
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
        return std::make_shared<HrEmployeeViewModel>(db);
    });
    viewModels_.registerCreator("hr.expense", [db]{
        return std::make_shared<HrExpenseViewModel>(db);
    });
    viewModels_.registerCreator("hr.expense.sheet", [db]{
        return std::make_shared<HrExpenseSheetViewModel>(db);
    });
    HrAttendance::registerViewModels(viewModels_, db);
    HrLeave::registerViewModels(viewModels_, db);
}

void HrModule::initialize() {
    ensureSchema_();
    seedDefaults_();
    seedMenus_();

    // Attendance and time off. One transaction each so a failure in one does
    // not leave the other half-created; all three steps are idempotent.
    {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        HrAttendance::ensureSchema(txn);
        HrLeave::ensureSchema(txn);
        HrLeave::seedDefaults(txn);
        HrKiosk::ensureSchema(txn);
        HrAttendance::seedMenus(txn);
        HrLeave::seedMenus(txn);
        txn.commit();
    }
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

    // ---- Employee expenses -------------------------------------------
    // Money columns are BIGINT micros, like every other monetary column in the
    // schema. Nothing here is NUMERIC: mixing the two representations is how a
    // total silently loses its last two decimals.
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_expense_sheet (
            id              SERIAL PRIMARY KEY,
            name            VARCHAR NOT NULL,
            employee_id     INTEGER REFERENCES hr_employee(id),
            date            DATE    DEFAULT CURRENT_DATE,
            total_amount    BIGINT  NOT NULL DEFAULT 0,
            payment_mode    VARCHAR NOT NULL DEFAULT 'own_account',
            state           VARCHAR NOT NULL DEFAULT 'draft',
            journal_id      INTEGER REFERENCES account_journal(id) ON DELETE SET NULL,
            move_id         INTEGER REFERENCES account_move(id)    ON DELETE SET NULL,
            payment_move_id INTEGER REFERENCES account_move(id)    ON DELETE SET NULL,
            note            TEXT,
            company_id      INTEGER REFERENCES res_company(id),
            create_date     TIMESTAMP DEFAULT now(),
            write_date      TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_expense (
            id           SERIAL PRIMARY KEY,
            name         VARCHAR NOT NULL,
            employee_id  INTEGER REFERENCES hr_employee(id),
            sheet_id     INTEGER REFERENCES hr_expense_sheet(id) ON DELETE SET NULL,
            date         DATE    DEFAULT CURRENT_DATE,
            product_id   INTEGER REFERENCES product_product(id)  ON DELETE SET NULL,
            account_id   INTEGER REFERENCES account_account(id)  ON DELETE SET NULL,
            quantity     BIGINT  NOT NULL DEFAULT 1000000,
            unit_amount  BIGINT  NOT NULL DEFAULT 0,
            total_amount BIGINT  NOT NULL DEFAULT 0,
            tax_id       INTEGER REFERENCES account_tax(id)      ON DELETE SET NULL,
            tax_amount   BIGINT  NOT NULL DEFAULT 0,
            payment_mode VARCHAR NOT NULL DEFAULT 'own_account',
            reference    VARCHAR,
            state        VARCHAR NOT NULL DEFAULT 'draft',
            company_id   INTEGER REFERENCES res_company(id),
            create_date  TIMESTAMP DEFAULT now(),
            write_date   TIMESTAMP DEFAULT now()
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS hr_expense_sheet_idx ON hr_expense(sheet_id)");
    txn.exec("CREATE INDEX IF NOT EXISTS hr_expense_employee_idx ON hr_expense(employee_id)");

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

    // ids 13/15 collided with ProductModule's part.unit / part.search (seeded
    // with ON CONFLICT DO UPDATE), which clobbered the Employees and Job
    // Positions actions. Use unique ids (50/51) with DO UPDATE so existing
    // databases self-heal. Departments (14) and Working Schedules (16) never
    // collided, so they keep their ids.
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (50, 'Employees', 'hr.employee', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (14, 'Departments', 'hr.department', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO NOTHING
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (51, 'Job Positions', 'hr.job', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode
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
        VALUES (81, 'Employees', 80, 10, 50)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
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
        VALUES (84, 'Job Positions', 83, 10, 51)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (85, 'Working Schedules', 83, 20, 16)
        ON CONFLICT (id) DO NOTHING
    )");

    // ---- Employee expenses -------------------------------------------
    // act_window 97/98 and ir_ui_menu 67/68 were free at the time of writing
    // (highest allocated: act 96, menu 66 within the low block). Before adding
    // another, run scripts/check_menu_ids.sh — reusing an id silently hijacks
    // someone else's menu, and seeding on top of an app root deletes a whole
    // app from the home screen (docs/089).
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (97, 'Employee Expenses', 'hr.expense', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (98, 'Expense Reports', 'hr.expense.sheet', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode
    )");
    // The employee records expenses in the Employees app...
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (68, 'Employee Expenses', 80, 25, 97)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    // ...and the accountant approves and posts the reports from Accounting,
    // where the journal entry they produce belongs.
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (67, 'Expense Reports', 10, 34, 98)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");

    txn.commit();
}

} // namespace cerp::modules::hr
