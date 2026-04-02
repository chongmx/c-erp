#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declaration — keeps pqxx out of this header
namespace odoo::infrastructure { class DbConnection; }

namespace odoo::infrastructure {

// ============================================================
// MigrationRunner — ordered, versioned schema migrations
// ============================================================
/**
 * @brief Applies registered SQL migrations that have not yet been run.
 *
 * A `schema_migrations` table (version INTEGER PRIMARY KEY) is created
 * automatically on first use. Each call to runPending() applies any
 * migrations whose version number is absent from that table, in ascending
 * version order, each in its own transaction.
 *
 * Modules register their migrations in registerMigrations():
 * @code
 *   void SaleModule::registerMigrations(MigrationRunner& r) {
 *       r.registerMigration({200, "add_sale_order_note",
 *           "ALTER TABLE sale_order ADD COLUMN IF NOT EXISTS note TEXT"});
 *   }
 * @endcode
 *
 * Container calls runPending() once during boot, before initialize().
 *
 * Version numbering (per-module ranges, non-overlapping):
 *   1-99    core / base
 *   100-199 account
 *   200-299 sale
 *   300-399 purchase
 *   400-499 stock
 *   500-599 mrp
 *   600-699 portal / report
 *   700-799 auth / auth_signup
 */
class MigrationRunner {
public:
    struct Migration {
        int         version;      ///< Globally unique integer (see range table above)
        std::string description;  ///< Short human-readable label (stored in DB)
        std::string upSql;        ///< Full SQL to apply; runs in one transaction
    };

    explicit MigrationRunner(std::shared_ptr<DbConnection> db);

    /** @brief Queue a migration for runPending(). Safe to call multiple times in any order. */
    void registerMigration(Migration m);

    /**
     * @brief Apply all unapplied migrations in ascending version order.
     *
     * Each migration runs in its own transaction. On failure the transaction
     * is rolled back and a std::runtime_error is thrown — this halts startup
     * so the operator can fix the migration before retrying.
     */
    void runPending();

private:
    void          ensureMigrationsTable_();
    std::set<int> appliedVersions_();

    std::shared_ptr<DbConnection> db_;
    std::vector<Migration>        migrations_;
};

} // namespace odoo::infrastructure
