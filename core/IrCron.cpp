// ============================================================
// core/IrCron.cpp — see IrCron.hpp
// ============================================================
#include "IrCron.hpp"
#include "infrastructure/DbConnection.hpp"

#include <drogon/drogon.h>
#include <pqxx/pqxx>
#include <trantor/utils/Logger.h>

#include <algorithm>

namespace cerp::core {

std::unique_ptr<IrCron> IrCron::s_instance_;

IrCron::IrCron(std::shared_ptr<infrastructure::DbConnection> db)
    : db_(std::move(db)) {}

void IrCron::initialize(std::shared_ptr<infrastructure::DbConnection> db) {
    if (!s_instance_) s_instance_.reset(new IrCron(std::move(db)));
}

IrCron& IrCron::instance() {
    if (!s_instance_)
        throw std::runtime_error("IrCron::instance() before initialize()");
    return *s_instance_;
}

bool IrCron::ready() { return s_instance_ != nullptr; }

void IrCron::registerJob(const std::string& code, JobFn fn) {
    std::lock_guard lk{mutex_};
    jobs_[code] = std::move(fn);
}

std::vector<std::string> IrCron::runningJobs() const {
    std::lock_guard lk{mutex_};
    return {running_.begin(), running_.end()};
}

void IrCron::start(double tickSeconds) {
    {
        std::lock_guard lk{mutex_};
        if (started_) return;
        started_ = true;
    }
    drogon::app().getLoop()->runEvery(tickSeconds, [this] { tick_(); });
    LOG_INFO << "[cron] scheduler started, tick " << tickSeconds << "s";
}


// ── the tick ──────────────────────────────────────────────────

void IrCron::tick_() {
    // Multi-company (docs/072): every tenant has its own ir_cron table, so tick
    // each tenant database with the DB router pinned to it. Single-tenant
    // deployments have exactly one tenant, so this is the old behaviour.
    std::vector<std::string> tenants;
    if (db_) tenants = db_->tenantDbs();
    if (tenants.empty()) tenants.push_back(std::string{});
    for (const auto& t : tenants) tickTenant_(t);
    if (db_) db_->clearCurrentTenant();
}

void IrCron::tickTenant_(const std::string& tenant) {
    if (db_ && !tenant.empty()) db_->setCurrentTenant(tenant);
    std::vector<std::pair<int, std::string>> due;
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        // next_run <= now() catches jobs that came due while the server was
        // down — the reason next_run is persisted rather than held in memory.
        auto rows = txn.exec(
            "SELECT id, code FROM ir_cron "
            " WHERE active AND next_run <= now() "
            " ORDER BY next_run LIMIT 20");
        txn.commit();
        for (const auto& r : rows)
            due.emplace_back(r["id"].as<int>(), r["code"].c_str());
    } catch (const std::exception& ex) {
        LOG_ERROR << "[cron] cannot read schedule for tenant '" << tenant << "': " << ex.what();
        return;   // transient DB problem — try again next tick
    }

    for (const auto& [id, code] : due) executeJob_(tenant, id, code);
}

void IrCron::executeJob_(const std::string& tenant, int id, const std::string& code) {
    // Pin the router to this tenant for the whole job — the lookup below, the
    // job body, and markSuccess_/markFailure_ all hit the right database.
    if (db_ && !tenant.empty()) db_->setCurrentTenant(tenant);
    const std::string guardKey = tenant + "|" + code;   // overlap guard is per-tenant
    JobFn fn;
    {
        std::lock_guard lk{mutex_};

        // Overlap guard. A job still running from a previous tick is skipped
        // rather than started again — this is what stops a 90 s billing run on
        // a 60 s interval from double-billing. Keyed per-tenant so company A's
        // slow billing does not block company B's.
        if (running_.count(guardKey)) {
            LOG_WARN << "[cron] '" << code << "' (tenant " << tenant
                     << ") still running — skipping this tick";
            return;
        }
        auto it = jobs_.find(code);
        if (it == jobs_.end()) {
            // A row with no handler: the module that owned it is gone, or the
            // code is misspelled. Warn ONCE rather than every tick.
            if (warnedMissing_.insert(code).second)
                LOG_ERROR << "[cron] no handler registered for '" << code
                          << "' — row is active but nothing will run it";
            return;
        }
        fn = it->second;
        running_.insert(guardKey);
    }

    int intervalMinutes = 60;
    int failures        = 0;
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT interval_minutes, failure_count FROM ir_cron WHERE id=$1",
                          pqxx::params{id});
        txn.commit();
        if (!r.empty()) {
            intervalMinutes = r[0][0].as<int>(60);
            failures        = r[0][1].as<int>(0);
        }
    } catch (const std::exception&) { /* fall back to defaults */ }

    const auto t0 = std::chrono::steady_clock::now();
    try {
        fn();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        LOG_INFO << "[cron] '" << code << "' completed in " << ms << " ms";
        markSuccess_(id, intervalMinutes);
    } catch (const std::exception& ex) {
        LOG_ERROR << "[cron] '" << code << "' FAILED: " << ex.what();
        markFailure_(id, ex.what(), failures + 1);
    } catch (...) {
        LOG_ERROR << "[cron] '" << code << "' FAILED: unknown exception";
        markFailure_(id, "unknown exception", failures + 1);
    }

    std::lock_guard lk{mutex_};
    running_.erase(guardKey);
}

void IrCron::markSuccess_(int id, int intervalMinutes) {
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        // next_run computed from now(), not from the previous next_run: a job
        // that was overdue does not then fire repeatedly to "catch up".
        txn.exec(
            "UPDATE ir_cron "
            "   SET last_run = now(), "
            "       next_run = now() + ($1 || ' minutes')::interval, "
            "       failure_count = 0, last_error = NULL "
            " WHERE id = $2",
            pqxx::params{std::to_string(intervalMinutes), id});
        txn.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR << "[cron] could not record success for job " << id << ": " << ex.what();
    }
}

void IrCron::markFailure_(int id, const std::string& error, int failures) {
    // Exponential backoff, capped. A broken dependency should not be retried
    // every minute for a week, but the job must never stop being retried —
    // a silently disabled billing job is worse than a noisy one.
    const int backoff = std::min(60 * 8, 1 << std::min(failures, 9));   // minutes
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(
            "UPDATE ir_cron "
            "   SET last_run = now(), "
            "       next_run = now() + ($1 || ' minutes')::interval, "
            "       failure_count = $2, last_error = $3 "
            " WHERE id = $4",
            pqxx::params{std::to_string(backoff), failures, error.substr(0, 500), id});
        txn.commit();
        LOG_WARN << "[cron] job " << id << " retry in " << backoff
                 << " min (failure #" << failures << ")";
    } catch (const std::exception& ex) {
        LOG_ERROR << "[cron] could not record failure for job " << id << ": " << ex.what();
    }
}

bool IrCron::runNow(const std::string& code) {
    // Runs in the caller's current tenant (the dispatcher pins it per request).
    const std::string tenant = db_ ? db_->currentTenant() : std::string{};
    {
        std::lock_guard lk{mutex_};
        if (running_.count(tenant + "|" + code)) return false;
        if (!jobs_.count(code))                  return false;
    }
    int id = 0;
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto r = txn.exec("SELECT id FROM ir_cron WHERE code=$1", pqxx::params{code});
        txn.commit();
        if (r.empty()) return false;
        id = r[0][0].as<int>();
    } catch (const std::exception&) { return false; }

    executeJob_(tenant, id, code);
    return true;
}

} // namespace cerp::core
