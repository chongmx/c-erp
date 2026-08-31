// =============================================================
// modules/hr/HrLeave.cpp — implementation (docs/113 §2)
// =============================================================
#include "HrLeave.hpp"
#include "BaseModel.hpp"
#include "GenericViewModel.hpp"
#include "Factories.hpp"
#include "DbConnection.hpp"
#include "Errors.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::hr {

using namespace cerp::infrastructure;
using namespace cerp::core;

namespace {
inline int lvM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer())
        return v[0].get<int>();
    return 0;
}
} // anonymous namespace

// ================================================================
// The day counter — the calculation everything else depends on.
//
// Done in SQL rather than C++ because PostgreSQL owns the calendar: it parses
// the dates, knows what a weekend is via EXTRACT(ISODOW), and can anti-join the
// public-holiday table in the same pass. Doing it here would mean
// reimplementing date arithmetic and then disagreeing with the database about
// what "2026-03-01" means.
// ================================================================
double HrLeave::workingDays(pqxx::transaction_base& txn,
                            const std::string& dateFrom,
                            const std::string& dateTo,
                            int companyId)
{
    if (dateFrom.empty() || dateTo.empty()) return 0.0;
    auto r = txn.exec(
        "SELECT COUNT(*)::numeric FROM generate_series($1::date, $2::date, '1 day') AS d "
        " WHERE EXTRACT(ISODOW FROM d) < 6 "
        "   AND NOT EXISTS (SELECT 1 FROM hr_public_holiday h "
        "                    WHERE h.date = d::date "
        "                      AND (h.company_id = $3 OR h.company_id IS NULL))",
        pqxx::params{dateFrom, dateTo, companyId});
    return r.empty() ? 0.0 : r[0][0].as<double>(0.0);
}

namespace {

// Approved allocation, approved leave, and pending leave for one employee+type.
struct Balance { double allocated = 0, taken = 0, pending = 0; };

Balance balanceOf(pqxx::transaction_base& txn, int employeeId, int typeId) {
    Balance b;
    auto a = txn.exec(
        "SELECT COALESCE(SUM(number_of_days),0)::numeric FROM hr_leave_allocation "
        " WHERE employee_id=$1 AND leave_type_id=$2 AND state='validate'",
        pqxx::params{employeeId, typeId});
    b.allocated = a.empty() ? 0.0 : a[0][0].as<double>(0.0);

    auto t = txn.exec(
        "SELECT COALESCE(SUM(number_of_days),0)::numeric FROM hr_leave "
        " WHERE employee_id=$1 AND leave_type_id=$2 AND state='validate'",
        pqxx::params{employeeId, typeId});
    b.taken = t.empty() ? 0.0 : t[0][0].as<double>(0.0);

    auto p = txn.exec(
        "SELECT COALESCE(SUM(number_of_days),0)::numeric FROM hr_leave "
        " WHERE employee_id=$1 AND leave_type_id=$2 AND state='confirm'",
        pqxx::params{employeeId, typeId});
    b.pending = p.empty() ? 0.0 : p[0][0].as<double>(0.0);
    return b;
}

} // anonymous namespace

