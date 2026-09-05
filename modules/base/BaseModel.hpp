#pragma once
#include "IModel.hpp"
#include "FieldRegistry.hpp"
#include "Domain.hpp"
#include "DbConnection.hpp"
#include "RuleEngine.hpp"
#include "Money.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cerp::core {

// ============================================================
// ODOO_MODEL macro
// ============================================================
#define ODOO_MODEL(model_name, table_name)                     \
    static constexpr const char* MODEL_NAME = model_name;     \
    static constexpr const char* TABLE_NAME = table_name;     \
    static constexpr const char* staticModelName() {          \
        return model_name;                                     \
    }

// ============================================================
// BaseModel<TDerived> — CRTP ORM base
// ============================================================
/**
 * Concrete models must:
 *   1. Inherit: class Foo : public BaseModel<Foo>
 *   2. Declare: ODOO_MODEL("foo.bar", "foo_bar")
 *   3. Implement registerFields(), serializeFields(), deserializeFields()
 *   4. Optionally override validate()
 *
 * serializeFields / deserializeFields / registerFields must be PUBLIC
 * so the CRTP base can call them via static_cast.
 */
template<typename TDerived>
class BaseModel : public IModel {
public:
    explicit BaseModel(std::shared_ptr<infrastructure::DbConnection> db)
        : db_(std::move(db)), id_(0)
    {
        // Initialize the field registry exactly once per concrete model type
        // (TDerived). The local static `once` is per template specialization,
        // so BaseModel<ResPartner> and BaseModel<ResUsers> each have their own.
        // Subsequent constructor calls skip registerFields() entirely (~50 map
        // insertions saved per request under PERF-02).
        static std::once_flag once;
        std::call_once(once, [this]() {
            static_cast<TDerived*>(this)->TDerived::registerFields();
        });
    }

    // ----------------------------------------------------------
    // IModel — Record-level authorization (S-30)
    // ----------------------------------------------------------
    void setUserContext(const UserContext& ctx) override { ctx_ = ctx; }

    // ----------------------------------------------------------
    // IModel — Identity
    // ----------------------------------------------------------
    std::string name() const override { return TDerived::MODEL_NAME; }
    int         id()   const override { return id_; }
    int         getId() const         { return id_; }

    // ----------------------------------------------------------
    // IModel — Introspection
    // ----------------------------------------------------------
    nlohmann::json fieldsGet(
        const std::vector<std::string>& fields     = {},
        const std::vector<std::string>& attributes = {}) const override
    {
        return fieldRegistry_.toJson(fields, attributes);
    }

    nlohmann::json toJson() const override {
        nlohmann::json j;
        j["id"] = id_;
        static_cast<const TDerived*>(this)->serializeFields(j);
        return j;
    }

    void fromJson(const nlohmann::json& j) override {
        if (j.contains("id") && j["id"].is_number_integer())
            id_ = j["id"].get<int>();
        static_cast<TDerived*>(this)->deserializeFields(j);
    }

    // ----------------------------------------------------------
    // IModel — CRUD
    // ----------------------------------------------------------
    int create(const nlohmann::json& values) override {
        nlohmann::json vals = values;
        rejectUnknownFields_(vals);
        coerceNumericStrings_(vals);   // "330" -> 330 before the member round-trip
        const int stampedCompany = stampCompany_(vals);   // docs/094
        fromJson(vals);
        const auto errors = static_cast<TDerived*>(this)->validate();
        if (!errors.empty())
            // A missing/invalid field is a USER error, not a server fault: throw
            // ValidationError so the dispatcher returns it as a 400 with the
            // message ("Name is required") instead of a 500 "Internal Error".
            throw cerp::infrastructure::ValidationError(errors[0]);

        const auto cols = fieldRegistry_.storedColumnNames();
        nlohmann::json full = toJson();

        std::string colList, placeholders;
        pqxx::params params;
        bool first = true;
        int  idx   = 1;

        for (const auto& col : cols) {
            if (col == "id" || !full.contains(col)) continue;
            if (!first) { colList += ","; placeholders += ","; }
            colList      += col;
            placeholders += "$" + std::to_string(idx++);
            appendParam_(params, normalizeForDb_(full[col], col));
            first = false;
        }

        // docs/094: most models with a company_id column never register it as a
        // field, so the loop above — which walks the field registry — would drop
        // the stamp and insert NULL, which under the NULL-is-shared rule means
        // the record would be visible to every company. Add the column here when
        // the registry did not carry it.
        if (stampedCompany > 0 && !fieldRegistry_.has("company_id")) {
            if (!first) { colList += ","; placeholders += ","; }
            colList      += "company_id";
            placeholders += "$" + std::to_string(idx++);
            params.append(stampedCompany);
            first = false;
        }

        const std::string sql =
            "INSERT INTO " + std::string(TDerived::TABLE_NAME) +
            " (" + colList + ") VALUES (" + placeholders + ") RETURNING id";

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result res;
        try {
            res = txn.exec(sql, params);
        } catch (const pqxx::sql_error& e) {
            // A NOT NULL / CHECK violation means the user omitted a required
            // field. Surface it as a ValidationError (400 "… is required")
            // rather than a raw 500 "Internal Error". The column name is not
            // sensitive; the full SQL text (SEC-28) is never included.
            const std::string what = e.what();
            const std::string col  = notNullColumn_(what);
            if (!col.empty())
                throw cerp::infrastructure::ValidationError(
                    "The field '" + col + "' is required.");
            if (what.find("violates check constraint") != std::string::npos)
                throw cerp::infrastructure::ValidationError(
                    "A value is out of the allowed range for this record.");
            // A UNIQUE violation is the same class of thing: the user typed a
            // value that is already taken. Left as a raw 500 it reached the
            // screen as "An internal error occurred" — met while creating a
            // rental unit whose code already existed, from a dialog that could
            // then only be cancelled. The COLUMNS are named; the offending
            // VALUES are not, because SEC-28 keeps database text out of the
            // response and the user already knows what they typed.
            if (what.find("violates unique constraint") != std::string::npos) {
                const auto cols = uniqueKeyColumns_(what);
                throw cerp::infrastructure::ValidationError(
                    cols.empty()
                        ? "Another record already uses this value."
                        : "Another record already uses this " + cols + ".");
            }
            // 23P01 — an EXCLUDE constraint, or a trigger raising with that
            // code. In this schema that means overlapping date ranges: a
            // rental unit cannot be let to two tenants over the same days
            // (migration 820). The message is generic because BaseModel does
            // not know which resource it is; the screens that create these
            // check for the clash first and name the dates. Left unmapped this
            // reached the user as "An internal error occurred".
            if (what.find("conflicting key value violates exclusion constraint")
                    != std::string::npos ||
                what.find("is already let") != std::string::npos) {
                throw cerp::infrastructure::ValidationError(
                    "Those dates overlap a period this is already booked for. "
                    "Choose dates that do not overlap.");
            }
            throw;   // anything else stays a gated internal error
        }
        auto row = res.one_row();
        txn.commit();
        id_ = row[0].as<int>();
        return id_;
    }

