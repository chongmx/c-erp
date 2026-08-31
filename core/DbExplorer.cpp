// =============================================================
// core/DbExplorer.cpp — implementation of the read-only database
// introspection behind Settings ▸ Database Tools (docs/093).
// =============================================================
#include "DbExplorer.hpp"
#include "infrastructure/Errors.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace cerp::core {

// Deliberate, user-facing refusals ("no such table", "one statement at a time")
// travel as ValidationError, which the dispatcher passes through verbatim. Any
// other exception is masked per SEC-28 — see handleDbTool_.
using cerp::infrastructure::ValidationError;

namespace {

// ---- small helpers ------------------------------------------------------

std::string sv_(const pqxx::field& f) { return f.is_null() ? std::string{} : std::string(f.c_str()); }

nlohmann::json jstr_(const pqxx::field& f) {
    if (f.is_null()) return nullptr;
    return std::string(f.c_str());
}

std::string lower_(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A relation the caller named, resolved against pg_catalog. The name we quote
// into SQL later is `relname` as PostgreSQL spelled it — never the raw input.
struct Relation {
    long long   oid  = 0;
    std::string name;
    char        kind = 'r';   // r/p table, v view, m matview
};

Relation resolveTable_(pqxx::transaction_base& txn, const std::string& requested) {
    if (requested.empty()) throw ValidationError("No table given.");
    auto r = txn.exec(
        "SELECT c.oid::bigint, c.relname, c.relkind::text "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' AND c.relname = $1 "
        "  AND c.relkind IN ('r','p','v','m')",
        pqxx::params{requested});
    if (r.empty()) throw ValidationError("No such table: " + requested);
    Relation rel;
    rel.oid  = r[0][0].as<long long>();
    rel.name = sv_(r[0][1]);
    rel.kind = sv_(r[0][2]).empty() ? 'r' : sv_(r[0][2])[0];
    return rel;
}

// Same contract for columns: resolved, then quoted from the catalog's spelling.
struct Column {
    std::string name;
    std::string type;   // format_type() output — safe to cast to, it came from PG
};

Column resolveColumn_(pqxx::transaction_base& txn, long long oid, const std::string& requested) {
    if (requested.empty()) throw ValidationError("No column given.");
    auto r = txn.exec(
        "SELECT a.attname, format_type(a.atttypid, a.atttypmod) "
        "FROM pg_attribute a "
        "WHERE a.attrelid = $1::oid AND a.attname = $2 "
        "  AND a.attnum > 0 AND NOT a.attisdropped",
        pqxx::params{oid, requested});
    if (r.empty()) throw ValidationError("No such column: " + requested);
    return Column{sv_(r[0][0]), sv_(r[0][1])};
}

// The module a table belongs to, inferred from its prefix. Used to colour the
// ER diagram and group the sidebar — `account_move_line` reads as "account".
std::string moduleOf_(const std::string& table) {
    static const std::vector<std::string> kPrefixes = {
        "account", "stock", "product", "sale", "purchase", "hr", "mrp", "uom",
        "mail", "rental", "portal", "report", "res", "ir", "audit", "payment",
    };
    for (const auto& p : kPrefixes)
        if (table.rfind(p + "_", 0) == 0 || table == p) return p;
    auto us = table.find('_');
    return us == std::string::npos ? table : table.substr(0, us);
}

// Values under these column names are replaced with a placeholder before they
// leave the server. An admin can already read the hashes via psql; the point is
// that a shoulder-surfable browser screen should not paint them by default.
const std::set<std::string>& secretNames_() {
    static const std::set<std::string> kNames = {
        "password", "passwd", "pwd", "password_hash", "secret", "token",
        "api_key", "apikey", "private_key", "access_token", "refresh_token",
        "session_token", "client_secret", "smtp_pass", "smtp_password",
    };
    return kNames;
}

constexpr const char* kMask = "••••••••";

// Turn a pqxx::result into {columns:[{name,type}], rows:[[...]]}, masking any
// column whose name is a known secret.
nlohmann::json resultToJson_(const pqxx::result& res) {
    nlohmann::json cols = nlohmann::json::array();
    std::vector<bool> mask(res.columns(), false);
    for (pqxx::row_size_type c = 0; c < res.columns(); ++c) {
        const std::string name = res.column_name(c);
        mask[c] = DbExplorer::isSecretColumn(name);
        cols.push_back({{"name", name}, {"masked", mask[c]}});
    }
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& r : res) {
        nlohmann::json line = nlohmann::json::array();
        for (pqxx::row_size_type c = 0; c < res.columns(); ++c) {
            if (r[c].is_null())   line.push_back(nullptr);
            else if (mask[c])     line.push_back(kMask);
            else                  line.push_back(std::string(r[c].c_str()));
        }
        rows.push_back(std::move(line));
    }
    return {{"columns", cols}, {"rows", rows}};
}

long long clampLimit_(const nlohmann::json& p, long long dflt, long long hi) {
    long long n = dflt;
    if (p.contains("limit") && p["limit"].is_number()) n = p["limit"].get<long long>();
    return std::max(1LL, std::min(n, hi));
}

// Types we can meaningfully take min/max/avg of.
bool numericType_(const std::string& t) {
    static const std::vector<std::string> kNum = {
        "smallint", "integer", "bigint", "numeric", "decimal",
        "real", "double precision", "money",
    };
    return std::find(kNum.begin(), kNum.end(), t) != kNum.end();
}

} // namespace