// ================================================================
// MODELS
// ================================================================
class HrLeaveType : public BaseModel<HrLeaveType> {
public:
    static constexpr const char* MODEL_NAME = "hr.leave.type";
    static constexpr const char* TABLE_NAME = "hr_leave_type";
    explicit HrLeaveType(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string name, code, color;
    bool requiresAllocation = true;
    bool isPaid             = true;
    int  maxDaysPerRequest  = 0;
    int  companyId          = 1;
    bool active             = true;

    void registerFields() {
        fieldRegistry_.add({"name",                 FieldType::Char,     "Leave Type", true});
        fieldRegistry_.add({"code",                 FieldType::Char,     "Code"});
        fieldRegistry_.add({"requires_allocation",  FieldType::Boolean,  "Requires Allocation"});
        fieldRegistry_.add({"is_paid",              FieldType::Boolean,  "Paid"});
        fieldRegistry_.add({"max_days_per_request", FieldType::Integer,  "Max Days / Request"});
        fieldRegistry_.add({"color",                FieldType::Char,     "Colour"});
        fieldRegistry_.add({"company_id",           FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",               FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;
        j["code"] = code.empty() ? nlohmann::json(false) : nlohmann::json(code);
        j["requires_allocation"]  = requiresAllocation;
        j["is_paid"]              = isPaid;
        j["max_days_per_request"] = maxDaysPerRequest;
        j["color"]      = color.empty() ? nlohmann::json(false) : nlohmann::json(color);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]     = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("code") && j["code"].is_string()) code = j["code"].get<std::string>();
        if (j.contains("requires_allocation") && j["requires_allocation"].is_boolean())
            requiresAllocation = j["requires_allocation"].get<bool>();
        if (j.contains("is_paid") && j["is_paid"].is_boolean()) isPaid = j["is_paid"].get<bool>();
        if (j.contains("max_days_per_request") && j["max_days_per_request"].is_number_integer())
            maxDaysPerRequest = j["max_days_per_request"].get<int>();
        if (j.contains("color") && j["color"].is_string()) color = j["color"].get<std::string>();
        if (j.contains("company_id")) companyId = lvM2oId(j["company_id"]);
        if (j.contains("active") && j["active"].is_boolean()) active = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Leave type name is required");
        return e;
    }
};

class HrPublicHoliday : public BaseModel<HrPublicHoliday> {
public:
    static constexpr const char* MODEL_NAME = "hr.public.holiday";
    static constexpr const char* TABLE_NAME = "hr_public_holiday";
    explicit HrPublicHoliday(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    std::string name, date;
    int companyId = 1;

    void registerFields() {
        fieldRegistry_.add({"name",       FieldType::Char,     "Holiday", true});
        fieldRegistry_.add({"date",       FieldType::Date,     "Date",    true});
        fieldRegistry_.add({"company_id", FieldType::Many2one, "Company", false, false, true, false, "res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"] = name;
        j["date"] = date.empty() ? nlohmann::json(false) : nlohmann::json(date);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
        if (j.contains("date") && j["date"].is_string()) date = j["date"].get<std::string>();
        if (j.contains("company_id")) companyId = lvM2oId(j["company_id"]);
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Holiday name is required");
        if (date.empty()) e.push_back("A date is required");
        return e;
    }
};

class HrLeaveAllocation : public BaseModel<HrLeaveAllocation> {
public:
    static constexpr const char* MODEL_NAME = "hr.leave.allocation";
    static constexpr const char* TABLE_NAME = "hr_leave_allocation";
    explicit HrLeaveAllocation(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    int         employeeId  = 0;
    int         leaveTypeId = 0;
    double      numberOfDays = 0;
    std::string dateFrom, dateTo, notes;
    std::string state = "draft";
    int         companyId = 1;

    void registerFields() {
        fieldRegistry_.add({"employee_id",    FieldType::Many2one, "Employee", true, false, true, false, "hr.employee"});
        fieldRegistry_.add({"leave_type_id",  FieldType::Many2one, "Leave Type", true, false, true, false, "hr.leave.type"});
        fieldRegistry_.add({"number_of_days", FieldType::Float,    "Days", true});
        fieldRegistry_.add({"date_from",      FieldType::Date,     "Valid From"});
        fieldRegistry_.add({"date_to",        FieldType::Date,     "Valid To"});
        fieldRegistry_.add({"state",          FieldType::Selection,"Status"});
        fieldRegistry_.add({"notes",          FieldType::Text,     "Notes"});
        fieldRegistry_.add({"company_id",     FieldType::Many2one, "Company", false, false, true, false, "res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["employee_id"]    = employeeId  > 0 ? nlohmann::json::array({employeeId, ""})  : nlohmann::json(false);
        j["leave_type_id"]  = leaveTypeId > 0 ? nlohmann::json::array({leaveTypeId, ""}) : nlohmann::json(false);
        j["number_of_days"] = numberOfDays;
        j["date_from"] = dateFrom.empty() ? nlohmann::json(false) : nlohmann::json(dateFrom);
        j["date_to"]   = dateTo.empty()   ? nlohmann::json(false) : nlohmann::json(dateTo);
        j["state"]     = state;
        j["notes"]     = notes.empty() ? nlohmann::json(false) : nlohmann::json(notes);
        j["company_id"]= companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("employee_id"))   employeeId  = lvM2oId(j["employee_id"]);
        if (j.contains("leave_type_id")) leaveTypeId = lvM2oId(j["leave_type_id"]);
        if (j.contains("number_of_days") && j["number_of_days"].is_number())
            numberOfDays = j["number_of_days"].get<double>();
        if (j.contains("date_from") && j["date_from"].is_string()) dateFrom = j["date_from"].get<std::string>();
        if (j.contains("date_to")   && j["date_to"].is_string())   dateTo   = j["date_to"].get<std::string>();
        if (j.contains("notes")     && j["notes"].is_string())     notes    = j["notes"].get<std::string>();
        if (j.contains("company_id")) companyId = lvM2oId(j["company_id"]);
        // state is moved by the workflow actions only, never by a plain write.
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = std::to_string(numberOfDays) + " days"; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (employeeId  <= 0) e.push_back("An employee is required");
        if (leaveTypeId <= 0) e.push_back("A leave type is required");
        if (numberOfDays <= 0) e.push_back("Allocated days must be greater than zero");
        return e;
    }
};

class HrLeaveReq : public BaseModel<HrLeaveReq> {
public:
    static constexpr const char* MODEL_NAME = "hr.leave";
    static constexpr const char* TABLE_NAME = "hr_leave";
    explicit HrLeaveReq(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    int         employeeId  = 0;
    int         leaveTypeId = 0;
    std::string dateFrom, dateTo, reason;
    double      numberOfDays = 0;
    std::string state = "draft";
    int         companyId = 1;

    void registerFields() {
        fieldRegistry_.add({"employee_id",    FieldType::Many2one, "Employee", true, false, true, false, "hr.employee"});
        fieldRegistry_.add({"leave_type_id",  FieldType::Many2one, "Leave Type", true, false, true, false, "hr.leave.type"});
        fieldRegistry_.add({"date_from",      FieldType::Date,     "From", true});
        fieldRegistry_.add({"date_to",        FieldType::Date,     "To",   true});
        // Derived from the dates on every write. Registered so it can be read
        // and filtered; a client-supplied value is discarded.
        fieldRegistry_.add({"number_of_days", FieldType::Float,    "Days"});
        fieldRegistry_.add({"state",          FieldType::Selection,"Status"});
        fieldRegistry_.add({"reason",         FieldType::Text,     "Reason"});
        fieldRegistry_.add({"company_id",     FieldType::Many2one, "Company", false, false, true, false, "res.company"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["employee_id"]   = employeeId  > 0 ? nlohmann::json::array({employeeId, ""})  : nlohmann::json(false);
        j["leave_type_id"] = leaveTypeId > 0 ? nlohmann::json::array({leaveTypeId, ""}) : nlohmann::json(false);
        j["date_from"] = dateFrom.empty() ? nlohmann::json(false) : nlohmann::json(dateFrom);
        j["date_to"]   = dateTo.empty()   ? nlohmann::json(false) : nlohmann::json(dateTo);
        j["number_of_days"] = numberOfDays;
        j["state"]  = state;
        j["reason"] = reason.empty() ? nlohmann::json(false) : nlohmann::json(reason);
        j["company_id"] = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("employee_id"))   employeeId  = lvM2oId(j["employee_id"]);
        if (j.contains("leave_type_id")) leaveTypeId = lvM2oId(j["leave_type_id"]);
        if (j.contains("date_from") && j["date_from"].is_string()) dateFrom = j["date_from"].get<std::string>();
        if (j.contains("date_to")   && j["date_to"].is_string())   dateTo   = j["date_to"].get<std::string>();
        if (j.contains("reason")    && j["reason"].is_string())    reason   = j["reason"].get<std::string>();
        if (j.contains("company_id")) companyId = lvM2oId(j["company_id"]);
        // number_of_days and state are derived / workflow-owned.
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = dateFrom + " → " + dateTo; return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (employeeId  <= 0) e.push_back("An employee is required");
        if (leaveTypeId <= 0) e.push_back("A leave type is required");
        if (dateFrom.empty() || dateTo.empty()) e.push_back("Both dates are required");
        if (!dateFrom.empty() && !dateTo.empty() && dateTo < dateFrom)
            e.push_back("The end date cannot be before the start date");
        return e;
    }
};

// ================================================================
// VIEWMODELS
// ================================================================

// Allocations: draft -> confirm -> validate | refuse.
class HrLeaveAllocationViewModel : public GenericViewModel<HrLeaveAllocation> {
public:
    explicit HrLeaveAllocationViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HrLeaveAllocation>(std::move(db))
    {
        REGISTER_METHOD("action_confirm", handleConfirm)
        REGISTER_METHOD("action_approve", handleApprove)
        REGISTER_METHOD("action_refuse",  handleRefuse)
    }
    std::string modelName() const override { return "hr.leave.allocation"; }

private:
    nlohmann::json step_(const core::CallKwArgs& call,
                         const std::vector<std::string>& from,
                         const std::string& to,
                         const std::string& refusal) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : call.ids()) {
            auto s = txn.exec("SELECT state FROM hr_leave_allocation WHERE id=$1",
                              pqxx::params{id});
            if (s.empty()) throw ValidationError("Allocation not found.");
            const std::string cur = s[0][0].c_str();
            if (std::find(from.begin(), from.end(), cur) == from.end())
                throw ValidationError(refusal);
            txn.exec("UPDATE hr_leave_allocation SET state=$2, write_date=now() WHERE id=$1",
                     pqxx::params{id, to});
        }
        txn.commit();
        return true;
    }
    nlohmann::json handleConfirm(const core::CallKwArgs& c) {
        return step_(c, {"draft"}, "confirm", "Only a draft allocation can be submitted.");
    }
    nlohmann::json handleApprove(const core::CallKwArgs& c) {
        return step_(c, {"confirm"}, "validate", "Only a submitted allocation can be approved.");
    }
    nlohmann::json handleRefuse(const core::CallKwArgs& c) {
        return step_(c, {"confirm","validate"}, "refuse", "Only a submitted or approved allocation can be refused.");
    }
};

// Leave requests: draft -> confirm -> validate | refuse | cancel.
class HrLeaveViewModel : public GenericViewModel<HrLeaveReq> {
public:
    explicit HrLeaveViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HrLeaveReq>(std::move(db))
    {
        REGISTER_MUTATOR("create",            handleCreateLeave)
        REGISTER_MUTATOR("write",             handleWriteLeave)
        REGISTER_METHOD("action_confirm",     handleConfirm)
        REGISTER_METHOD("action_approve",     handleApprove)
        REGISTER_METHOD("action_refuse",      handleRefuse)
        REGISTER_METHOD("action_cancel",      handleCancel)
        REGISTER_METHOD("action_reset_draft", handleResetDraft)
        REGISTER_METHOD("leave_balance",      handleBalance)
    }
    std::string modelName() const override { return "hr.leave"; }

private:
    // Recompute the day count from the STORED dates. Called after every create
    // and write, so number_of_days can never disagree with the dates it is
    // supposed to describe — whatever the client sent.
    void recount_(const std::vector<int>& ids) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : ids) {
            auto r = txn.exec(
                "SELECT to_char(date_from,'YYYY-MM-DD'), to_char(date_to,'YYYY-MM-DD'), "
                "       COALESCE(company_id,1) FROM hr_leave WHERE id=$1",
                pqxx::params{id});
            if (r.empty()) continue;
            const double days = HrLeave::workingDays(
                txn, r[0][0].c_str(), r[0][1].c_str(), r[0][2].as<int>(1));
            txn.exec("UPDATE hr_leave SET number_of_days=$2, write_date=now() WHERE id=$1",
                     pqxx::params{id, days});
        }
        txn.commit();
    }

    nlohmann::json handleCreateLeave(const core::CallKwArgs& call) {
        auto res = handleCreate(call);
        if (res.is_number_integer()) recount_({res.get<int>()});
        return res;
    }
    nlohmann::json handleWriteLeave(const core::CallKwArgs& call) {
        // Editing the dates of an already-approved leave would silently change
        // a balance that has been reported. Force it back through the workflow.
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            for (int id : call.ids()) {
                auto s = txn.exec("SELECT state FROM hr_leave WHERE id=$1", pqxx::params{id});
                if (!s.empty()) {
                    const std::string cur = s[0][0].c_str();
                    if (cur == "validate" || cur == "refuse")
                        throw ValidationError(
                            "This leave is already " + cur +
                            "d. Reset it to draft before editing.");
                }
            }
        }
        auto res = handleWrite(call);
        recount_(call.ids());
        return res;
    }

    // Approving is where every rule is enforced, because approval is the moment
    // the leave starts counting against a balance and blocking other requests.
    nlohmann::json handleApprove(const core::CallKwArgs& call) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : call.ids()) {
            auto r = txn.exec(
                "SELECT state, employee_id, leave_type_id, number_of_days, "
                "       to_char(date_from,'YYYY-MM-DD'), to_char(date_to,'YYYY-MM-DD') "
                "  FROM hr_leave WHERE id=$1",
                pqxx::params{id});
            if (r.empty()) throw ValidationError("Leave request not found.");
            if (std::string(r[0][0].c_str()) != "confirm")
                throw ValidationError("Only a submitted leave request can be approved.");

            const int    employeeId = r[0][1].as<int>();
            const int    typeId     = r[0][2].as<int>();
            const double days       = r[0][3].as<double>(0.0);
            const std::string from  = r[0][4].c_str();
            const std::string to    = r[0][5].c_str();

            if (days <= 0)
                throw ValidationError(
                    "This request covers no working days — check the dates, "
                    "weekends and public holidays.");

            // You cannot be on leave twice. Overlap is checked against APPROVED
            // leave only: two pending requests are a decision for the approver,
            // not an error.
            auto ov = txn.exec(
                "SELECT 1 FROM hr_leave "
                " WHERE employee_id=$1 AND id<>$2 AND state='validate' "
                "   AND date_from <= $4::date AND date_to >= $3::date LIMIT 1",
                pqxx::params{employeeId, id, from, to});
            if (!ov.empty())
                throw ValidationError(
                    "This employee already has approved leave overlapping those dates.");

            // The balance is a ceiling when the type says so.
            // as<bool>(), not a string compare: pqxx renders a boolean as
            // "t"/"f", so comparing against "true" silently yields false and
            // the allocation ceiling stops being enforced at all.
            auto ta = txn.exec(
                "SELECT requires_allocation, name FROM hr_leave_type WHERE id=$1",
                pqxx::params{typeId});
            const bool requiresAlloc = !ta.empty() && ta[0][0].as<bool>(false);
            if (requiresAlloc) {
                const Balance b = balanceOf(txn, employeeId, typeId);
                const double remaining = b.allocated - b.taken;
                if (days > remaining + 1e-9) {
                    const std::string tname = ta.empty() ? "this type" : ta[0][1].c_str();
                    throw ValidationError(
                        "Not enough " + tname + " remaining: " +
                        std::to_string(static_cast<int>(remaining)) +
                        " day(s) left, " + std::to_string(static_cast<int>(days)) +
                        " requested.");
                }
            }

            txn.exec("UPDATE hr_leave SET state='validate', write_date=now() WHERE id=$1",
                     pqxx::params{id});
        }
        txn.commit();
        return true;
    }

