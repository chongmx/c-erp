// =============================================================
// modules/hr/HrAttendance.cpp — implementation (docs/113 §1)
// =============================================================
#include "HrAttendance.hpp"
#include "BaseModel.hpp"
#include "GenericViewModel.hpp"
#include "Factories.hpp"
#include "DbConnection.hpp"
#include "Errors.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::hr {

using namespace cerp::infrastructure;
using namespace cerp::core;

namespace {
inline int attM2oId(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer())
        return v[0].get<int>();
    return 0;
}

// The employee a call is about. Accepts it positionally (args[0]) or as a
// kwarg, because the kiosk and the back-office screen call this differently
// and neither should have to know which the other uses.
int employeeArg(const core::CallKwArgs& call) {
    if (call.arg(0).is_number_integer()) return call.arg(0).get<int>();
    if (call.arg(0).is_array() && !call.arg(0).empty() &&
        call.arg(0)[0].is_number_integer())
        return call.arg(0)[0].get<int>();
    if (call.kwargs.contains("employee_id"))
        return attM2oId(call.kwargs["employee_id"]);
    return 0;
}

void requireEmployee(pqxx::transaction_base& txn, int employeeId) {
    if (employeeId <= 0)
        throw ValidationError("An employee is required.");
    auto r = txn.exec("SELECT 1 FROM hr_employee WHERE id=$1 AND active",
                      pqxx::params{employeeId});
    if (r.empty())
        throw ValidationError("That employee does not exist, or is archived.");
}
} // anonymous namespace

// ================================================================
// MODEL
// ================================================================
class HrAttendanceRec : public BaseModel<HrAttendanceRec> {
public:
    static constexpr const char* MODEL_NAME = "hr.attendance";
    static constexpr const char* TABLE_NAME = "hr_attendance";

