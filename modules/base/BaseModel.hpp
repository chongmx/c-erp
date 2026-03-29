#pragma once
#include "IModel.hpp"
#include "FieldRegistry.hpp"
#include "Domain.hpp"
#include "DbConnection.hpp"
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
        fromJson(values);
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
        const std::string sql  =
            "SELECT " + cols + " FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE id = ANY($1::int[])";

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        auto res = txn.exec(sql, pqxx::params{idsToArray_(ids)});
        return rowsToJson_(res);
    }

    bool write(const std::vector<int>&  ids,
               const nlohmann::json&    values) override {
        if (ids.empty() || values.empty()) return true;

        std::string setClause;
        pqxx::params params;
        int idx = 1;
        bool first = true;

        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!fieldRegistry_.has(it.key())) continue;
            if (!first) setClause += ",";
            setClause += it.key() + "=$" + std::to_string(idx++);
            appendParam_(params, normalizeForDb_(it.value(), it.key()));
            first = false;
        }
        if (setClause.empty()) return true;

        params.append(idsToArray_(ids));
        const std::string sql =
            "UPDATE " + std::string(TDerived::TABLE_NAME) +
            " SET " + setClause +
            " WHERE id = ANY($" + std::to_string(idx) + "::int[])";

        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(sql, params);
        txn.commit();
        return true;
    }

    bool unlink(const std::vector<int>& ids) override {
        if (ids.empty()) return true;
        const std::string sql =
            "DELETE FROM " + std::string(TDerived::TABLE_NAME) +
            " WHERE id = ANY($1::int[])";
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};
        txn.exec(sql, pqxx::params{idsToArray_(ids)});
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
        auto [where, paramVec] = domainFromJson(domainJson).toSql();
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

    nlohmann::json searchRead(const nlohmann::json&           domainJson,
                               const std::vector<std::string>& fields = {},
                               int limit = 0, int offset = 0,
                               const std::string& order = "") override {
        validateOrder_(order);
        auto [where, paramVec] = domainFromJson(domainJson).toSql();
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
        auto [where, paramVec] = domainFromJson(domainJson).toSql();
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
    int           id_ = 0;
    // One FieldRegistry shared across all instances of the same concrete model type.
    // Populated once via call_once in the constructor; thereafter read-only.
    inline static FieldRegistry fieldRegistry_{};

private:
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

        return val;
    }

    static void appendParam_(pqxx::params& p, const nlohmann::json& v) {
        if (v.is_null())    p.append(nullptr);
        else if (v.is_boolean()) p.append(v.get<bool>());
        else if (v.is_number_integer()) p.append(v.get<int>());
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

    static nlohmann::json rowsToJson_(const pqxx::result& res) {
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