    nlohmann::json step_(const core::CallKwArgs& call,
                         const std::vector<std::string>& from,
                         const std::string& to,
                         const std::string& refusal) {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : call.ids()) {
            auto s = txn.exec("SELECT state FROM hr_leave WHERE id=$1", pqxx::params{id});
            if (s.empty()) throw ValidationError("Leave request not found.");
            const std::string cur = s[0][0].c_str();
            if (std::find(from.begin(), from.end(), cur) == from.end())
                throw ValidationError(refusal);
            txn.exec("UPDATE hr_leave SET state=$2, write_date=now() WHERE id=$1",
                     pqxx::params{id, to});
        }
        txn.commit();
        return true;
    }

    nlohmann::json handleConfirm(const core::CallKwArgs& c) {
        return step_(c, {"draft"}, "confirm", "Only a draft leave request can be submitted.");
    }
    nlohmann::json handleRefuse(const core::CallKwArgs& c) {
        return step_(c, {"confirm","validate"}, "refuse",
                     "Only a submitted or approved leave request can be refused.");
    }
    // Cancelling an approved leave returns the days: state leaves 'validate',
    // and the balance is derived from state, so nothing else has to be undone.
    nlohmann::json handleCancel(const core::CallKwArgs& c) {
        return step_(c, {"draft","confirm","validate"}, "cancel",
                     "This leave request cannot be cancelled.");
    }
    nlohmann::json handleResetDraft(const core::CallKwArgs& c) {
        return step_(c, {"confirm","refuse","cancel"}, "draft",
                     "Only a submitted, refused or cancelled request can be reset to draft.");
    }

    // leave_balance(employee_id[, leave_type_id]) -> one row per type.
    nlohmann::json handleBalance(const core::CallKwArgs& call) {
        int employeeId = 0;
        if (call.arg(0).is_number_integer()) employeeId = call.arg(0).get<int>();
        else if (call.arg(0).is_array() && !call.arg(0).empty() &&
                 call.arg(0)[0].is_number_integer()) employeeId = call.arg(0)[0].get<int>();
        else if (call.kwargs.contains("employee_id")) employeeId = lvM2oId(call.kwargs["employee_id"]);
        if (employeeId <= 0) throw ValidationError("An employee is required.");

        const int onlyType = call.kwargs.contains("leave_type_id")
                             ? lvM2oId(call.kwargs["leave_type_id"]) : 0;

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto types = onlyType > 0
            ? txn.exec("SELECT id, name, requires_allocation FROM hr_leave_type "
                       " WHERE id=$1 AND active ORDER BY name", pqxx::params{onlyType})
            : txn.exec("SELECT id, name, requires_allocation FROM hr_leave_type "
                       " WHERE active ORDER BY name");

        nlohmann::json out = nlohmann::json::array();
        for (const auto& t : types) {
            const int tid = t["id"].as<int>();
            const Balance b = balanceOf(txn, employeeId, tid);
            const bool req = t["requires_allocation"].as<bool>(false);
            out.push_back({
                {"leave_type_id",       tid},
                {"leave_type",          std::string(t["name"].c_str())},
                {"requires_allocation", req},
                {"allocated",           b.allocated},
                {"taken",               b.taken},
                {"pending",             b.pending},
                // Unlimited types report the days taken, not a remaining count
                // that would be meaningless.
                {"remaining",           req ? (b.allocated - b.taken) : 0.0},
            });
        }
        return out;
    }
};