    explicit HrAttendanceRec(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    int         employeeId  = 0;
    std::string checkIn;
    std::string checkOut;
    double      workedHours = 0.0;
    int         companyId   = 1;

    void registerFields() {
        fieldRegistry_.add({"employee_id",  FieldType::Many2one, "Employee", true, false, true, false, "hr.employee"});
        fieldRegistry_.add({"check_in",     FieldType::Datetime, "Check In",  true});
        fieldRegistry_.add({"check_out",    FieldType::Datetime, "Check Out"});
        // Derived on the server at check-out. Registered so it can be READ and
        // filtered, but a write to it is overwritten by the next check-out.
        fieldRegistry_.add({"worked_hours", FieldType::Float,    "Worked Hours"});
        fieldRegistry_.add({"company_id",   FieldType::Many2one, "Company", false, false, true, false, "res.company"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["employee_id"]  = employeeId > 0 ? nlohmann::json::array({employeeId, ""}) : nlohmann::json(false);
        j["check_in"]     = checkIn.empty()  ? nlohmann::json(false) : nlohmann::json(checkIn);
        j["check_out"]    = checkOut.empty() ? nlohmann::json(false) : nlohmann::json(checkOut);
        j["worked_hours"] = workedHours;
        j["company_id"]   = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("employee_id"))  employeeId = attM2oId(j["employee_id"]);
        if (j.contains("check_in")   && j["check_in"].is_string())   checkIn   = j["check_in"].get<std::string>();
        if (j.contains("check_out")  && j["check_out"].is_string())  checkOut  = j["check_out"].get<std::string>();
        if (j.contains("company_id"))   companyId  = attM2oId(j["company_id"]);
        // worked_hours is deliberately NOT deserialised: it is derived.
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = checkIn;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (employeeId <= 0) e.push_back("An employee is required");
        if (checkIn.empty()) e.push_back("A check-in time is required");
        return e;
    }
};

// ================================================================
// VIEWMODEL — the clock
// ================================================================
class HrAttendanceViewModel : public GenericViewModel<HrAttendanceRec> {
public:
    explicit HrAttendanceViewModel(std::shared_ptr<DbConnection> db)
        : GenericViewModel<HrAttendanceRec>(std::move(db))
    {
        REGISTER_MUTATOR("write",            handleWriteAtt)
        REGISTER_METHOD("action_check_in",   handleCheckIn)
        REGISTER_METHOD("action_check_out",  handleCheckOut)
        REGISTER_METHOD("action_toggle",     handleToggle)
        REGISTER_METHOD("attendance_state",  handleState)
    }
    std::string modelName() const override { return "hr.attendance"; }

private:
    // worked_hours is DERIVED, and `write` is the path that forgets it.
    // deserializeFields() ignoring the field is not enough: BaseModel::write()
    // writes registered fields straight from the payload, so a client could
    // set its own hours and — on an already closed record, which check-out
    // never revisits — the number would simply stand. Recomputing here from
    // the stored timestamps makes the derivation true on every path rather
    // than only on the one that happens to run check-out.
    nlohmann::json handleWriteAtt(const core::CallKwArgs& call) {
        auto res = handleWrite(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        for (int id : call.ids()) {
            txn.exec(
                "UPDATE hr_attendance "
                "   SET worked_hours = CASE WHEN check_out IS NULL THEN 0 "
                "       ELSE ROUND((EXTRACT(EPOCH FROM (check_out - check_in))/3600.0)::numeric, 2) END "
                " WHERE id = $1",
                pqxx::params{id});
        }
        txn.commit();
        return res;
    }

    // The open attendance for an employee, or 0.
    static int openIdFor(pqxx::transaction_base& txn, int employeeId) {
        auto r = txn.exec(
            "SELECT id FROM hr_attendance "
            " WHERE employee_id=$1 AND check_out IS NULL LIMIT 1",
            pqxx::params{employeeId});
        return r.empty() ? 0 : r[0][0].as<int>();
    }

    // Sum of today's completed hours, plus the running open one if any.
    static nlohmann::json stateOf(pqxx::transaction_base& txn, int employeeId) {
        auto r = txn.exec(
            "SELECT id, to_char(check_in,'YYYY-MM-DD\"T\"HH24:MI:SS') AS since "
            "  FROM hr_attendance WHERE employee_id=$1 AND check_out IS NULL LIMIT 1",
            pqxx::params{employeeId});

        // Completed hours booked today, plus the elapsed part of an open one —
        // so a kiosk shows a number that moves rather than one that jumps at
        // check-out.
        auto t = txn.exec(
            "SELECT COALESCE(SUM(COALESCE(worked_hours, "
            "         EXTRACT(EPOCH FROM (now() - check_in))/3600.0)),0)::numeric(8,2) "
            "  FROM hr_attendance "
            " WHERE employee_id=$1 AND check_in::date = CURRENT_DATE",
            pqxx::params{employeeId});
        const double today = t.empty() ? 0.0 : t[0][0].as<double>(0.0);

        nlohmann::json out{
            {"employee_id",        employeeId},
            {"state",              r.empty() ? "checked_out" : "checked_in"},
            {"worked_hours_today", today},
        };
        out["attendance_id"] = r.empty() ? nlohmann::json(false)
                                         : nlohmann::json(r[0]["id"].as<int>());
        out["since"]         = r.empty() ? nlohmann::json(false)
                                         : nlohmann::json(std::string(r[0]["since"].c_str()));
        return out;
    }

    // Open an attendance. The partial unique index is the real guard against a
    // double tap; this check exists to turn the resulting constraint violation
    // into a sentence a person can act on.
    nlohmann::json handleCheckIn(const core::CallKwArgs& call) {
        const int employeeId = employeeArg(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        requireEmployee(txn, employeeId);
        if (openIdFor(txn, employeeId) != 0)
            throw ValidationError("Already checked in. Check out first.");

        auto r = txn.exec(
            "INSERT INTO hr_attendance (employee_id, check_in, company_id) "
            "SELECT $1, now(), COALESCE(company_id,1) FROM hr_employee WHERE id=$1 "
            "RETURNING id",
            pqxx::params{employeeId});
        const int id = r[0][0].as<int>();
        auto out = stateOf(txn, employeeId);
        txn.commit();
        out["attendance_id"] = id;
        return out;
    }

    // Close it, and derive worked_hours from the STORED timestamps — never from
    // anything the caller supplied.
    nlohmann::json handleCheckOut(const core::CallKwArgs& call) {
        const int employeeId = employeeArg(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        requireEmployee(txn, employeeId);
        const int openId = openIdFor(txn, employeeId);
        if (openId == 0)
            throw ValidationError("Not checked in.");

        // An interval that would overlap an already-closed one means the clock
        // has been tampered with or the row was hand-edited; refuse rather than
        // silently book time twice.
        auto ov = txn.exec(
            "SELECT 1 FROM hr_attendance a "
            " WHERE a.employee_id=$1 AND a.id<>$2 AND a.check_out IS NOT NULL "
            "   AND a.check_out > (SELECT check_in FROM hr_attendance WHERE id=$2) "
            " LIMIT 1",
            pqxx::params{employeeId, openId});
        if (!ov.empty())
            throw ValidationError(
                "This check-out would overlap an existing attendance record.");

        txn.exec(
            "UPDATE hr_attendance "
            "   SET check_out = now(), "
            "       worked_hours = ROUND((EXTRACT(EPOCH FROM (now() - check_in))/3600.0)::numeric, 2), "
            "       write_date = now() "
            " WHERE id = $1",
            pqxx::params{openId});

        auto out = stateOf(txn, employeeId);
        auto w = txn.exec("SELECT worked_hours FROM hr_attendance WHERE id=$1",
                          pqxx::params{openId});
        txn.commit();
        out["attendance_id"] = openId;
        out["worked_hours"]  = w.empty() ? 0.0 : w[0][0].as<double>(0.0);
        return out;
    }

    // What a kiosk button calls: in if out, out if in.
    nlohmann::json handleToggle(const core::CallKwArgs& call) {
        const int employeeId = employeeArg(call);
        bool isIn;
        {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            requireEmployee(txn, employeeId);
            isIn = openIdFor(txn, employeeId) != 0;
        }
        return isIn ? handleCheckOut(call) : handleCheckIn(call);
    }

    nlohmann::json handleState(const core::CallKwArgs& call) {
        const int employeeId = employeeArg(call);
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        requireEmployee(txn, employeeId);
        return stateOf(txn, employeeId);
    }
};

// ================================================================
// SCHEMA
// ================================================================
void HrAttendance::ensureSchema(pqxx::transaction_base& txn) {
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS hr_attendance (
            id           SERIAL PRIMARY KEY,
            employee_id  INTEGER NOT NULL REFERENCES hr_employee(id) ON DELETE CASCADE,
            check_in     TIMESTAMP NOT NULL DEFAULT now(),
            check_out    TIMESTAMP,
            worked_hours NUMERIC(8,2) NOT NULL DEFAULT 0,
            company_id   INTEGER NOT NULL DEFAULT 1,
            create_date  TIMESTAMP NOT NULL DEFAULT now(),
            write_date   TIMESTAMP NOT NULL DEFAULT now(),
            CONSTRAINT hr_attendance_interval_chk
                CHECK (check_out IS NULL OR check_out > check_in)
        )
    )");

    // THE guard: at most one open attendance per employee. A partial unique
    // index makes two simultaneous kiosk taps produce one row and one error,
    // rather than two rows and a plausible-looking timesheet.
    txn.exec(R"(
        CREATE UNIQUE INDEX IF NOT EXISTS hr_attendance_one_open_uniq
            ON hr_attendance (employee_id) WHERE check_out IS NULL
    )");
    txn.exec(R"(
        CREATE INDEX IF NOT EXISTS hr_attendance_emp_day_idx
            ON hr_attendance (employee_id, check_in)
    )");
}

// ================================================================
// REGISTRATION
// ================================================================
void HrAttendance::registerModels(core::ModelFactory& models,
                                  std::shared_ptr<infrastructure::DbConnection> db) {
    models.registerCreator("hr.attendance", [db]{ return std::make_shared<HrAttendanceRec>(db); });
}

void HrAttendance::registerViewModels(core::ViewModelFactory& viewModels,
                                      std::shared_ptr<infrastructure::DbConnection> db) {
    viewModels.registerCreator("hr.attendance", [db]{ return std::make_shared<HrAttendanceViewModel>(db); });
}

void HrAttendance::seedMenus(pqxx::transaction_base& txn) {
    // ids verified free before use (docs/113): actions 118, menus 404.
    txn.exec(R"(
        INSERT INTO ir_act_window (id, name, res_model, view_mode, context, target)
        VALUES (118, 'Attendance', 'hr.attendance', 'list,form', '{}', 'current')
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, res_model=EXCLUDED.res_model,
                view_mode=EXCLUDED.view_mode
    )");
    txn.exec(R"(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id)
        VALUES (404, 'Attendance', 80, 15, 118)
        ON CONFLICT (id) DO UPDATE
            SET name=EXCLUDED.name, parent_id=EXCLUDED.parent_id,
                sequence=EXCLUDED.sequence, action_id=EXCLUDED.action_id
    )");
}

} // namespace cerp::modules::hr
