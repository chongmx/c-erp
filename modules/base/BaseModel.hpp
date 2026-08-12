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

namespace odoo::core {

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
        coerceNumericStrings_(vals);   // "330" -> 330 before the member round-trip
        fromJson(vals);
        const auto errors = static_cast<TDerived*>(this)->validate();
        if (!errors.empty())
            throw std::runtime_error("Validation failed: " + errors[0]);

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

        const std::string sql =
            "INSERT INTO " + std::string(TDerived::TABLE_NAME) +
            " (" + colList + ") VALUES (" + placeholders + ") RETURNING id";

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto row = txn.exec(sql, params).one_row();
        txn.commit();
        id_ = row[0].as<int>();
        return id_;
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
        const std::string sql =
            "SELECT COUNT(*) FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE " + where;
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

    // Odoo JSON uses `false` for null FK/integer values and `[id,"Name"]` arrays
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

} // namespace odoo::core