// ================================================================
// SCHEMA
// ================================================================
void HrLeave::ensureSchema(pqxx::transaction_base& txn) {
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_leave_type (
            id                   SERIAL PRIMARY KEY,
            name                 VARCHAR NOT NULL,
            code                 VARCHAR NOT NULL DEFAULT '',
            requires_allocation  BOOLEAN NOT NULL DEFAULT TRUE,
            is_paid              BOOLEAN NOT NULL DEFAULT TRUE,
            max_days_per_request INTEGER NOT NULL DEFAULT 0,
            color                VARCHAR NOT NULL DEFAULT '',
            company_id           INTEGER NOT NULL DEFAULT 1,
            active               BOOLEAN NOT NULL DEFAULT TRUE,
            create_date          TIMESTAMP NOT NULL DEFAULT now(),
            write_date           TIMESTAMP NOT NULL DEFAULT now()
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_public_holiday (
            id          SERIAL PRIMARY KEY,
            name        VARCHAR NOT NULL,
            date        DATE NOT NULL,
            company_id  INTEGER NOT NULL DEFAULT 1,
            create_date TIMESTAMP NOT NULL DEFAULT now(),
            write_date  TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT hr_public_holiday_uniq UNIQUE (date, company_id)
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_leave_allocation (
            id             SERIAL PRIMARY KEY,
            employee_id    INTEGER NOT NULL REFERENCES hr_employee(id) ON DELETE CASCADE,
            leave_type_id  INTEGER NOT NULL REFERENCES hr_leave_type(id),
            number_of_days NUMERIC(6,2) NOT NULL DEFAULT 0,
            date_from      DATE,
            date_to        DATE,
            state          VARCHAR NOT NULL DEFAULT 'draft',
            notes          TEXT NOT NULL DEFAULT '',
            company_id     INTEGER NOT NULL DEFAULT 1,
            create_date    TIMESTAMP NOT NULL DEFAULT now(),
            write_date     TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT hr_leave_allocation_state_chk
                CHECK (state IN ('draft','confirm','validate','refuse')),
            CONSTRAINT hr_leave_allocation_days_chk CHECK (number_of_days >= 0)
        )
    )");

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_leave (
            id             SERIAL PRIMARY KEY,
            employee_id    INTEGER NOT NULL REFERENCES hr_employee(id) ON DELETE CASCADE,
            leave_type_id  INTEGER NOT NULL REFERENCES hr_leave_type(id),
            date_from      DATE NOT NULL,
            date_to        DATE NOT NULL,
            number_of_days NUMERIC(6,2) NOT NULL DEFAULT 0,
            state          VARCHAR NOT NULL DEFAULT 'draft',
            reason         TEXT NOT NULL DEFAULT '',
            company_id     INTEGER NOT NULL DEFAULT 1,
            create_date    TIMESTAMP NOT NULL DEFAULT now(),
            write_date     TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT hr_leave_state_chk
                CHECK (state IN ('draft','confirm','validate','refuse','cancel')),
            CONSTRAINT hr_leave_range_chk CHECK (date_to >= date_from)
        )
    )");

    txn.exec("CREATE INDEX IF NOT EXISTS hr_leave_emp_idx ON hr_leave (employee_id, state)");
    txn.exec("CREATE INDEX IF NOT EXISTS hr_leave_dates_idx ON hr_leave (date_from, date_to)");
    txn.exec("CREATE INDEX IF NOT EXISTS hr_leave_alloc_emp_idx ON hr_leave_allocation (employee_id, state)");

    // The OPTIONAL text columns must be nullable. A model serialises an empty
    // string as JSON `false` (the convention every other optional text field
    // here follows — see hr_employee.work_email), and the insert turns that
    // into NULL; a NOT NULL column then rejects the row with "the field
    // 'reason' is required" for a field nobody said was required. Dropping the
    // constraint is idempotent and repairs a table already created with it.
    txn.exec("ALTER TABLE hr_leave            ALTER COLUMN reason DROP NOT NULL");
    txn.exec("ALTER TABLE hr_leave_allocation ALTER COLUMN notes  DROP NOT NULL");
    txn.exec("ALTER TABLE hr_leave_type       ALTER COLUMN code   DROP NOT NULL");
    txn.exec("ALTER TABLE hr_leave_type       ALTER COLUMN color  DROP NOT NULL");
}