    /**
     * The column list out of a Postgres unique-violation DETAIL line:
     *
     *     Key (code, company_id)=(A-101, 1) already exists.
     *              ^^^^^^^^^^^^^^^
     *
     * Returns "code" — company_id is dropped because it is the tenant stamp
     * (docs/094), not something the user chose, and naming it in "already uses
     * this code and company_id" only confuses. Empty when the message has no
     * DETAIL, which happens when the server locale or client_min_messages
     * suppresses it; the caller then falls back to a generic sentence.
     */
    static std::string uniqueKeyColumns_(const std::string& msg) {
        const auto a = msg.find("Key (");
        if (a == std::string::npos) return {};
        const auto b = msg.find(')', a + 5);
        if (b == std::string::npos) return {};
        const std::string raw = msg.substr(a + 5, b - (a + 5));
        std::vector<std::string> keep;
        std::size_t pos = 0;
        while (pos <= raw.size()) {
            auto comma = raw.find(',', pos);
            if (comma == std::string::npos) comma = raw.size();
            std::string col = raw.substr(pos, comma - pos);
            while (!col.empty() && col.front() == ' ') col.erase(col.begin());
            while (!col.empty() && col.back()  == ' ') col.pop_back();
            if (!col.empty() && col != "company_id") keep.push_back(col);
            pos = comma + 1;
        }
        std::string out;
        for (std::size_t i = 0; i < keep.size(); ++i) {
            if (i) out += (i + 1 == keep.size()) ? " and " : ", ";
            out += keep[i];
        }
        return out;
    }

    // Extract the column from a Postgres "null value in column \"x\" ... violates
    // not-null constraint" message; empty string if it isn't that error.
    static std::string notNullColumn_(const std::string& msg) {
        if (msg.find("not-null constraint") == std::string::npos) return {};
        const auto a = msg.find("column \"");
        if (a == std::string::npos) return {};
        const auto b = msg.find('"', a + 8);
        if (b == std::string::npos) return {};
        return msg.substr(a + 8, b - (a + 8));
    }

    nlohmann::json read(const std::vector<int>&         ids,
                        const std::vector<std::string>& fields = {}) override {
        if (ids.empty()) return nlohmann::json::array();
        const std::string cols = buildSelectCols_(fields);
        pqxx::params params;
        params.append(idsToArray_(ids));                           // $1
        std::string sql =
            "SELECT " + cols + " FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE id = ANY($1::int[])";

        // S-30: inject record-rule filter after the ids param ($2+)
        appendRuleClause_(sql, params, RuleOp::Read, 1);
        appendCompanyClause_(sql);   // docs/094

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto res = txn.exec(sql, params);
        return rowsToJson_(res);
    }

    bool write(const std::vector<int>&  ids,
               const nlohmann::json&    values) override {
        // OCC: work on a mutable copy so we can strip the concurrency sentinel
        nlohmann::json vals = values;
        coerceNumericStrings_(vals);   // "330" -> 330 so scaled fields scale

        // OCC: extract __expected_write_date before building SET clause
        std::string expectedWd;
        if (vals.contains("__expected_write_date")) {
            if (vals["__expected_write_date"].is_string())
                expectedWd = vals["__expected_write_date"].get<std::string>();
            vals.erase("__expected_write_date");
        }

        rejectUnknownFields_(vals);

        if (ids.empty() || vals.empty()) return true;

        std::string setClause;
        pqxx::params params;
        int idx = 1;
        bool first = true;

        for (auto it = vals.begin(); it != vals.end(); ++it) {
            if (!fieldRegistry_.has(it.key())) continue;
            if (!first) setClause += ",";
            setClause += it.key() + "=$" + std::to_string(idx++);
            appendParam_(params, normalizeForDb_(it.value(), it.key()));
            first = false;
        }

        // OCC: always stamp write_date = now() if the model tracks it
        if (fieldRegistry_.has("write_date")) {
            if (!setClause.empty()) setClause += ",";
            setClause += "write_date=now()";
        }

        if (setClause.empty()) return true;

        params.append(idsToArray_(ids));                           // $idx (ids)
        std::string sql =
            "UPDATE " + std::string(TDerived::TABLE_NAME) +
            " SET " + setClause +
            " WHERE id = ANY($" + std::to_string(idx) + "::int[])";

        // OCC: add write_date guard before rule clause so paramCount stays accurate
        int paramCount = idx;   // total $N already bound
        if (!expectedWd.empty()) {
            ++paramCount;
            params.append(expectedWd);
            sql += " AND write_date = $" + std::to_string(paramCount);
        }

        // S-30: inject record-rule filter after all explicit params
        appendRuleClause_(sql, params, RuleOp::Write, paramCount);
        appendCompanyClause_(sql);   // docs/094 — an id from another company
                                     // must not be updatable by guessing it

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        const auto res = txn.exec(sql, params);
        txn.commit();

        // OCC: 0 rows affected + expected write_date → concurrent modification
        if (!expectedWd.empty() && res.affected_rows() == 0)
            throw infrastructure::ConcurrencyConflictException(
                "Record was modified by another user. "
                "Please reload and re-apply your changes.");
        return true;
    }

