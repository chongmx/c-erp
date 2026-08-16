#pragma once
#include "Errors.hpp"
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace odoo::infrastructure {

// ============================================================
// DbConfig
// ============================================================
struct DbConfig {
    std::string host     = "localhost";
    int         port     = 5432;
    std::string name     = "odoo";
    std::string user     = "odoo";
    std::string password = "";
    int         poolSize = 10;

    /** @brief Build a libpqxx connection string from this config. */
    std::string connectionString() const {
        return "host="     + host     +
               " port="    + std::to_string(port) +
               " dbname="  + name     +
               " user="    + user     +
               " password="+ password;
    }
};


// ============================================================
// PooledConnection — RAII connection lease
// ============================================================
/**
 * @brief Scoped lease on a pqxx::connection from DbConnection's pool.
 *
 * Returned by DbConnection::acquire(). Releases the connection back to the
 * pool of the TENANT it was drawn from (not the thread's current tenant,
 * which may change mid-request) on destruction.
 */
class DbConnection;   // forward

class PooledConnection {
public:
    PooledConnection(std::shared_ptr<pqxx::connection> conn,
                     DbConnection&                      pool,
                     std::string                        tenant)
        : conn_(std::move(conn)), pool_(&pool), tenant_(std::move(tenant)) {}

    ~PooledConnection();                        // defined after DbConnection

    PooledConnection(const PooledConnection&)            = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&&)                 = default;
    PooledConnection& operator=(PooledConnection&&)      = default;

    /** @brief Raw libpqxx connection reference. */
    pqxx::connection& get() { return *conn_; }

    /** @brief Convenience: execute a query and return rows. */
    pqxx::result exec(const std::string& sql) {
        pqxx::work txn{*conn_};
        auto rows = txn.exec(sql);
        txn.commit();
        return rows;
    }

private:
    std::shared_ptr<pqxx::connection> conn_;
    DbConnection*                     pool_;
    std::string                       tenant_;

    friend class DbConnection;
};


// ============================================================
// DbConnection — thread-safe, MULTI-TENANT connection pool router
// ============================================================
/**
 * @brief Thread-safe libpqxx connection pool, routing per tenant DATABASE.
 *
 * Single-tenant behaviour is unchanged: constructing with one DbConfig
 * registers it as the default tenant, and acquire() serves it. For
 * multi-company (docs/072), additional tenant databases are registered with
 * registerTenant(), and each HTTP request selects its tenant via the
 * thread-local set by setCurrentTenant()/clearCurrentTenant() (the dispatcher
 * does this from session.db / the authenticate db arg / the Host subdomain).
 * Every existing `db_->acquire()` therefore routes to the right database with
 * no change at the call site.
 *
 * The tenant KEY is the database name (which is what Session.db already
 * carries). Unknown/empty tenant → the default database, so background work
 * (crons, boot) that never sets a tenant keeps hitting the primary DB.
 */
class DbConnection {
public:
    explicit DbConnection(const DbConfig& cfg) {
        defaultTenant_ = cfg.name;
        registerTenant(cfg);
    }

    DbConnection(const DbConnection&)            = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    // ----------------------------------------------------------
    // Tenant registry
    // ----------------------------------------------------------

    /**
     * @brief Register (or update) a tenant database and eagerly open its pool.
     * @param cfg           connection config; cfg.name is the tenant key.
     * @param subdomain     Host-subdomain that routes here (optional).
     * @param emailDomains  login email domains that route here (optional).
     */
    void registerTenant(const DbConfig& cfg,
                        const std::string&              subdomain    = "",
                        const std::vector<std::string>& emailDomains = {}) {
        std::scoped_lock lock{mutex_};
        auto& t = tenants_[cfg.name];
        t.cfg          = cfg;
        t.subdomain    = subdomain;
        t.emailDomains = emailDomains;
        while (static_cast<int>(t.pool.size()) < cfg.poolSize)
            t.pool.push_back(makeConnection_(cfg));
        if (!subdomain.empty()) subdomainIndex_[toLower_(subdomain)] = cfg.name;
        for (const auto& d : emailDomains) emailIndex_[toLower_(d)] = cfg.name;
    }

    /** @brief List every registered tenant database name (for the migration runner). */
    std::vector<std::string> tenantDbs() const {
        std::scoped_lock lock{mutex_};
        std::vector<std::string> out;
        out.reserve(tenants_.size());
        for (const auto& [name, _] : tenants_) out.push_back(name);
        return out;
    }

    bool hasTenant(const std::string& db) const {
        std::scoped_lock lock{mutex_};
        return tenants_.count(db) > 0;
    }

    const std::string& defaultTenant() const { return defaultTenant_; }

    /** @brief Map a Host header (e.g. "acme.easylockerspace.com:443") to a tenant db, or "". */
    std::string resolveHost(const std::string& host) const {
        // strip :port, take the first label as the subdomain
        std::string h = host;
        auto colon = h.find(':');
        if (colon != std::string::npos) h = h.substr(0, colon);
        auto dot = h.find('.');
        const std::string sub = toLower_(dot == std::string::npos ? h : h.substr(0, dot));
        std::scoped_lock lock{mutex_};
        auto it = subdomainIndex_.find(sub);
        return it == subdomainIndex_.end() ? std::string{} : it->second;
    }