void HrLeave::seedDefaults(pqxx::transaction_base& txn) {
    // Four types that cover an ordinary Malaysian employment contract. Unpaid
    // and Emergency need no allocation: they are approved on the merits, not
    // against a balance.
    txn.exec(R"(
        INSERT INTO hr_leave_type (name, code, requires_allocation, is_paid, color)
        SELECT v.name, v.code, v.req, v.paid, v.color
          FROM (VALUES
              ('Annual Leave',    'ANNUAL', TRUE,  TRUE,  '#0a6f7d'),
              ('Sick Leave',      'SICK',   TRUE,  TRUE,  '#a95c09'),
              ('Unpaid Leave',    'UNPAID', FALSE, FALSE, '#6b7c8c'),
              ('Emergency Leave', 'EMERG',  FALSE, TRUE,  '#8a3b3b')
          ) AS v(name, code, req, paid, color)
         WHERE NOT EXISTS (SELECT 1 FROM hr_leave_type t WHERE t.code = v.code)
    )");
    // Public holidays are NOT seeded: they vary by year and by state, and a
    // wrong date silently miscounts every request that spans it (docs/113).
}

// ================================================================
// REGISTRATION
// ================================================================
void HrLeave::registerModels(core::ModelFactory& models,
                             std::shared_ptr<infrastructure::DbConnection> db) {
    models.registerCreator("hr.leave.type",       [db]{ return std::make_shared<HrLeaveType>(db); });
    models.registerCreator("hr.public.holiday",   [db]{ return std::make_shared<HrPublicHoliday>(db); });
    models.registerCreator("hr.leave.allocation", [db]{ return std::make_shared<HrLeaveAllocation>(db); });
    models.registerCreator("hr.leave",            [db]{ return std::make_shared<HrLeaveReq>(db); });
}