// ---- statement gate -----------------------------------------------------
//
// The read-only transaction is what actually prevents writes. This function
// exists for two narrower jobs the transaction cannot do: keep one box to one
// statement (so a result grid always corresponds to the SQL above it), and give
// a readable reason instead of a raw PostgreSQL error.

std::string DbExplorer::rejectReason(const std::string& rawSql) {
    const std::string sql = trim_(rawSql);
    if (sql.empty()) return "Enter a query.";

    // Walk the text once, tracking which literal/comment we are inside, so a
    // semicolon in a string or a comment is not mistaken for a separator.
    size_t      i = 0, firstWordAt = std::string::npos;
    int         blockDepth = 0;
    std::string dollarTag;
    bool        sawSeparator = false;

    auto codeChar = [&](size_t at) {   // first non-space char of the statement body
        if (firstWordAt == std::string::npos && !std::isspace((unsigned char)sql[at]))
            firstWordAt = at;
    };

    while (i < sql.size()) {
        const char c = sql[i];
        if (blockDepth > 0) {                                  // inside /* ... */
            if (c == '*' && i + 1 < sql.size() && sql[i + 1] == '/') { --blockDepth; i += 2; continue; }
            if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') { ++blockDepth; i += 2; continue; }
            ++i; continue;
        }
        if (!dollarTag.empty()) {                              // inside $tag$ ... $tag$
            if (c == '$' && sql.compare(i, dollarTag.size(), dollarTag) == 0) { i += dollarTag.size(); dollarTag.clear(); }
            else ++i;
            continue;
        }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') { ++blockDepth; i += 2; continue; }
        if (c == '\'' || c == '"') {                           // literal / quoted identifier
            codeChar(i);
            const char q = c;
            ++i;
            while (i < sql.size()) {
                if (sql[i] == q) {
                    if (i + 1 < sql.size() && sql[i + 1] == q) { i += 2; continue; }   // '' escape
                    ++i; break;
                }
                ++i;
            }
            continue;
        }
        if (c == '$') {                                        // possible dollar quote
            size_t j = i + 1;
            while (j < sql.size() && (std::isalnum((unsigned char)sql[j]) || sql[j] == '_')) ++j;
            if (j < sql.size() && sql[j] == '$') { codeChar(i); dollarTag = sql.substr(i, j - i + 1); i = j + 1; continue; }
            codeChar(i); ++i; continue;
        }
        if (c == ';') {
            // Legal only as the very last thing in the box.
            const std::string rest = trim_(sql.substr(i + 1));
            if (!rest.empty()) sawSeparator = true;
            ++i; continue;
        }
        codeChar(i);
        ++i;
    }

    if (sawSeparator)
        return "One statement at a time — remove the extra ';'.";
    if (firstWordAt == std::string::npos)
        return "Enter a query.";

    size_t e = firstWordAt;
    while (e < sql.size() && (std::isalpha((unsigned char)sql[e]) || sql[e] == '_')) ++e;
    const std::string first = lower_(sql.substr(firstWordAt, e - firstWordAt));

    static const std::set<std::string> kAllowed = {"select", "with", "table", "values", "explain", "show"};
    if (!kAllowed.count(first))
        return "Read-only console: statements must start with SELECT, WITH, TABLE, VALUES, EXPLAIN or SHOW.";

    // Defence in depth. Each of these is already blocked (server-side file access
    // needs a role we do not have; the role catalogues are superuser-only), but
    // naming them gives a clear message instead of a permission error.
    const std::string flat = lower_(sql);
    static const std::vector<std::pair<const char*, const char*>> kBlocked = {
        {"pg_authid",           "pg_authid holds role password hashes and is not browsable here."},
        {"pg_shadow",           "pg_shadow holds role password hashes and is not browsable here."},
        {"pg_read_file",        "Server-side file access is not available from this console."},
        {"pg_read_binary_file", "Server-side file access is not available from this console."},
        {"pg_ls_dir",           "Server-side file access is not available from this console."},
        {"lo_import",           "Large-object import is not available from this console."},
        {"lo_export",           "Large-object export is not available from this console."},
        {"dblink",              "Outbound database links are not available from this console."},
    };
    for (const auto& [needle, message] : kBlocked)
        if (flat.find(needle) != std::string::npos) return message;

    return {};
}

