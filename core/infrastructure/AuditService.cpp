// ============================================================
// core/infrastructure/AuditService.cpp
// ============================================================
#include "AuditService.hpp"
#include "DbConnection.hpp"
#include <pqxx/pqxx>
#include <drogon/drogon.h>
#include <sstream>
#include <stdexcept>

namespace odoo::infrastructure {

// ── Static members ────────────────────────────────────────────
std::once_flag                AuditService::s_once_;
std::unique_ptr<AuditService> AuditService::s_instance_;

// ── Lifecycle ─────────────────────────────────────────────────
void AuditService::initialize(std::shared_ptr<DbConnection> db) {
    std::call_once(s_once_, [&db]() {
        s_instance_.reset(new AuditService(std::move(db)));
    });
}

AuditService& AuditService::instance() {
    if (!s_instance_)
        throw std::runtime_error("AuditService: not initialized");
    return *s_instance_;
}

bool AuditService::ready() { return s_instance_ != nullptr; }

AuditService::AuditService(std::shared_ptr<DbConnection> db)
    : db_(std::move(db)) {}

// ── log ───────────────────────────────────────────────────────
void AuditService::log(const std::string&      model,
                       const std::string&      operation,
                       const std::vector<int>& recordIds,
                       int                     uid) {
    if (recordIds.empty()) return;

    // Build record_ids as a PostgreSQL integer array literal
    std::string idsLit = "{";
    for (std::size_t i = 0; i < recordIds.size(); ++i) {
        if (i) idsLit += ',';
        idsLit += std::to_string(recordIds[i]);
    }
    idsLit += '}';

    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec0(
            "INSERT INTO audit_log(model, operation, record_ids, uid, created_at) "
            "VALUES (" +
            txn.quote(model) + "," +
            txn.quote(operation) + "," +
            txn.quote(idsLit) + "::int[]," +
            std::to_string(uid) + "," +
            "now())");
        txn.commit();
    } catch (const std::exception& ex) {
        // Audit logging must never break the main operation.
        // Log and swallow — the original transaction has already committed.
        LOG_ERROR << "[audit] failed to log " << operation << " on " << model
                  << " ids=" << idsLit << ": " << ex.what();
    }
}

} // namespace odoo::infrastructure