    bool unlink(const std::vector<int>& ids) override {
        if (ids.empty()) return true;
        pqxx::params params;
        params.append(idsToArray_(ids));                           // $1
        std::string sql =
            "DELETE FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE id = ANY($1::int[])";

        // S-30: inject record-rule filter after the ids param
        appendRuleClause_(sql, params, RuleOp::Unlink, 1);
        appendCompanyClause_(sql);   // docs/094 — likewise for delete

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(sql, params);
        txn.commit();
        return true;
    }

    // ----------------------------------------------------------
    // IModel — Search
    // ----------------------------------------------------------
    std::vector<int> search(const nlohmann::json& domainJson,
                            int limit = 0, int offset = 0,
                            const std::string& order = "") override {
        validateOrder_(order);
        // S-30: merge rule domain into user domain before compiling SQL
        const nlohmann::json merged = mergeRuleDomain_(domainJson, RuleOp::Read);
        auto [where, paramVec] = domainFromJson(merged).toSql(&filterableColumns_());
        std::string sql =
            "SELECT id FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE " + where;
        appendCompanyClause_(sql);   // docs/094
        if (!order.empty()) sql += " ORDER BY " + order;
        if (limit  > 0)     sql += " LIMIT "  + std::to_string(limit);
        if (offset > 0)     sql += " OFFSET " + std::to_string(offset);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result res;
        if (paramVec.empty()) {
            res = txn.exec(sql);
        } else {
            pqxx::params p; for (auto& s : paramVec) p.append(s);
            res = txn.exec(sql, p);
        }
        std::vector<int> ids;
        for (const auto& row : res) ids.push_back(row[0].as<int>());
        return ids;
    }

    // PERF-F: maximum rows returned by any single searchRead call.
    // Prevents accidental or malicious full-table scans from the HTTP API.
    // Set high enough for real use (countries=250, states=700) but bounded.
    static constexpr int kMaxPageSize = 1000;

    nlohmann::json searchRead(const nlohmann::json&           domainJson,
                               const std::vector<std::string>& fields = {},
                               int limit = 0, int offset = 0,
                               const std::string& order = "") override {
        validateOrder_(order);
        // PERF-F: cap to prevent unbounded queries (limit==0 means "no limit" from caller)
        if (limit <= 0 || limit > kMaxPageSize) limit = kMaxPageSize;
        // S-30: merge rule domain into user domain before compiling SQL
        const nlohmann::json merged = mergeRuleDomain_(domainJson, RuleOp::Read);
        auto [where, paramVec] = domainFromJson(merged).toSql(&filterableColumns_());
        const std::string cols = buildSelectCols_(fields);
        std::string sql =
            "SELECT " + cols + " FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE " + where;
        appendCompanyClause_(sql);   // docs/094
        if (!order.empty()) sql += " ORDER BY " + order;
        if (limit  > 0)     sql += " LIMIT "  + std::to_string(limit);
        if (offset > 0)     sql += " OFFSET " + std::to_string(offset);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result res;
        if (paramVec.empty()) {
            res = txn.exec(sql);
        } else {
            pqxx::params p; for (auto& s : paramVec) p.append(s);
            res = txn.exec(sql, p);
        }
        return rowsToJson_(res);
    }

    int searchCount(const nlohmann::json& domainJson) override {
        // S-30: merge rule domain
        const nlohmann::json merged = mergeRuleDomain_(domainJson, RuleOp::Read);
        auto [where, paramVec] = domainFromJson(merged).toSql(&filterableColumns_());
        std::string sql =
            "SELECT COUNT(*) FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE " + where;
        appendCompanyClause_(sql);   // docs/094 — a count must not reveal rows
                                     // the same domain would refuse to return
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result res;
        if (paramVec.empty()) {
            res = txn.exec(sql);
        } else {
            pqxx::params p; for (auto& s : paramVec) p.append(s);
            res = txn.exec(sql, p);
        }
        return res[0][0].as<int>();
    }

