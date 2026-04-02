// =============================================================
// core/infrastructure/MigrationRunner.cpp  — implementation
// =============================================================
#include "MigrationRunner.hpp"
#include "DbConnection.hpp"
#include <pqxx/pqxx>
#include <algorithm>
#include <stdexcept>
#include <trantor/utils/Logger.h>

namespace odoo::infrastructure {

MigrationRunner::MigrationRunner(std::shared_ptr<DbConnection> db)
    : db_(std::move(db)) {}

void MigrationRunner::registerMigration(Migration m) {
    migrations_.push_back(std::move(m));
}

void MigrationRunner::ensureMigrationsTable_() {
    // timeout=0: infinite wait — this runs at startup, not in a request handler
    auto conn = db_->acquire(0);
    pqxx::work txn{conn.get()};
    txn.exec0(R"(
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version     INTEGER   PRIMARY KEY,
            description TEXT      NOT NULL,
            applied_at  TIMESTAMP NOT NULL DEFAULT now()
        )
    )");
    txn.commit();
}

std::set<int> MigrationRunner::appliedVersions_() {
    auto conn = db_->acquire(0);
    pqxx::work txn{conn.get()};
    const auto rows = txn.exec("SELECT version FROM schema_migrations ORDER BY version");
    txn.commit();
    std::set<int> applied;
    for (const auto& row : rows)
        applied.insert(row[0].as<int>());
    return applied;
}

void MigrationRunner::runPending() {
    ensureMigrationsTable_();
    const auto applied = appliedVersions_();

    // Apply in ascending version order regardless of registration order
    std::sort(migrations_.begin(), migrations_.end(),
        [](const Migration& a, const Migration& b) { return a.version < b.version; });

    int count = 0;
    for (const auto& m : migrations_) {
        if (applied.count(m.version)) continue;

        LOG_INFO << "[migrations] Applying v" << m.version << ": " << m.description;
        auto conn = db_->acquire(0);
        pqxx::work txn{conn.get()};
        try {
            txn.exec0(m.upSql);
            txn.exec0(
                "INSERT INTO schema_migrations(version, description) VALUES (" +
                std::to_string(m.version) + ", " +
                txn.quote(m.description) + ")");
            txn.commit();
            ++count;
        } catch (const std::exception& ex) {
            // txn will be auto-aborted on destruction
            throw std::runtime_error(
                "[migrations] v" + std::to_string(m.version) +
                " '" + m.description + "' FAILED: " + ex.what());
        }
    }

    if (count > 0)
        LOG_INFO << "[migrations] Applied " << count << " migration(s).";
    else
        LOG_INFO << "[migrations] Schema is up to date.";
}

} // namespace odoo::infrastructure