    /** @brief Map a login email (e.g. "x@companyA.com") to a tenant db, or "". */
    std::string resolveEmailDomain(const std::string& login) const {
        auto at = login.find('@');
        if (at == std::string::npos) return {};
        const std::string dom = toLower_(login.substr(at + 1));
        std::scoped_lock lock{mutex_};
        auto it = emailIndex_.find(dom);
        return it == emailIndex_.end() ? std::string{} : it->second;
    }

    // ----------------------------------------------------------
    // Per-request tenant selection (thread-local)
    // ----------------------------------------------------------
    void setCurrentTenant(const std::string& db) { currentTenant_ = db; }
    void clearCurrentTenant()                     { currentTenant_.clear(); }
    std::string currentTenant() const {
        return currentTenant_.empty() ? defaultTenant_ : currentTenant_;
    }

    // ----------------------------------------------------------
    // Connection lease
    // ----------------------------------------------------------
    /**
     * @brief Acquire a connection from the CURRENT tenant's pool.
     * @throws PoolExhaustedException on timeout (propagate to a 503).
     */
    PooledConnection acquire(int timeoutMs = 5000) {
        std::unique_lock lock{mutex_};
        std::string key = currentTenant_.empty() ? defaultTenant_ : currentTenant_;
        auto it = tenants_.find(key);
        if (it == tenants_.end()) { key = defaultTenant_; it = tenants_.find(key); }
        Tenant& t = it->second;

        auto pred = [&t]{ return !t.pool.empty(); };
        if (timeoutMs > 0) {
            const bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred);
            if (!ok)
                throw PoolExhaustedException(
                    "Database connection pool exhausted for '" + key + "' (all " +
                    std::to_string(t.cfg.poolSize) + " connections in use after " +
                    std::to_string(timeoutMs) + "ms)");
        } else {
            cv_.wait(lock, pred);
        }
        auto conn = std::move(t.pool.front());
        t.pool.pop_front();
        return PooledConnection{std::move(conn), *this, key};
    }

    // ----------------------------------------------------------
    // Health
    // ----------------------------------------------------------
    bool isHealthy() const {
        std::scoped_lock lock{mutex_};
        for (const auto& [_, t] : tenants_)
            for (const auto& c : t.pool)
                if (c && c->is_open()) return true;
        return !tenants_.empty();
    }

    nlohmann::json healthInfo() const {
        std::scoped_lock lock{mutex_};
        int size = 0, available = 0;
        nlohmann::json perTenant = nlohmann::json::object();
        for (const auto& [name, t] : tenants_) {
            size      += t.cfg.poolSize;
            available += static_cast<int>(t.pool.size());
            perTenant[name] = {{"pool_size", t.cfg.poolSize},
                               {"available", static_cast<int>(t.pool.size())}};
        }
        return {
            {"pool_size", size},
            {"available", available},
            {"in_use",    size - available},
            {"tenants",   perTenant},
            {"status",    (!tenants_.empty()) ? "ok" : "down"},
        };
    }

    /** @brief Config of the default tenant (kept for backward compat). */
    const DbConfig& config() const { return tenants_.at(defaultTenant_).cfg; }

    /** @brief Connection config for a specific tenant db (for pg_dump/restore).
     *  Falls back to the default tenant if `db` is unknown. */
    DbConfig tenantConfig(const std::string& db) const {
        std::scoped_lock lock{mutex_};
        auto it = tenants_.find(db);
        if (it == tenants_.end()) it = tenants_.find(defaultTenant_);
        return it->second.cfg;
    }

private:
    friend class PooledConnection;

    struct Tenant {
        DbConfig                                       cfg;
        std::deque<std::shared_ptr<pqxx::connection>>  pool;
        std::string                                    subdomain;
        std::vector<std::string>                       emailDomains;
    };

    void release_(std::shared_ptr<pqxx::connection> conn, const std::string& tenant) {
        {
            std::scoped_lock lock{mutex_};
            auto it = tenants_.find(tenant);
            if (it == tenants_.end()) it = tenants_.find(defaultTenant_);
            if (!conn || !conn->is_open()) conn = makeConnection_(it->second.cfg);
            it->second.pool.push_back(std::move(conn));
        }
        cv_.notify_all();   // waiters on other tenants re-check their own pool
    }

    static std::shared_ptr<pqxx::connection> makeConnection_(const DbConfig& cfg) {
        return std::make_shared<pqxx::connection>(cfg.connectionString());
    }

    static std::string toLower_(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    }

    mutable std::mutex                        mutex_;
    std::condition_variable                   cv_;
    std::map<std::string, Tenant>             tenants_;
    std::map<std::string, std::string>        subdomainIndex_;
    std::map<std::string, std::string>        emailIndex_;
    std::string                               defaultTenant_;

    // Per-thread selected tenant. Empty → default. Drogon runs each request on
    // one worker thread, so the dispatcher sets this for the request's duration.
    inline static thread_local std::string    currentTenant_;
};


// ============================================================
// PooledConnection destructor (defined after DbConnection)
// ============================================================
inline PooledConnection::~PooledConnection() {
    if (conn_ && pool_) pool_->release_(std::move(conn_), tenant_);
}

} // namespace odoo::infrastructure
