// =============================================================
// modules/hr/HrKiosk.cpp — implementation (docs/113 §3a)
// =============================================================
#include "HrKiosk.hpp"
#include "AuthService.hpp"
#include "ClientIp.hpp"
#include "DbConnection.hpp"
#include "Errors.hpp"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cerp::modules::hr {

using namespace cerp::infrastructure;

namespace {

// ---------------------------------------------------------------
// Rate limiter — the kiosk is the one place an attacker can stand and guess
// 4-digit PINs, so this is load-bearing rather than hygiene. Keyed on IP, with
// a lockout long enough to make exhaustive search impractical and short enough
// that a genuinely fat-fingered employee is not locked out for the shift.
// ---------------------------------------------------------------
class PunchLimiter {
public:
    static constexpr int kMaxFailures  = 8;
    static constexpr int kWindowSeconds = 180;

    bool allow(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(m_);
        auto& e = t_[ip];
        if (e.count >= kMaxFailures &&
            (now - e.start) < std::chrono::seconds(kWindowSeconds))
            return false;
        if ((now - e.start) >= std::chrono::seconds(kWindowSeconds)) {
            e.start = now; e.count = 0;
        }
        return true;
    }
    void fail(const std::string& ip) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(m_);
        auto& e = t_[ip];
        if ((now - e.start) >= std::chrono::seconds(kWindowSeconds)) { e.start = now; e.count = 0; }
        ++e.count;
    }
    void ok(const std::string& ip) {
        std::lock_guard<std::mutex> lk(m_);
        t_.erase(ip);
    }
private:
    using Clock = std::chrono::steady_clock;
    struct E { Clock::time_point start = Clock::now(); int count = 0; };
    std::mutex m_;
    std::unordered_map<std::string, E> t_;
};

std::shared_ptr<PunchLimiter> g_limiter = std::make_shared<PunchLimiter>();

void addSec(const drogon::HttpResponsePtr& r) {
    r->addHeader("X-Content-Type-Options", "nosniff");
    r->addHeader("X-Frame-Options",        "DENY");
    r->addHeader("Referrer-Policy",        "strict-origin-when-cross-origin");
}

} // anonymous namespace

std::string HrKiosk::hashPin(const std::string& pin) {
    // The same PBKDF2 a password gets. A PIN is short, so the work factor is
    // the only thing standing between a stolen database and every PIN in it.
    return cerp::modules::auth::AuthService::hashPassword(pin);
}

void HrKiosk::ensureSchema(pqxx::transaction_base& txn) {
    txn.exec("ALTER TABLE hr_employee ADD COLUMN IF NOT EXISTS pin_hash VARCHAR");
}

