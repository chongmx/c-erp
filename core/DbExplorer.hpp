#pragma once
// =============================================================
// core/DbExplorer.hpp — read-only introspection for the in-app
// Database Tools screen (docs/093).
//
// Every entry point here is a QUERY. There is deliberately no write
// path: the caller runs these inside a pqxx::read_transaction, so
// even a crafted `WITH ... INSERT` data-modifying CTE is refused by
// PostgreSQL itself rather than by a string check we wrote. That is
// the security model — the parser-level checks below are a second
// line, not the first.
//
// Identifier safety (S-49): a table or column name arriving from the
// client is never interpolated on trust. It is resolved against
// pg_catalog first; what gets quoted into SQL is the name PostgreSQL
// handed back, not the name the browser sent. A charset check would
// stop injection but would still let a caller name any relation in
// the cluster — resolution against the public schema is what stops
// that.
// =============================================================
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>

namespace cerp::core {

class DbExplorer {
public:
    /// Database-level summary: size, version, object counts, the heaviest tables.
    static nlohmann::json overview(pqxx::transaction_base& txn);

    /// Every table / view in the public schema with row estimate, size, column count.
    static nlohmann::json tables(pqxx::transaction_base& txn);

    /// One relation in full: columns, keys, constraints, indexes, both FK directions.
    static nlohmann::json table(pqxx::transaction_base& txn, const std::string& name);

    /// A page of rows. Params: table, limit, offset, order, dir, filter{col,op,value}.
    static nlohmann::json rows(pqxx::transaction_base& txn, const nlohmann::json& p);

    /// Column profile: null share, distinct count, top values, numeric range.
    static nlohmann::json profile(pqxx::transaction_base& txn, const nlohmann::json& p);

    /// The foreign-key graph of the schema — nodes + edges for the ER diagram.
    static nlohmann::json graph(pqxx::transaction_base& txn);

    /// Run one read-only statement. Params: sql, limit.
    static nlohmann::json query(pqxx::transaction_base& txn, const nlohmann::json& p);

    /// Exposed for tests: is `sql` a single, non-mutating statement?
    /// Returns an empty string when acceptable, else the reason to show the user.
    static std::string rejectReason(const std::string& sql);

    /// Exposed for tests: should this column's values be masked in output?
    static bool isSecretColumn(const std::string& column);
};

} // namespace cerp::core