    // ----------------------------------------------------------
    // read_group — grouped aggregation (docs/095)
    // ----------------------------------------------------------
    /**
     * @brief Aggregate rows into groups, the reference ERP's `read_group` contract.
     *
     * This is the primitive the whole reporting surface stands on: grouped
     * lists, the pivot, the graph and the kanban board are all read_group with
     * a different renderer. Before it existed, every one of those had to be a
     * bespoke screen with its own hand-written SQL — which is exactly why the
     * dashboards in this codebase are bespoke screens with hand-written SQL.
     *
     * @param domainJson  filter, same as search
     * @param fieldsJson  fields to aggregate: numeric ones get SUM
     * @param groupbyJson one or more group keys. A date field may carry a
     *                    granularity — "date:month" — as the reference ERP spells it.
     * @param limit/offset/orderBy paging over the GROUPS, not the rows.
     *
     * Returns one object per group carrying the group key, `__count`, the
     * aggregates, and `__domain` — the filter that selects exactly this
     * group's rows, so a client can drill in without reconstructing it.
     */
    nlohmann::json readGroup(const nlohmann::json& domainJson,
                             const nlohmann::json& fieldsJson,
                             const nlohmann::json& groupbyJson,
                             int limit = 0, int offset = 0,
                             const std::string& orderBy = "") override {
        // ---- resolve the group keys ------------------------------------
        struct GroupKey {
            std::string field;      // registered column
            std::string interval;   // "" | day | week | month | quarter | year
            std::string expr;       // SQL expression to GROUP BY
            std::string alias;      // result column name
        };
        std::vector<GroupKey> keys;
        auto addKey = [&](const std::string& spec) {
            std::string field = spec, interval;
            const auto colon = spec.find(':');
            if (colon != std::string::npos) {
                field    = spec.substr(0, colon);
                interval = spec.substr(colon + 1);
            }
            if (!fieldRegistry_.has(field))
                throw cerp::infrastructure::ValidationError("Unknown group-by field: " + field);
            const auto& fd = fieldRegistry_.get(field);
            GroupKey k;
            k.field    = field;
            k.interval = interval;
            k.alias    = "g" + std::to_string(keys.size());
            if (!interval.empty()) {
                // Only these five, and matched exactly — the value reaches
                // date_trunc as SQL text, so it can never be caller-supplied.
                static const std::set<std::string> kIntervals =
                    {"day", "week", "month", "quarter", "year"};
                if (!kIntervals.count(interval))
                    throw cerp::infrastructure::ValidationError(
                        "Unsupported group-by interval: " + interval);
                k.expr = "date_trunc('" + interval + "', " + field + ")";
            } else if (fd.type == FieldType::Date || fd.type == FieldType::Datetime) {
                k.expr = "date_trunc('month', " + field + ")";
                k.interval = "month";
            } else {
                k.expr = field;
            }
            keys.push_back(std::move(k));
        };
        if (groupbyJson.is_string()) addKey(groupbyJson.get<std::string>());
        else if (groupbyJson.is_array())
            for (const auto& g : groupbyJson)
                if (g.is_string()) addKey(g.get<std::string>());
        if (keys.empty())
            throw cerp::infrastructure::ValidationError("read_group needs at least one group-by field.");

        // ---- resolve the measures --------------------------------------
        // the reference ERP sends the whole field list; only the numeric ones can be summed,
        // and silently skipping the rest is what the client expects.
        struct Measure { std::string field, alias; bool scaled; };
        std::vector<Measure> measures;
        auto addMeasure = [&](std::string spec) {
            // "amount_total:sum" — the aggregate suffix is accepted and ignored;
            // SUM is the only one the client ever asks for here.
            const auto colon = spec.find(':');
            if (colon != std::string::npos) spec = spec.substr(0, colon);
            if (spec == "id" || !fieldRegistry_.has(spec)) return;
            const auto& fd = fieldRegistry_.get(spec);
            const bool numeric = fd.type == FieldType::Integer || fd.type == FieldType::Float ||
                                 fd.type == FieldType::Monetary;
            if (!numeric) return;
            for (const auto& k : keys) if (k.field == spec) return;   // never sum a group key
            for (const auto& m : measures) if (m.field == spec) return;
            measures.push_back({spec, "m" + std::to_string(measures.size()), fd.scaled});
        };
        if (fieldsJson.is_array())
            for (const auto& f : fieldsJson) if (f.is_string()) addMeasure(f.get<std::string>());

        // ---- build the statement ---------------------------------------
        const nlohmann::json merged = mergeRuleDomain_(domainJson, RuleOp::Read);
        auto [where, paramVec] = domainFromJson(merged).toSql(&filterableColumns_());

        std::string sel, grp;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) { sel += ", "; grp += ", "; }
            sel += keys[i].expr + " AS " + keys[i].alias;
            grp += keys[i].expr;
            // Select the bucket's exclusive end next to its start so the group's
            // __domain is half-open and two adjacent buckets can never both
            // claim the same row.
            if (!keys[i].interval.empty())
                sel += ", (" + keys[i].expr + " + interval '" +
                       bucketStep_(keys[i].interval) + "') AS " + keys[i].alias + "_end";
        }
        sel += ", COUNT(*) AS __count";
        for (const auto& m : measures)
            sel += ", SUM(" + m.field + ") AS " + m.alias;

        std::string sql = "SELECT " + sel + " FROM " + std::string(TDerived::TABLE_NAME) +
                          " WHERE " + where;
        appendCompanyClause_(sql);          // docs/094 — groups must not span companies
        sql += " GROUP BY " + grp;

        // ORDER BY over groups: an aggregate alias or a group key, nothing else.
        std::string order;
        if (!orderBy.empty()) {
            std::string col = orderBy, dir = "ASC";
            const auto sp = orderBy.find(' ');
            if (sp != std::string::npos) {
                col = orderBy.substr(0, sp);
                dir = lowerAscii_(orderBy.substr(sp + 1)) == "desc" ? "DESC" : "ASC";
            }
            if (col == "__count") order = " ORDER BY __count " + dir;
            for (const auto& k : keys) if (k.field == col) order = " ORDER BY " + k.alias + " " + dir;
            for (const auto& m : measures) if (m.field == col) order = " ORDER BY " + m.alias + " " + dir;
        }
        if (order.empty()) order = " ORDER BY " + keys[0].alias + " ASC NULLS LAST";
        sql += order;
        if (limit  > 0) sql += " LIMIT "  + std::to_string(limit);
        if (offset > 0) sql += " OFFSET " + std::to_string(offset);

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        pqxx::result res;
        if (paramVec.empty()) res = txn.exec(sql);
        else { pqxx::params p; for (auto& s : paramVec) p.append(s); res = txn.exec(sql, p); }

        // ---- shape the reply -------------------------------------------
        // Many2one keys are resolved to [id, display_name] in one extra query
        // per key rather than one per row.
        std::map<std::string, std::map<int, std::string>> labels;
        for (const auto& k : keys) {
            const auto& fd = fieldRegistry_.get(k.field);
            if (fd.type != FieldType::Many2one || fd.relation.empty()) continue;
            std::set<int> ids;
            const auto ci = res.column_number(k.alias);
            for (const auto& row : res)
                if (!row[ci].is_null()) ids.insert(row[ci].template as<int>(0));
            if (ids.empty()) continue;
            labels[k.field] = resolveM2oLabels_(txn, fd.relation, ids);
        }

        // Resolve column positions once. Indexing a row by name inside this
        // template also makes `as<>` a dependent name, which is a needless
        // fight with the compiler for something that is faster done by index.
        struct KeyCol { std::size_t idx; std::size_t endIdx; bool hasEnd; };
        std::vector<KeyCol> keyCols;
        for (const auto& k : keys) {
            KeyCol kc{res.column_number(k.alias), 0, !k.interval.empty()};
            if (kc.hasEnd) kc.endIdx = res.column_number(k.alias + "_end");
            keyCols.push_back(kc);
        }
        std::vector<std::size_t> measureCols;
        for (const auto& m : measures) measureCols.push_back(res.column_number(m.alias));
        const auto countCol = res.column_number("__count");

        nlohmann::json out = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json g = nlohmann::json::object();
            nlohmann::json gdom = nlohmann::json::array();

            for (std::size_t i = 0; i < keys.size(); ++i) {
                const auto& k  = keys[i];
                const auto& fd = fieldRegistry_.get(k.field);
                const auto& f  = row[keyCols[i].idx];
                const std::string outKey = k.interval.empty() ? k.field
                                                              : (k.field + ":" + k.interval);
                if (f.is_null()) {
                    g[outKey] = false;
                    gdom.push_back({k.field, "=", nullptr});
                } else if (fd.type == FieldType::Many2one) {
                    const int id = f.template as<int>(0);
                    const auto it = labels.find(k.field);
                    std::string lbl = (it != labels.end() && it->second.count(id))
                                          ? it->second.at(id) : ("#" + std::to_string(id));
                    g[outKey] = nlohmann::json::array({id, lbl});
                    gdom.push_back({k.field, "=", id});
                } else if (!k.interval.empty()) {
                    // date_trunc returns a timestamp; the client wants the bucket
                    // start plus the half-open range it can filter on.
                    std::string v = f.c_str();
                    if (v.size() >= 10) v = v.substr(0, 10);
                    const auto& fe = row[keyCols[i].endIdx];
                    std::string endv = fe.is_null() ? std::string{} : std::string(fe.c_str());
                    if (endv.size() >= 10) endv = endv.substr(0, 10);
                    g[outKey] = v;
                    gdom.push_back({k.field, ">=", v});
                    if (!endv.empty()) gdom.push_back({k.field, "<", endv});
                } else if (fd.type == FieldType::Boolean) {
                    const bool b = f.template as<bool>(false);
                    g[outKey] = b;
                    gdom.push_back({k.field, "=", b});
                } else {
                    const std::string v = f.c_str();
                    g[outKey] = v;
                    gdom.push_back({k.field, "=", v});
                }
            }

            g["__count"] = row[countCol].template as<long long>(0);
            for (std::size_t i = 0; i < measures.size(); ++i) {
                const auto& f = row[measureCols[i]];
                if (f.is_null())            g[measures[i].field] = 0;
                else if (measures[i].scaled)
                    g[measures[i].field] = Money::fromMicros(f.template as<long long>(0)).toJson();
                else                        g[measures[i].field] = f.template as<double>(0);
            }
            g["__domain"] = std::move(gdom);
            out.push_back(std::move(g));
        }
        return out;
    }

    // ----------------------------------------------------------
    // Hooks — must be PUBLIC for CRTP static_cast to reach them
    // ----------------------------------------------------------
    virtual void registerFields()                               = 0;
    virtual void serializeFields(nlohmann::json& j)   const    = 0;
    virtual void deserializeFields(const nlohmann::json& j)    = 0;
    virtual std::vector<std::string> validate() const { return {}; }