void HrKiosk::registerRoutes(std::shared_ptr<infrastructure::DbConnection> db,
                             bool devMode,
                             const std::string& trustedProxies)
{
    const ClientIpResolver clientIp{trustedProxies};
    auto limiter = g_limiter;

    // ---------------------------------------------------------------
    // GET /kiosk — the page. Static, unauthenticated, no session.
    // ---------------------------------------------------------------
    drogon::app().registerHandler("/kiosk",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newFileResponse("web/static/kiosk.html");
            addSec(res);
            cb(res);
        },
        {drogon::Get});

    // ---------------------------------------------------------------
    // POST /kiosk/api/punch  {pin}
    //
    // The ONE thing the kiosk can do. It returns the employee's own name and
    // hours — which is what makes it usable — and nothing about anybody else.
    // ---------------------------------------------------------------
    drogon::app().registerHandler("/kiosk/api/punch",
        [db, limiter, clientIp, devMode](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb)
        {
            auto res = drogon::HttpResponse::newHttpResponse();
            // setContentTypeCode, not addHeader("Content-Type", …): addHeader
            // APPENDS, so the response goes out with drogon's default
            // text/html AND application/json. A client that honours the first
            // one renders a JSON body as HTML.
            res->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            addSec(res);

            const std::string ip = clientIp(req);
            if (!limiter->allow(ip)) {
                res->setStatusCode(drogon::k429TooManyRequests);
                res->setBody(nlohmann::json{
                    {"error", "Too many attempts. Please wait a few minutes."}}.dump());
                cb(res); return;
            }

            try {
                nlohmann::json body;
                try { body = nlohmann::json::parse(req->body()); }
                catch (...) {
                    res->setStatusCode(drogon::k400BadRequest);
                    res->setBody(nlohmann::json{{"error", "Invalid request"}}.dump());
                    cb(res); return;
                }
                const std::string pin = body.value("pin", "");
                if (pin.size() < 4 || pin.size() > 32) {
                    limiter->fail(ip);
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "Invalid PIN"}}.dump());
                    cb(res); return;
                }

                auto conn = db->acquire();
                pqxx::work txn{conn.get()};

                // Every active employee with a PIN is a candidate: the PIN is
                // the whole credential, so the row is found by VERIFYING rather
                // than by looking anything up. The set is small (staff), and it
                // means the kiosk never has to be told who is punching.
                auto rows = txn.exec(
                    "SELECT id, name, pin_hash, COALESCE(company_id,1) AS company_id "
                    "  FROM hr_employee WHERE active AND pin_hash IS NOT NULL");

                int         empId = 0;
                std::string empName;
                for (const auto& r : rows) {
                    if (cerp::modules::auth::AuthService::verifyPassword(
                            pin, std::string(r["pin_hash"].c_str()))) {
                        empId   = r["id"].as<int>();
                        empName = r["name"].c_str();
                        break;
                    }
                }
                if (empId == 0) {
                    limiter->fail(ip);
                    res->setStatusCode(drogon::k401Unauthorized);
                    res->setBody(nlohmann::json{{"error", "PIN not recognised"}}.dump());
                    cb(res); return;
                }
                limiter->ok(ip);

                // Toggle. The partial unique index on hr_attendance is still
                // the thing that makes a double tap impossible; this just picks
                // the direction.
                auto open = txn.exec(
                    "SELECT id FROM hr_attendance "
                    " WHERE employee_id=$1 AND check_out IS NULL LIMIT 1",
                    pqxx::params{empId});

                std::string state;
                if (open.empty()) {
                    txn.exec(
                        "INSERT INTO hr_attendance (employee_id, check_in, company_id) "
                        "SELECT $1, now(), COALESCE(company_id,1) FROM hr_employee WHERE id=$1",
                        pqxx::params{empId});
                    state = "checked_in";
                } else {
                    txn.exec(
                        "UPDATE hr_attendance "
                        "   SET check_out=now(), "
                        "       worked_hours=ROUND((EXTRACT(EPOCH FROM (now()-check_in))/3600.0)::numeric,2), "
                        "       write_date=now() "
                        " WHERE id=$1",
                        pqxx::params{open[0][0].as<int>()});
                    state = "checked_out";
                }

                auto today = txn.exec(
                    "SELECT COALESCE(SUM(COALESCE(worked_hours, "
                    "         EXTRACT(EPOCH FROM (now()-check_in))/3600.0)),0)::numeric(8,2) "
                    "  FROM hr_attendance "
                    " WHERE employee_id=$1 AND check_in::date = CURRENT_DATE",
                    pqxx::params{empId});
                txn.commit();

                res->setStatusCode(drogon::k200OK);
                res->setBody(nlohmann::json{
                    {"ok",                 true},
                    {"name",               empName},
                    {"state",              state},
                    {"worked_hours_today", today.empty() ? 0.0 : today[0][0].as<double>(0.0)},
                }.dump());
                cb(res);
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[kiosk] pool: " << e.what();
                res->setStatusCode(drogon::k503ServiceUnavailable);
                res->setBody(nlohmann::json{
                    {"error", "The server is temporarily overloaded. Please retry."}}.dump());
                cb(res);
            } catch (const std::exception& e) {
                LOG_ERROR << "[kiosk/punch] " << e.what();
                res->setStatusCode(drogon::k500InternalServerError);
                res->setBody(nlohmann::json{
                    {"error", devMode ? e.what() : "An internal error occurred"}}.dump());
                cb(res);
            }
        },
        {drogon::Post});
}

} // namespace cerp::modules::hr
