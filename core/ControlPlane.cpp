#include "ControlPlane.hpp"
#include <pqxx/pqxx>

namespace odoo::core {

using infrastructure::DbConnection;
using infrastructure::DbConfig;

std::unique_ptr<ControlPlane> ControlPlane::s_instance_;

ControlPlane::ControlPlane(std::shared_ptr<DbConnection> db) : db_(std::move(db)) {
    ensureSchema_();
}

void ControlPlane::initialize(const DbConfig& controlCfg) {
    if (s_instance_) return;
    auto db = std::make_shared<DbConnection>(controlCfg);
    s_instance_.reset(new ControlPlane(std::move(db)));
}

bool ControlPlane::ready() { return static_cast<bool>(s_instance_); }

ControlPlane& ControlPlane::instance() {
    if (!s_instance_) throw std::runtime_error("ControlPlane not initialized");
    return *s_instance_;
}

void ControlPlane::ensureSchema_() {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mc_membership (
            identity     TEXT    NOT NULL,
            tenant_db    TEXT    NOT NULL,
            local_login  TEXT    NOT NULL,
            active       BOOLEAN NOT NULL DEFAULT TRUE,
            PRIMARY KEY (identity, tenant_db)
        )
    )");
    txn.exec("CREATE INDEX IF NOT EXISTS mc_membership_tenant_idx "
             "ON mc_membership (tenant_db, local_login)");
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS mc_shared_product (
            code       TEXT   PRIMARY KEY,
            name       TEXT   NOT NULL,
            list_price BIGINT NOT NULL DEFAULT 0
        )
    )");
    txn.commit();
}

std::vector<ControlPlane::SharedProduct> ControlPlane::sharedProducts() const {
    std::vector<SharedProduct> out;
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    auto rows = txn.exec("SELECT code, name, list_price FROM mc_shared_product ORDER BY code");
    for (const auto& r : rows)
        out.push_back({r["code"].c_str(), r["name"].c_str(),
                       r["list_price"].as<long long>(0)});
    return out;
}

void ControlPlane::upsertSharedProduct(const std::string& code, const std::string& name,
                                       long long listPriceMicros) {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    txn.exec("INSERT INTO mc_shared_product (code, name, list_price) VALUES ($1,$2,$3) "
             "ON CONFLICT (code) DO UPDATE SET name=EXCLUDED.name, list_price=EXCLUDED.list_price",
             pqxx::params{code, name, listPriceMicros});
    txn.commit();
}

void ControlPlane::removeSharedProduct(const std::string& code) {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    txn.exec("DELETE FROM mc_shared_product WHERE code=$1", pqxx::params{code});
    txn.commit();
}

std::vector<ControlPlane::Membership>
ControlPlane::membershipsFor(const std::string& identity) const {
    std::vector<Membership> out;
    if (identity.empty()) return out;
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    auto rows = txn.exec(
        "SELECT tenant_db, local_login FROM mc_membership "
        "WHERE identity=$1 AND active=TRUE ORDER BY tenant_db",
        pqxx::params{identity});
    for (const auto& r : rows)
        out.push_back({r["tenant_db"].c_str(), r["local_login"].c_str()});
    return out;
}

std::string ControlPlane::identityFor(const std::string& tenantDb,
                                      const std::string& localLogin) const {
    if (tenantDb.empty() || localLogin.empty()) return {};
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    auto rows = txn.exec(
        "SELECT identity FROM mc_membership "
        "WHERE tenant_db=$1 AND local_login=$2 AND active=TRUE LIMIT 1",
        pqxx::params{tenantDb, localLogin});
    return rows.empty() ? std::string{} : std::string(rows[0]["identity"].c_str());
}

std::string ControlPlane::loginFor(const std::string& identity,
                                   const std::string& tenantDb) const {
    if (identity.empty() || tenantDb.empty()) return {};
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    auto rows = txn.exec(
        "SELECT local_login FROM mc_membership "
        "WHERE identity=$1 AND tenant_db=$2 AND active=TRUE LIMIT 1",
        pqxx::params{identity, tenantDb});
    return rows.empty() ? std::string{} : std::string(rows[0]["local_login"].c_str());
}

void ControlPlane::upsertMembership(const std::string& identity,
                                    const std::string& tenantDb,
                                    const std::string& localLogin) {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(
        "INSERT INTO mc_membership (identity, tenant_db, local_login, active) "
        "VALUES ($1,$2,$3,TRUE) "
        "ON CONFLICT (identity, tenant_db) DO UPDATE SET "
        "  local_login=EXCLUDED.local_login, active=TRUE",
        pqxx::params{identity, tenantDb, localLogin});
    txn.commit();
}

void ControlPlane::removeMembership(const std::string& identity, const std::string& tenantDb) {
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    txn.exec("DELETE FROM mc_membership WHERE identity=$1 AND tenant_db=$2",
             pqxx::params{identity, tenantDb});
    txn.commit();
}

std::vector<ControlPlane::MembershipRow> ControlPlane::allMemberships() const {
    std::vector<MembershipRow> out;
    auto conn = db_->acquire();
    pqxx::work txn{conn.get()};
    auto rows = txn.exec("SELECT identity, tenant_db, local_login, active "
                         "FROM mc_membership ORDER BY identity, tenant_db");
    for (const auto& r : rows)
        out.push_back({r["identity"].c_str(), r["tenant_db"].c_str(),
                       r["local_login"].c_str(), r["active"].as<bool>(true)});
    return out;
}

} // namespace odoo::core