protected:
    std::shared_ptr<infrastructure::DbConnection> db_;
    int           id_  = 0;
    UserContext   ctx_;   ///< S-30: set by GenericViewModel before each CRUD call
    // One FieldRegistry shared across all instances of the same concrete model type.
    // Populated once via call_once in the constructor; thereafter read-only.
    inline static FieldRegistry fieldRegistry_{};

private:
    // ── S-49: filterable-column allowlist ──────────────────────
    //
    // The set a domain may filter on: the model's stored columns. Built
    // once per model. The rule-domain merge appends leaves too, but those
    // name registered fields by construction, so one allowlist covers
    // both the user's leaves and the rule's.
    static const std::set<std::string>& filterableColumns_() {
        static const std::set<std::string> cols = [] {
            const auto v = fieldRegistry_.storedColumnNames();
            return std::set<std::string>(v.begin(), v.end());
        }();
        return cols;
    }

    // ── read_group helpers (docs/095) ──────────────────────────

    static std::string lowerAscii_(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    /// The SQL interval that advances one bucket. Used to select the bucket's
    /// exclusive end alongside its start, so PostgreSQL does the calendar
    /// arithmetic — month lengths and leap years are not worth reimplementing.
    static const char* bucketStep_(const std::string& interval) {
        if (interval == "day")     return "1 day";
        if (interval == "week")    return "1 week";
        if (interval == "quarter") return "3 months";
        if (interval == "year")    return "1 year";
        return "1 month";
    }

    /// Resolve display names for a set of many2one ids in one round trip.
    /// Falls back to the id when the target has no obvious label column, which
    /// keeps a group readable rather than blank.
    std::map<int, std::string> resolveM2oLabels_(pqxx::transaction_base& txn,
                                                 const std::string& model,
                                                 const std::set<int>& ids) const {
        std::map<int, std::string> out;
        if (ids.empty()) return out;
        std::string table;
        for (char c : model) table += (c == '.') ? '_' : c;
        try {
            // Pick the first label-ish column this table actually has.
            // display_name first: where a table has one it is the label the
            // rest of the UI shows, and a group header reading "Carol" while
            // every picker reads "Carol, Big Carrots" is the same record
            // wearing two names. Only res_partner has the column today, so
            // nothing else changes.
            static const std::vector<std::string> kCandidates =
                {"display_name", "name", "complete_name", "code", "login"};
            std::string labelCol;
            for (const auto& c : kCandidates) {
                auto r = txn.exec(
                    "SELECT 1 FROM information_schema.columns "
                    "WHERE table_schema='public' AND table_name=$1 AND column_name=$2",
                    pqxx::params{table, c});
                if (!r.empty()) { labelCol = c; break; }
            }
            if (labelCol.empty()) return out;
            std::string idList;
            for (int id : ids) { if (!idList.empty()) idList += ","; idList += std::to_string(id); }
            auto r = txn.exec("SELECT id, " + txn.quote_name(labelCol) +
                              " FROM " + txn.quote_name(table) +
                              " WHERE id IN (" + idList + ")");
            for (const auto& row : r)
                out[row[0].as<int>()] = row[1].is_null() ? "" : row[1].c_str();
        } catch (const std::exception&) {
            // A missing or unreadable relation must not fail the whole grouping.
        }
        return out;
    }

    // ── docs/094: multi-company scoping ────────────────────────
    //
    // Every table that carries a company_id is filtered to the caller's ACTIVE
    // company, plus rows whose company_id IS NULL — those are shared records
    // (a product available to the whole group, a country, a currency), which is
    // the same convention the reference ERP uses.
    //
    // This deliberately does NOT go through ir.rule, for two reasons:
    //
    //  1. RuleEngine::buildRuleDomain returns immediately for ctx.isAdmin, so a
    //     company rule expressed as an ir.rule would not apply to the admin —
    //     which is the account nearly everyone actually uses. Company scoping
    //     that the main user bypasses is not scoping.
    //  2. Rules are per-model rows someone has to remember to add. This applies
    //     to every model with a company_id automatically, so a new module cannot
    //     forget to opt in.
    //
    // It is applied on read, search, write AND unlink. Read-side filtering alone
    // would still let a caller who guesses an id modify or delete another
    // company's row, because write/unlink address rows by id, not by domain.
    //
    // uid == 0 means an internal caller (migrations, cron, startup seeding) that
    // never had a session; those legitimately operate across companies. Every
    // HTTP path is authenticated before it reaches a model, so uid == 0 is not
    // reachable from outside.
    // Whether this model's TABLE has a company_id column.
    //
    // Deliberately not `fieldRegistry_.has("company_id")`. The registry lists
    // the fields a model chose to declare, and most models with a company_id
    // column never declare it — res.partner is one — so keying on the registry
    // left scoping switched off for exactly the models that carry the column.
    // The table is the ground truth; one catalogue query answers it for every
    // model at once and is cached for the process lifetime.
    static const std::set<std::string>& companyTables_(infrastructure::DbConnection& db) {
        static const std::set<std::string> tables = [&db] {
            std::set<std::string> t;
            try {
                auto conn = db.acquire();
                pqxx::work txn{conn.get()};
                for (const auto& r : txn.exec(
                         "SELECT table_name FROM information_schema.columns "
                         "WHERE table_schema='public' AND column_name='company_id'"))
                    t.insert(r[0].c_str());
            } catch (const std::exception&) {
                // Leave empty; hasCompanyColumn_ falls back to the registry.
            }
            return t;
        }();
        return tables;
    }

    bool hasCompanyColumn_() const {
        if (!db_) return false;
        const auto& t = companyTables_(*db_);
        if (t.empty()) return fieldRegistry_.has("company_id");   // catalogue unavailable
        return t.count(std::string(TDerived::TABLE_NAME)) > 0;
    }

    // The context to scope by. ctx_ is what the ViewModel handed us; when it is
    // anonymous we fall back to the request-scoped CurrentUser, so a ViewModel
    // that forgets to call setUserContext cannot switch company scoping off.
    const UserContext& scopeCtx_() const {
        return ctx_.uid > 0 ? ctx_ : CurrentUser::get();
    }

    bool companyScoped_() const {
        return scopeCtx_().uid > 0 && hasCompanyColumn_();
    }

    // SQL fragment, already parenthesised, or "" when no scoping applies.
    // The company id is a C++ int taken from the session, never client text, so
    // interpolating it cannot inject; doing so keeps every caller's $N numbering
    // untouched, which is what makes this safe to bolt onto five different
    // query builders.
    std::string companyClause_() const {
        if (!companyScoped_()) return {};
        const int cid = scopeCtx_().companyId;
        if (cid > 0)
            return "(company_id IS NULL OR company_id = " + std::to_string(cid) + ")";
        // Authenticated but with no company: they may only ever see shared rows.
        return "(company_id IS NULL)";
    }

    // Appends " AND (...)" to a WHERE that already has at least one condition.
    void appendCompanyClause_(std::string& sql) const {
        const std::string c = companyClause_();
        if (!c.empty()) sql += " AND " + c;
    }

    // Decide the company of a record being created.
    //
    // Unstamped, a new record would land with company_id NULL, which under the
    // NULL-is-shared rule above means "visible to every company" — so simply
    // not filling the field in would quietly publish it group-wide. Records
    // therefore belong to the company that created them unless someone says
    // otherwise, and only an admin can say otherwise.
    /// Returns the company the new row must carry, or 0 for "leave it NULL"
    /// (a shared record, or no scoping in force). Also validates any
    /// company_id the caller supplied.
    int stampCompany_(nlohmann::json& vals) const {
        if (!companyScoped_()) return 0;
        const UserContext& u = scopeCtx_();

        const bool supplied = vals.contains("company_id") &&
                              !vals["company_id"].is_null() &&
                              !(vals["company_id"].is_boolean() && !vals["company_id"].get<bool>());

        if (supplied) {
            int want = 0;
            if (vals["company_id"].is_number()) want = vals["company_id"].get<int>();
            else if (vals["company_id"].is_string()) {
                try { want = std::stoi(vals["company_id"].get<std::string>()); } catch (...) { want = 0; }
            }
            // Writing into a company you are not a member of is how records get
            // planted where their owner cannot see them; refuse rather than
            // silently rewrite, so the caller learns the request was wrong.
            if (want > 0 && want != u.companyId && !u.mayUseCompany(want))
                throw cerp::infrastructure::ValidationError(
                    "You cannot create records for another company.");
            return want > 0 ? want : u.companyId;
        }

        // Explicit null/false = "shared across all companies". Reserved for
        // admins; for anyone else an omitted company means their own.
        const bool explicitlyShared =
            vals.contains("company_id") &&
            (vals["company_id"].is_null() ||
             (vals["company_id"].is_boolean() && !vals["company_id"].get<bool>()));
        if (explicitlyShared && u.isAdmin) { vals.erase("company_id"); return 0; }

        if (u.companyId > 0) {
            vals["company_id"] = u.companyId;
            return u.companyId;
        }
        return 0;
    }

    // ── S-30: Record-rule helpers ──────────────────────────────

    // Returns the user domain merged with applicable ir.rule domains (implicit AND).
    // Used by search / searchRead / searchCount which compile a full domain to SQL.
    nlohmann::json mergeRuleDomain_(const nlohmann::json& userDomain,
                                     RuleOp op) const {
        if (!RuleEngine::ready()) return userDomain;
        const auto ruleDomain = RuleEngine::instance().buildRuleDomain(
            TDerived::MODEL_NAME, op, ctx_);
        if (ruleDomain.empty()) return userDomain;
        // Concatenate: domainFromJson treats a flat list as implicit AND
        nlohmann::json merged = userDomain;
        for (const auto& item : ruleDomain) merged.push_back(item);
        return merged;
    }

    // Appends a rule WHERE clause to an existing SQL string, with $N parameters
    // offset so they follow the already-bound params.
    // `existingParamCount` = number of $N placeholders already in sql.
    void appendRuleClause_(std::string&  sql,
                            pqxx::params& params,
                            RuleOp        op,
                            int           existingParamCount) const {
        if (!RuleEngine::ready()) return;
        const auto ruleDomain = RuleEngine::instance().buildRuleDomain(
            TDerived::MODEL_NAME, op, ctx_);
        if (ruleDomain.empty()) return;
        auto [rClause, rParams] = domainFromJson(ruleDomain).toSql();
        if (rClause == "TRUE" || rClause.empty()) return;
        sql += " AND " + offsetParams_(rClause, existingParamCount);
        for (auto& s : rParams) params.append(s);
    }

    // Shift all $N placeholders in a SQL string by `offset`.
    // Safe because we generated the string ourselves (no user content in it).
    static std::string offsetParams_(const std::string& sql, int offset) {
        if (offset == 0) return sql;
        std::string result;
        result.reserve(sql.size() + 8);
        for (std::size_t i = 0; i < sql.size(); ) {
            if (sql[i] == '$' && i + 1 < sql.size() && std::isdigit(sql[i + 1])) {
                std::size_t j = i + 1;
                while (j < sql.size() && std::isdigit(sql[j])) ++j;
                const int n = std::stoi(sql.substr(i + 1, j - i - 1));
                result += '$';
                result += std::to_string(n + offset);
                i = j;
            } else {
                result += sql[i++];
            }
        }
        return result;
    }

    // ── Existing private helpers ───────────────────────────────

    // Validates that every column name in a (regex-sanitized) ORDER BY clause
    // actually exists in this model's field registry.  "id" is always valid.
    // Throws std::invalid_argument for unknown columns, preventing information
    // leakage via sort order on fields not exposed in the SELECT list.
    /// A write the model cannot honour must FAIL, not succeed quietly.
    ///
    /// Both paths used to drop unknown keys without a word: write() skipped
    /// anything `fieldRegistry_.has()` did not recognise, and create() only ever
    /// read the keys deserializeFields asked for. Either way the caller got back
    /// a fresh id and a success it had not earned, with the field discarded.
    ///
    /// That is how "create a contact with parent_id" appeared to work for months
    /// while linking nothing — res.partner had no such column, the key was
    /// dropped, and the API answered {"result": 768}. A typo in a field name
    /// behaved the same way: silent data loss reported as success. An error here
    /// costs a minute; the silence cost an afternoon.
    ///
    /// Exempt: `id`, and anything `__`-prefixed — the OCC sentinel
    /// `__expected_write_date` and its kin are protocol, not model fields.
    void rejectUnknownFields_(const nlohmann::json& vals) const {
        if (!vals.is_object()) return;
        for (auto it = vals.begin(); it != vals.end(); ++it) {
            const std::string& k = it.key();
            if (k == "id" || k.rfind("__", 0) == 0) continue;
            if (fieldRegistry_.has(k)) continue;
            throw cerp::infrastructure::ValidationError(
                "Unknown field '" + k + "' on " + std::string(TDerived::MODEL_NAME));
        }
    }

    void validateOrder_(const std::string& order) const {
        if (order.empty()) return;
        std::istringstream ss(order);
        std::string token;
        while (std::getline(ss, token, ',')) {
            std::size_t start = token.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            std::size_t end = token.find_first_of(" \t", start);
            const std::string col = (end == std::string::npos)
                ? token.substr(start)
                : token.substr(start, end - start);
            if (col != "id" && !fieldRegistry_.has(col))
                throw std::invalid_argument("Unknown order field: " + col);
        }
    }

    std::string buildSelectCols_(const std::vector<std::string>& fields) const {
        if (fields.empty()) {
            const auto cols = fieldRegistry_.storedColumnNames();
            std::string s;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                if (i) s += ","; s += cols[i];
            }
            return s;
        }
        std::string s = "id";
        for (const auto& f : fields)
            if (f != "id" && fieldRegistry_.has(f)) s += "," + f;
        return s;
    }

    // HTML <input type="number"> sends its value as a STRING ("330"), and so do
    // CSV import and many API clients. Coerce those to JSON numbers for every
    // registered NUMERIC field, up front, so both write paths agree:
    //   • create() round-trips through typed members whose deserializeFields do
    //     `is_number()` checks — a string was dropped, leaving the 0 default;
    //   • write()/create() then scale via normalizeForDb_, which needs a number.
    // Without this, keying 330 into a rate stored 0 (create) or 0.00033 (write).
    // Non-numeric strings and non-registered keys are left untouched.
    void coerceNumericStrings_(nlohmann::json& v) const {
        if (!v.is_object()) return;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!it.value().is_string() || !fieldRegistry_.has(it.key())) continue;
            const auto& fd = fieldRegistry_.get(it.key());
            const bool numeric = fd.scaled
                              || fd.type == FieldType::Monetary
                              || fd.type == FieldType::Float
                              || fd.type == FieldType::Integer;
            if (!numeric) continue;
            const std::string s = it.value().get<std::string>();
            if (s.empty()) {                       // cleared numeric field -> 0
                it.value() = (fd.type == FieldType::Integer)
                                 ? nlohmann::json(0) : nlohmann::json(0.0);
                continue;
            }
            try {
                std::size_t pos = 0;
                if (fd.type == FieldType::Integer) {
                    long long n = std::stoll(s, &pos);
                    if (pos == s.size()) it.value() = n;
                } else {
                    double d = std::stod(s, &pos);
                    if (pos == s.size()) it.value() = d;
                }
            } catch (...) { /* not numeric — leave as-is, let validation reject */ }
        }
    }

    // the reference ERP JSON uses `false` for null FK/integer values and `[id,"Name"]` arrays
    // for set Many2one values.  Neither is accepted by PostgreSQL for integer
    // columns — normalise them before binding.
    nlohmann::json normalizeForDb_(const nlohmann::json& val,
                                   const std::string&    col) const {
        if (!fieldRegistry_.has(col)) return val;
        const auto& fdef = fieldRegistry_.get(col);

        // false → NULL for every non-boolean field
        if (val.is_boolean() && !val.get<bool>() &&
            fdef.type != FieldType::Boolean)
            return nlohmann::json(nullptr);

        // 0 → NULL for Many2one fields (0 is not a valid FK id)
        if (fdef.type == FieldType::Many2one &&
            val.is_number_integer() && val.get<int>() == 0)
            return nlohmann::json(nullptr);

        // [id, "Name"] → id  (Many2one display tuple)
        if (fdef.type == FieldType::Many2one &&
            val.is_array() && !val.empty() && val[0].is_number_integer())
            return nlohmann::json(val[0].get<int>());

        // P2 write boundary: incoming JSON carries MAJOR units; the column is
        // BIGINT micro-units. Convert here so every write path — create(),
        // write(), CSV import — is covered in one place. Money::fromJson
        // rounds at scale 6, so 0.1 arriving as 0.09999999999999999 still
        // lands on exactly 100000 micros.
        //
        // MUST handle numeric STRINGS as well as JSON numbers: HTML <input
        // type="number"> yields e.target.value as a STRING ("330"), and so do
        // CSV import and many API clients. A string skipped this conversion,
        // landing "330" in the BIGINT column raw — read back ÷1e6 = 0.00033,
        // the factor-of-a-million bug. Scale the string too.
        if (fdef.scaled) {
            if (val.is_number())
                return nlohmann::json(core::Money::fromJson(val.get<double>()).toDb());
            if (val.is_string()) {
                const std::string s = val.get<std::string>();
                if (s.empty()) return nlohmann::json(static_cast<long long>(0));  // cleared -> 0
                try { return nlohmann::json(core::Money::parse(s).toDb()); }
                catch (...) { /* not a number — leave as-is, let the DB reject it */ }
            }
        }

        return val;
    }

    static void appendParam_(pqxx::params& p, const nlohmann::json& v) {
        if (v.is_null())    p.append(nullptr);
        else if (v.is_boolean()) p.append(v.get<bool>());
        // P2: must be long long, not int. Scaled columns arrive here as
        // micro-units, so RM 2,148 is already 2,148,000,000 — past INT32_MAX.
        // get<int>() would have silently truncated every amount above ~2,147.
        else if (v.is_number_integer()) p.append(v.get<long long>());
        else if (v.is_number_float())   p.append(v.get<double>());
        else if (v.is_string()) p.append(v.get<std::string>());
        else p.append(v.dump());
    }

    static std::string idsToArray_(const std::vector<int>& ids) {
        std::string s = "{";
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i) s += ","; s += std::to_string(ids[i]);
        }
        return s + "}";
    }

    /**
     * @brief P2 read boundary: BIGINT micro-units → major units for JSON.
     *
     * After migrations 901–960 a money column is BIGINT, so the OID dispatch
     * below would emit 250000000 where the client expects 250.00 — every
     * amount wrong by a factor of a million. Converting here keeps the wire
     * format identical to before the migration, which is what lets the 69
     * frontend money sites stay untouched (docs/047 §3).
     *
     * Deliberately NOT static: it needs the field registry to know which
     * columns are scaled.
     */
    nlohmann::json rowsToJson_(const pqxx::result& res) const {
        // PostgreSQL built-in OIDs used for zero-exception type dispatch
        static constexpr pqxx::oid OID_BOOL    = 16;
        static constexpr pqxx::oid OID_INT2    = 21;
        static constexpr pqxx::oid OID_INT4    = 23;
        static constexpr pqxx::oid OID_INT8    = 20;
        static constexpr pqxx::oid OID_OID     = 26;
        static constexpr pqxx::oid OID_FLOAT4  = 700;
        static constexpr pqxx::oid OID_FLOAT8  = 701;
        static constexpr pqxx::oid OID_NUMERIC = 1700;

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& row : res) {
            nlohmann::json obj;
            for (const auto& field : row) {
                const std::string col = field.name();
                if (field.is_null()) { obj[col] = nullptr; continue; }
                const pqxx::oid oid = field.type();
                if (oid == OID_BOOL) {
                    const char* s = field.c_str();
                    obj[col] = (s[0] == 't' || s[0] == '1');
                    continue;
                }
                if (oid == OID_INT2 || oid == OID_INT4 || oid == OID_INT8 || oid == OID_OID) {
                    // P2: a scaled column is micro-units — return major units.
                    if (oid == OID_INT8 && fieldRegistry_.isScaled(col)) {
                        obj[col] = core::Money::fromMicros(field.as<long long>()).toJson();
                        continue;
                    }
                    obj[col] = field.as<long long>(); continue;
                }
                if (oid == OID_FLOAT4 || oid == OID_FLOAT8 || oid == OID_NUMERIC) {
                    obj[col] = field.as<double>(); continue;
                }
                obj[col] = field.c_str();
            }
            arr.push_back(std::move(obj));
        }
        return arr;
    }
};

} // namespace cerp::core