void HrLeave::registerViewModels(core::ViewModelFactory& viewModels,
                                 std::shared_ptr<infrastructure::DbConnection> db) {
    viewModels.registerCreator("hr.leave.type",       [db]{ return std::make_shared<GenericViewModel<HrLeaveType>>(db); });
    viewModels.registerCreator("hr.public.holiday",   [db]{ return std::make_shared<GenericViewModel<HrPublicHoliday>>(db); });
    viewModels.registerCreator("hr.leave.allocation", [db]{ return std::make_shared<HrLeaveAllocationViewModel>(db); });
    viewModels.registerCreator("hr.leave",            [db]{ return std::make_shared<HrLeaveViewModel>(db); });
}

void HrLeave::seedMenus(pqxx::transaction_base& txn) {
    // Actions 119-121, menus 405-408 — verified free before use (docs/113).
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (119, 'Time Off', 'hr.leave', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (120, 'Allocations', 'hr.leave.allocation', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (121, 'Leave Types', 'hr.leave.type', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (122, 'Public Holidays', 'hr.public.holiday', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, res_model=EXCLUDED.res_model, view_mode=EXCLUDED.view_mode
    )");

    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (405, 'Time Off', 80, 16, 119)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (406, 'Allocations', 80, 17, 120)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (407, 'Leave Types', 83, 30, 121)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (408, 'Public Holidays', 83, 40, 122)
        ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
            sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
}

} // namespace cerp::modules::hr