bool DbExplorer::isSecretColumn(const std::string& column) {
    return secretNames_().count(lower_(column)) > 0;
}

// ---- overview -----------------------------------------------------------

nlohmann::json DbExplorer::overview(pqxx::transaction_base& txn) {
    nlohmann::json out;

    auto meta = txn.exec(
        "SELECT current_database(), "
        "       pg_database_size(current_database())::bigint, "
        "       current_setting('server_version'), "
        "       pg_size_pretty(pg_database_size(current_database()))");
    out["database"]   = sv_(meta[0][0]);
    out["size_bytes"] = meta[0][1].as<long long>(0);
    out["version"]    = sv_(meta[0][2]);
    out["size_human"] = sv_(meta[0][3]);

    auto counts = txn.exec(
        "SELECT count(*) FILTER (WHERE relkind IN ('r','p')), "
        "       count(*) FILTER (WHERE relkind IN ('v','m')), "
        "       count(*) FILTER (WHERE relkind = 'i'), "
        "       COALESCE(SUM(reltuples) FILTER (WHERE relkind IN ('r','p')), 0)::bigint "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public'");
    out["table_count"] = counts[0][0].as<long long>(0);
    out["view_count"]  = counts[0][1].as<long long>(0);
    out["index_count"] = counts[0][2].as<long long>(0);
    out["row_estimate"] = counts[0][3].as<long long>(0);

    // Foreign keys are the interesting structural number for an ERP schema.
    out["fk_count"] = txn.exec(
        "SELECT count(*) FROM pg_constraint c JOIN pg_namespace n ON n.oid = c.connamespace "
        "WHERE c.contype = 'f' AND n.nspname = 'public'")[0][0].as<long long>(0);

    nlohmann::json bySize = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT c.relname, pg_total_relation_size(c.oid)::bigint, "
             "       GREATEST(c.reltuples, 0)::bigint "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE n.nspname = 'public' AND c.relkind IN ('r','p') "
             "ORDER BY pg_total_relation_size(c.oid) DESC LIMIT 12"))
        bySize.push_back({{"table", sv_(r[0])}, {"bytes", r[1].as<long long>(0)},
                          {"rows", r[2].as<long long>(0)}, {"module", moduleOf_(sv_(r[0]))}});
    out["top_by_size"] = bySize;

    nlohmann::json byRows = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT c.relname, GREATEST(c.reltuples, 0)::bigint, "
             "       pg_total_relation_size(c.oid)::bigint "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE n.nspname = 'public' AND c.relkind IN ('r','p') AND c.reltuples > 0 "
             "ORDER BY c.reltuples DESC LIMIT 12"))
        byRows.push_back({{"table", sv_(r[0])}, {"rows", r[1].as<long long>(0)},
                          {"bytes", r[2].as<long long>(0)}, {"module", moduleOf_(sv_(r[0]))}});
    out["top_by_rows"] = byRows;

    // Per-module rollup — how the schema divides up by owning module.
    nlohmann::json mods = nlohmann::json::object();
    for (const auto& r : txn.exec(
             "SELECT c.relname, pg_total_relation_size(c.oid)::bigint, "
             "       GREATEST(c.reltuples, 0)::bigint "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE n.nspname = 'public' AND c.relkind IN ('r','p')")) {
        const std::string m = moduleOf_(sv_(r[0]));
        if (!mods.contains(m)) mods[m] = {{"module", m}, {"tables", 0}, {"bytes", 0}, {"rows", 0}};
        mods[m]["tables"] = mods[m]["tables"].get<long long>() + 1;
        mods[m]["bytes"]  = mods[m]["bytes"].get<long long>() + r[1].as<long long>(0);
        mods[m]["rows"]   = mods[m]["rows"].get<long long>() + r[2].as<long long>(0);
    }
    nlohmann::json modArr = nlohmann::json::array();
    for (auto& [k, v] : mods.items()) modArr.push_back(v);
    std::sort(modArr.begin(), modArr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a["bytes"].get<long long>() > b["bytes"].get<long long>();
    });
    out["modules"] = modArr;

    return out;
}

