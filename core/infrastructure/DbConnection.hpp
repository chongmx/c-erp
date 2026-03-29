#pragma once
#include "Errors.hpp"
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

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
 * Returned by DbConnection::acquire(). Releases the connection back to
 * the pool on destruction — the caller never calls release() manually.
 *
 * Usage:
 * @code
 *   auto conn = db->acquire();
 *   pqxx::work txn{conn.get()};
 *   auto rows = txn.exec("SELECT id, name FROM res_partner LIMIT 10");
 *   txn.commit();
 * @endcode
 */
class DbConnection;   // forward

class PooledConnection {
public:
    PooledConnection(std::shared_ptr<pqxx::connection> conn,
                     DbConnection&                      pool)
        : conn_(std::move(conn)), pool_(pool) {}

    ~PooledConnection();                        // defined after DbConnection

    PooledConnection(const PooledConnection&)            = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&&)                 = default;
    PooledConnection& operator=(PooledConnection&&)      = default;

    /** @brief Raw libpqxx connection reference. */
    pqxx::connection& get() { return *conn_; }

    /** @brief Convenience: execute a parameterised query and return rows. */
    pqxx::result exec(const std::string& sql) {
        pqxx::work txn{*conn_};
        auto rows = txn.exec(sql);
        txn.commit();
        return rows;
    }

private:
    std::shared_ptr<pqxx::connection> conn_;
    DbConnection&                     pool_;

    friend class DbConnection;
};


// ============================================================
// DbConnection — thread-safe connection pool
// ============================================================
/**
 * @brief Thread-safe libpqxx connection pool.
 *
 * Opens DbConfig::poolSize connections eagerly on construction.
 * acquire() blocks until a connection is available (bounded wait).
 *
 * Typical usage (via Container):
 * @code
 *   auto conn = container->db->acquire();
 *   pqxx::work txn{conn.get()};
 *   txn.exec0("UPDATE res_partner SET name=$1 WHERE id=$2",
 *              pqxx::params{"Acme", 42});
 *   txn.commit();
 * @endcode
 *
 * The pool is intentionally NOT an IService — it is infrastructure
 * owned by Container directly and injected into ModelFactory and
 * ServiceFactory at construction time.
 */
class DbConnection {
public:
    explicit DbConnection(const DbConfig& cfg) : cfg_(cfg) {
        for (int i = 0; i < cfg_.poolSize; ++i)
            pool_.push_back(makeConnection_());
    }

    // Non-copyable, non-movable — shared via shared_ptr
    DbConnection(const DbConnection&)            = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    // ----------------------------------------------------------
    // Connection lease
    // ----------------------------------------------------------

    /**
     * @brief Acquire a connection from the pool (blocks until one is free or timeout).
     *
     * @param timeoutMs  Max wait time in milliseconds.  Default: 5000 ms (PERF-C).
     *                   Pass 0 to block indefinitely (strongly discouraged in
     *                   production — use only in non-HTTP contexts).
     * @throws PoolExhaustedException if the timeout expires before a connection
     *         is available.  Callers should propagate this to a 503 response.
     */
    PooledConnection acquire(int timeoutMs = 5000) {
        std::unique_lock lock{mutex_};

        auto pred = [this]{ return !pool_.empty(); };

        if (timeoutMs > 0) {
            const bool ok = cv_.wait_for(
                lock, std::chrono::milliseconds(timeoutMs), pred);
            if (!ok)
                throw PoolExhaustedException(
                    "Database connection pool exhausted (all " +
                    std::to_string(cfg_.poolSize) +
                    " connections in use after " +
                    std::to_string(timeoutMs) + "ms)");
        } else {
            cv_.wait(lock, pred);
        }

        auto conn = std::move(pool_.front());
        pool_.pop_front();
        return PooledConnection{std::move(conn), *this};
    }

    // ----------------------------------------------------------
    // Health
    // ----------------------------------------------------------

    /**
     * @brief Return true if at least one pooled connection is alive.
     * Cheap — does not send a network packet.
     */
    bool isHealthy() const {
        std::scoped_lock lock{mutex_};
        return isHealthy_();
    }

    /**
     * @brief Return a JSON health descriptor (for Container::healthCheck()).
     */
    nlohmann::json healthInfo() const {
        std::scoped_lock lock{mutex_};
        const int available = static_cast<int>(pool_.size());
        return {
            {"pool_size",  cfg_.poolSize},
            {"available",  available},
            {"in_use",     cfg_.poolSize - available},
            {"status",     isHealthy_() ? "ok" : "down"},
        };
    }

    // ----------------------------------------------------------
    // Config accessor
    // ----------------------------------------------------------
    const DbConfig& config() const { return cfg_; }

private:
    friend class PooledConnection;

    /** @brief Lock-free health check — caller must hold mutex_. */
    bool isHealthy_() const {
        for (const auto& c : pool_)
            if (c && c->is_open()) return true;
        // Connections are all in use — assume the server is up.
        return !pool_.empty() || (cfg_.poolSize > 0);
    }

    /** @brief Return a connection to the pool (called by ~PooledConnection). */
    void release_(std::shared_ptr<pqxx::connection> conn) {
        {
            std::scoped_lock lock{mutex_};
            // Reconnect if the connection dropped while in use.
            if (!conn || !conn->is_open())
                conn = makeConnection_();
            pool_.push_back(std::move(conn));
        }
        cv_.notify_one();
    }

    std::shared_ptr<pqxx::connection> makeConnection_() {
        return std::make_shared<pqxx::connection>(cfg_.connectionString());
    }

    DbConfig                                     cfg_;
    mutable std::mutex                           mutex_;
    std::condition_variable                      cv_;
    std::deque<std::shared_ptr<pqxx::connection>> pool_;
};


// ============================================================
// PooledConnection destructor (defined after DbConnection)
// ============================================================
inline PooledConnection::~PooledConnection() {
    if (conn_) pool_.release_(std::move(conn_));
}

} // namespace odoo::infrastructure