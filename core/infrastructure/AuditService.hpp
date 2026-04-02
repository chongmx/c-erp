#pragma once
// ============================================================
// core/infrastructure/AuditService.hpp
//
// Singleton service that records every create/write/unlink
// operation to the audit_log table.
//
// Lifecycle mirrors RuleEngine:
//   AuditService::initialize(db) — called once at startup
//   AuditService::ready()        — true after initialize()
//   AuditService::instance()     — returns the singleton
//
// GenericViewModel calls log() in its handleCreate/handleWrite/
// handleUnlink handlers so every model operation is captured
// without modifying individual modules.
//
// The audit_log table is created via MigrationRunner v1.
// ============================================================
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace odoo::infrastructure {

class DbConnection;  // forward declaration — keeps pqxx out of header (PERF-E)

class AuditService {
public:
    // ── Lifecycle ─────────────────────────────────────────────

    /// Call once at application startup (IrModule::initialize()).
    static void initialize(std::shared_ptr<DbConnection> db);

    /// Returns the singleton. Throws if not initialized.
    static AuditService& instance();

    /// True after initialize() has been called.
    static bool ready();

    // ── Core API (called by GenericViewModel) ─────────────────

    /**
     * @brief Record one CRUD event in audit_log.
     *
     * @param model     e.g. "sale.order"
     * @param operation "create" | "write" | "unlink"
     * @param recordIds affected record ids
     * @param uid       acting user id (0 = anonymous / system)
     */
    void log(const std::string&      model,
             const std::string&      operation,
             const std::vector<int>& recordIds,
             int                     uid);

private:
    explicit AuditService(std::shared_ptr<DbConnection> db);

    std::shared_ptr<DbConnection> db_;

    static std::once_flag               s_once_;
    static std::unique_ptr<AuditService> s_instance_;
};

} // namespace odoo::infrastructure