// ---- table list ---------------------------------------------------------

nlohmann::json DbExplorer::tables(pqxx::transaction_base& txn) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT c.relname, c.relkind::text, "
             "       GREATEST(c.reltuples, 0)::bigint AS est_rows, "
             "       pg_total_relation_size(c.oid)::bigint AS bytes, "
             "       (SELECT count(*) FROM pg_attribute a "
             "         WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped) AS ncols, "
             "       obj_description(c.oid, 'pg_class') AS comment "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE n.nspname = 'public' AND c.relkind IN ('r','p','v','m') "
             "ORDER BY c.relname")) {
        const std::string name = sv_(r[0]);
        const std::string kind = sv_(r[1]);
        arr.push_back({
            {"name",    name},
            {"kind",    (kind == "v" || kind == "m") ? "view" : "table"},
            {"rows",    r[2].as<long long>(0)},
            {"bytes",   r[3].as<long long>(0)},
            {"columns", r[4].as<long long>(0)},
            {"comment", jstr_(r[5])},
            {"module",  moduleOf_(name)},
        });
    }
    return {{"tables", arr}};
}

// ---- one table ----------------------------------------------------------

nlohmann::json DbExplorer::table(pqxx::transaction_base& txn, const std::string& requested) {
    const Relation rel = resolveTable_(txn, requested);
    nlohmann::json out;
    out["name"]   = rel.name;
    out["kind"]   = (rel.kind == 'v' || rel.kind == 'm') ? "view" : "table";
    out["module"] = moduleOf_(rel.name);

    auto meta = txn.exec(
        "SELECT pg_total_relation_size($1::oid)::bigint, "
        "       pg_size_pretty(pg_total_relation_size($1::oid)), "
        "       GREATEST(c.reltuples, 0)::bigint, "
        "       obj_description(c.oid, 'pg_class') "
        "FROM pg_class c WHERE c.oid = $1::oid",
        pqxx::params{rel.oid});
    if (!meta.empty()) {
        out["bytes"]      = meta[0][0].as<long long>(0);
        out["size_human"] = sv_(meta[0][1]);
        out["est_rows"]   = meta[0][2].as<long long>(0);
        out["comment"]    = jstr_(meta[0][3]);
    }

    // Primary-key columns, so the grid can badge them.
    std::set<std::string> pk;
    for (const auto& r : txn.exec(
             "SELECT a.attname FROM pg_constraint con "
             "JOIN pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = ANY(con.conkey) "
             "WHERE con.conrelid = $1::oid AND con.contype = 'p'",
             pqxx::params{rel.oid}))
        pk.insert(sv_(r[0]));

    // Single-column FKs, mapped for the "→ table.column" badge and click-through.
    nlohmann::json fkByCol = nlohmann::json::object();
    for (const auto& r : txn.exec(
             "SELECT a.attname, tgt.relname, ta.attname "
             "FROM pg_constraint con "
             "JOIN pg_class tgt ON tgt.oid = con.confrelid "
             "JOIN pg_attribute a  ON a.attrelid  = con.conrelid  AND a.attnum  = con.conkey[1] "
             "JOIN pg_attribute ta ON ta.attrelid = con.confrelid AND ta.attnum = con.confkey[1] "
             "WHERE con.conrelid = $1::oid AND con.contype = 'f' "
             "  AND array_length(con.conkey, 1) = 1",
             pqxx::params{rel.oid}))
        fkByCol[sv_(r[0])] = {{"table", sv_(r[1])}, {"column", sv_(r[2])}};

    nlohmann::json cols = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, "
             "       pg_get_expr(d.adbin, d.adrelid), col_description(a.attrelid, a.attnum), a.attnum "
             "FROM pg_attribute a "
             "LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
             "WHERE a.attrelid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
             "ORDER BY a.attnum",
             pqxx::params{rel.oid})) {
        const std::string name = sv_(r[0]);
        nlohmann::json col = {
            {"name",     name},
            {"type",     sv_(r[1])},
            {"notnull",  r[2].as<bool>(false)},
            {"default",  jstr_(r[3])},
            {"comment",  jstr_(r[4])},
            {"pk",       pk.count(name) > 0},
            {"secret",   isSecretColumn(name)},
            {"fk",       fkByCol.contains(name) ? fkByCol[name] : nlohmann::json(nullptr)},
        };
        cols.push_back(std::move(col));
    }
    out["columns"] = cols;

    nlohmann::json cons = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT con.conname, con.contype::text, pg_get_constraintdef(con.oid) "
             "FROM pg_constraint con WHERE con.conrelid = $1::oid "
             "ORDER BY con.contype, con.conname",
             pqxx::params{rel.oid})) {
        static const std::map<std::string, std::string> kKind = {
            {"p", "primary key"}, {"f", "foreign key"}, {"u", "unique"},
            {"c", "check"}, {"t", "trigger"}, {"x", "exclusion"},
        };
        const std::string t = sv_(r[1]);
        auto it = kKind.find(t);
        cons.push_back({{"name", sv_(r[0])}, {"kind", it == kKind.end() ? t : it->second},
                        {"definition", sv_(r[2])}});
    }
    out["constraints"] = cons;

    nlohmann::json idx = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT i.indexname, i.indexdef, "
             "       COALESCE(pg_relation_size(('public.' || quote_ident(i.indexname))::regclass), 0)::bigint "
             "FROM pg_indexes i WHERE i.schemaname = 'public' AND i.tablename = $1 "
             "ORDER BY i.indexname",
             pqxx::params{rel.name}))
        idx.push_back({{"name", sv_(r[0])}, {"definition", sv_(r[1])}, {"bytes", r[2].as<long long>(0)}});
    out["indexes"] = idx;

    // Both directions of the relationship graph, for the Relations tab.
    nlohmann::json refOut = nlohmann::json::array(), refIn = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT con.conname, tgt.relname, pg_get_constraintdef(con.oid) "
             "FROM pg_constraint con JOIN pg_class tgt ON tgt.oid = con.confrelid "
             "WHERE con.conrelid = $1::oid AND con.contype = 'f' ORDER BY tgt.relname",
             pqxx::params{rel.oid}))
        refOut.push_back({{"name", sv_(r[0])}, {"table", sv_(r[1])}, {"definition", sv_(r[2])}});
    for (const auto& r : txn.exec(
             "SELECT con.conname, src.relname, pg_get_constraintdef(con.oid) "
             "FROM pg_constraint con JOIN pg_class src ON src.oid = con.conrelid "
             "WHERE con.confrelid = $1::oid AND con.contype = 'f' ORDER BY src.relname",
             pqxx::params{rel.oid}))
        refIn.push_back({{"name", sv_(r[0])}, {"table", sv_(r[1])}, {"definition", sv_(r[2])}});
    out["references"]    = refOut;
    out["referenced_by"] = refIn;

    return out;
}

