#pragma once
// ============================================================
// core/IrCron.hpp
//
// Scheduled job runner. (P5 — docs/045)
//
// WHY
//   There was no scheduler at all. Recurring rental invoicing, recurring
//   expenses, session GC and dunning all need one, and without it each
//   would have grown its own timer.
//
// HOW IT DIFFERS FROM A BARE TIMER
//   The mechanism already existed — Container::startSessionEviction_()
//   uses drogon's runEvery(). What was missing is everything that makes
//   a scheduler trustworthy for money:
//
//     * PERSISTENCE   next-run survives a restart, so a job due while the
//                     server was down runs on startup instead of being
//                     skipped silently.
//     * OVERLAP GUARD a slow job cannot re-enter. Billing that takes 90 s
//                     on a 60 s interval must not run twice concurrently
//                     and double-bill.
//     * FAILURE       a throwing job is logged, counted, and RESCHEDULED.
//                     It is never silently dropped, and it never halts the
//                     scheduler for other jobs.
//     * BACKOFF       repeated failures back off instead of hammering a
//                     broken dependency every minute.
//
// IDEMPOTENCY IS STILL THE JOB'S PROBLEM
//   This guarantees at-least-once, not exactly-once: a crash between
//   "work done" and "next-run written" re-runs the job. Jobs that create
//   documents must therefore carry their own uniqueness constraint —
//   for rental billing that is UNIQUE(contract_line_id, period_start)
//   (docs/040 §3.2). A scheduler cannot provide this on the job's behalf.
// ============================================================
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cerp::infrastructure { class DbConnection; }

namespace cerp::core {

class IrCron {
public:
    /// A job body. Throwing marks the run failed; the job is rescheduled.
    using JobFn = std::function<void()>;

    static void    initialize(std::shared_ptr<infrastructure::DbConnection> db);
    static IrCron& instance();
    static bool    ready();

    /**
     * @brief Bind a job code to its implementation.
     *
     * Called by modules during boot. The DB row decides *when* it runs and
     * whether it is active; this decides *what* runs. A row with no
     * registered handler is reported once and skipped — that is the state
     * after a module is removed but its rows remain.
     */
    void registerJob(const std::string& code, JobFn fn);

    /**
     * @brief Start the scheduler tick.
     *
     * Runs on the drogon event loop, so no extra thread. Must be called
     * after the loop starts (Container::run), not before.
     */
    void start(double tickSeconds = 30.0);

    /// Run one job now, ignoring its schedule. For a "Run now" button.
    /// Honours the overlap guard. @returns false if already running.
    bool runNow(const std::string& code);

    /// Jobs currently executing — for diagnostics and the health endpoint.
    std::vector<std::string> runningJobs() const;

private:
    explicit IrCron(std::shared_ptr<infrastructure::DbConnection> db);

    void tick_();
    void tickTenant_(const std::string& tenant);
    // Runs a job with the DB router pinned to `tenant` for its whole duration.
    void executeJob_(const std::string& tenant, int id, const std::string& code);
    void markSuccess_(int id, int intervalMinutes);
    void markFailure_(int id, const std::string& error, int failures);

    std::shared_ptr<infrastructure::DbConnection> db_;

    mutable std::mutex                        mutex_;
    std::unordered_map<std::string, JobFn>    jobs_;
    std::unordered_set<std::string>           running_;   ///< overlap guard
    std::unordered_set<std::string>           warnedMissing_;
    bool                                      started_ = false;

    static std::unique_ptr<IrCron> s_instance_;
};

} // namespace cerp::core