// ---- rows ---------------------------------------------------------------

nlohmann::json DbExplorer::rows(pqxx::transaction_base& txn, const nlohmann::json& p) {
    const Relation rel = resolveTable_(txn, p.value("table", std::string{}));
    const long long limit  = clampLimit_(p, 50, 500);
    long long offset = p.contains("offset") && p["offset"].is_number() ? p["offset"].get<long long>() : 0;
    if (offset < 0) offset = 0;

    const std::string qname = "public." + txn.quote_name(rel.name);

    // WHERE — one bound filter. The column is resolved; the operator comes from a
    // fixed table; the value is always a parameter. Comparison casts the parameter
    // to the column's own type so `amount > 100` orders numerically, not as text.
    std::string   where;
    pqxx::params  args;
    bool          hasValue = false;
    if (p.contains("filter") && p["filter"].is_object() &&
        !p["filter"].value("col", std::string{}).empty()) {
        const auto& f  = p["filter"];
        const Column c = resolveColumn_(txn, rel.oid, f.value("col", std::string{}));
        const std::string op    = f.value("op", std::string("contains"));
        const std::string value = f.value("value", std::string{});
        const std::string qcol  = txn.quote_name(c.name);

        static const std::map<std::string, std::string> kCmp = {
            {"eq", "="}, {"ne", "<>"}, {"lt", "<"}, {"lte", "<="}, {"gt", ">"}, {"gte", ">="},
        };
        if (op == "empty")            where = qcol + " IS NULL";
        else if (op == "notempty")    where = qcol + " IS NOT NULL";
        else if (op == "contains")  { where = qcol + "::text ILIKE $1";      args.append("%" + value + "%"); hasValue = true; }
        else if (op == "startswith"){ where = qcol + "::text ILIKE $1";      args.append(value + "%");       hasValue = true; }
        else if (auto it = kCmp.find(op); it != kCmp.end()) {
            // Compare in the column's own type, so `amount > 100` orders
            // numerically rather than as text. Probe the cast first: a bad value
            // is the user's typo, and it should say so instead of surfacing as
            // a masked internal error from three statements later.
            try {
                txn.exec("SELECT $1::" + c.type, pqxx::params{value});
            } catch (const pqxx::sql_error&) {
                throw ValidationError("“" + value + "” is not a valid " + c.type +
                                      " — column " + c.name + " holds " + c.type + " values.");
            }
            where = qcol + " " + it->second + " $1::" + c.type;
            args.append(value); hasValue = true;
        } else throw ValidationError("Unsupported filter operator: " + op);
    }
    const std::string whereSql = where.empty() ? std::string{} : (" WHERE " + where);

    // ORDER BY — resolved column, direction from a two-value set.
    std::string orderSql;
    std::string orderCol = p.value("order", std::string{});
    std::string dir      = lower_(p.value("dir", std::string("asc"))) == "desc" ? "DESC" : "ASC";
    if (!orderCol.empty()) {
        const Column c = resolveColumn_(txn, rel.oid, orderCol);
        orderCol = c.name;
        orderSql = " ORDER BY " + txn.quote_name(c.name) + " " + dir + " NULLS LAST";
    }

    // count(*) is exact on the tables this ERP actually has; past a couple of
    // hundred thousand rows it becomes the slowest part of paging, so fall back
    // to the planner's estimate and say so rather than stalling the screen.
    long long total = 0;
    bool      estimated = false;
    auto est = txn.exec("SELECT GREATEST(reltuples, 0)::bigint FROM pg_class WHERE oid = $1::oid",
                        pqxx::params{rel.oid});
    const long long estRows = est.empty() ? 0 : est[0][0].as<long long>(0);
    if (where.empty() && estRows > 200000) {
        total = estRows;
        estimated = true;
    } else {
        auto cr = txn.exec("SELECT count(*)::bigint FROM " + qname + whereSql, args);
        total = cr[0][0].as<long long>(0);
    }

    const std::string sql = "SELECT * FROM " + qname + whereSql + orderSql +
                            " LIMIT " + std::to_string(limit) +
                            " OFFSET " + std::to_string(offset);

    const auto t0 = std::chrono::steady_clock::now();
    pqxx::result res = hasValue ? txn.exec(sql, args) : txn.exec(sql);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    nlohmann::json out = resultToJson_(res);
    out["table"]     = rel.name;
    out["total"]     = total;
    out["estimated"] = estimated;
    out["limit"]     = limit;
    out["offset"]    = offset;
    out["order"]     = orderCol;
    out["dir"]       = lower_(dir);
    out["elapsed_ms"] = static_cast<long long>(ms);
    out["sql"]       = sql;   // shown in the UI: the grid is honest about its query
    return out;
}

// ---- column profile -----------------------------------------------------

nlohmann::json DbExplorer::profile(pqxx::transaction_base& txn, const nlohmann::json& p) {
    const Relation rel = resolveTable_(txn, p.value("table", std::string{}));
    const Column   col = resolveColumn_(txn, rel.oid, p.value("column", std::string{}));
    if (isSecretColumn(col.name))
        throw ValidationError("This column holds credentials and is not profiled.");

    const std::string qname = "public." + txn.quote_name(rel.name);
    const std::string qcol  = txn.quote_name(col.name);

    nlohmann::json out;
    out["table"]  = rel.name;
    out["column"] = col.name;
    out["type"]   = col.type;

    auto agg = txn.exec("SELECT count(*)::bigint, count(" + qcol + ")::bigint, "
                        "count(DISTINCT " + qcol + ")::bigint FROM " + qname);
    const long long total   = agg[0][0].as<long long>(0);
    const long long nonNull = agg[0][1].as<long long>(0);
    out["total"]    = total;
    out["nulls"]    = total - nonNull;
    out["distinct"] = agg[0][2].as<long long>(0);

    if (numericType_(col.type)) {
        auto n = txn.exec("SELECT min(" + qcol + ")::text, max(" + qcol + ")::text, "
                          "round(avg(" + qcol + ")::numeric, 4)::text FROM " + qname);
        out["min"] = jstr_(n[0][0]);
        out["max"] = jstr_(n[0][1]);
        out["avg"] = jstr_(n[0][2]);
    }

    nlohmann::json top = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT " + qcol + "::text, count(*)::bigint FROM " + qname +
             " WHERE " + qcol + " IS NOT NULL GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 12"))
        top.push_back({{"value", sv_(r[0])}, {"count", r[1].as<long long>(0)}});
    out["top_values"] = top;

    return out;
}

// ---- relationship graph -------------------------------------------------

nlohmann::json DbExplorer::graph(pqxx::transaction_base& txn) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT c.relname, GREATEST(c.reltuples, 0)::bigint, "
             "       (SELECT count(*) FROM pg_attribute a "
             "         WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped) "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE n.nspname = 'public' AND c.relkind IN ('r','p') "
             "ORDER BY c.relname")) {
        const std::string name = sv_(r[0]);
        nodes.push_back({{"id", name}, {"rows", r[1].as<long long>(0)},
                         {"columns", r[2].as<long long>(0)}, {"module", moduleOf_(name)}});
    }

    nlohmann::json edges = nlohmann::json::array();
    for (const auto& r : txn.exec(
             "SELECT src.relname, tgt.relname, a.attname, ta.attname "
             "FROM pg_constraint con "
             "JOIN pg_class src ON src.oid = con.conrelid "
             "JOIN pg_class tgt ON tgt.oid = con.confrelid "
             "JOIN pg_namespace n ON n.oid = src.relnamespace "
             "JOIN pg_attribute a  ON a.attrelid  = con.conrelid  AND a.attnum  = con.conkey[1] "
             "JOIN pg_attribute ta ON ta.attrelid = con.confrelid AND ta.attnum = con.confkey[1] "
             "WHERE con.contype = 'f' AND n.nspname = 'public' "
             "ORDER BY src.relname, tgt.relname"))
        edges.push_back({{"source", sv_(r[0])}, {"target", sv_(r[1])},
                         {"source_column", sv_(r[2])}, {"target_column", sv_(r[3])}});

    return {{"nodes", nodes}, {"edges", edges}};
}

// ---- SQL console --------------------------------------------------------

nlohmann::json DbExplorer::query(pqxx::transaction_base& txn, const nlohmann::json& p) {
    const std::string sql = trim_(p.value("sql", std::string{}));
    const std::string bad = rejectReason(sql);
    if (!bad.empty()) throw ValidationError(bad);

    const long long limit = clampLimit_(p, 200, 1000);

    // Strip a trailing ';' so the statement can be wrapped.
    std::string body = sql;
    while (!body.empty() && (body.back() == ';' || std::isspace((unsigned char)body.back())))
        body.pop_back();

    // Wrapping in a subquery caps the result set without editing the user's text
    // (appending LIMIT would break `... ORDER BY x LIMIT 5` and UNION queries).
    // EXPLAIN and SHOW are not subquery-able, so they run as written.
    const std::string first = lower_(body.substr(0, body.find_first_of(" \t\r\n(")));
    const bool wrappable = (first == "select" || first == "with" || first == "table" || first == "values");
    // The newlines are load-bearing: a query ending in a `-- comment` would
    // otherwise swallow the closing paren and everything after it on the line.
    const std::string finalSql = wrappable
        ? "SELECT * FROM (\n" + body + "\n) AS _dbtool_q LIMIT " + std::to_string(limit)
        : body;

    const auto t0 = std::chrono::steady_clock::now();
    pqxx::result res = txn.exec(finalSql);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    nlohmann::json out = resultToJson_(res);
    out["elapsed_ms"] = static_cast<long long>(ms);
    out["row_count"]  = static_cast<long long>(res.size());
    out["truncated"]  = wrappable && static_cast<long long>(res.size()) >= limit;
    out["limit"]      = limit;
    return out;
}

} // namespace cerp::core
