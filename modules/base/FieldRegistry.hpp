#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

namespace odoo::core {

// ============================================================
// FieldType
// ============================================================
enum class FieldType {
    Char,
    Text,
    Integer,
    Float,
    Boolean,
    Date,
    Datetime,
    Binary,
    Selection,
    Many2one,
    One2many,
    Many2many,
    Html,
    Monetary,
};

inline std::string fieldTypeName(FieldType t) {
    switch (t) {
        case FieldType::Char:      return "char";
        case FieldType::Text:      return "text";
        case FieldType::Integer:   return "integer";
        case FieldType::Float:     return "float";
        case FieldType::Boolean:   return "boolean";
        case FieldType::Date:      return "date";
        case FieldType::Datetime:  return "datetime";
        case FieldType::Binary:    return "binary";
        case FieldType::Selection: return "selection";
        case FieldType::Many2one:  return "many2one";
        case FieldType::One2many:  return "one2many";
        case FieldType::Many2many: return "many2many";
        case FieldType::Html:      return "html";
        case FieldType::Monetary:  return "monetary";
    }
    return "char";
}

// ============================================================
// FieldDef — metadata for a single model field
// ============================================================
struct FieldDef {
    std::string name;
    FieldType   type        = FieldType::Char;
    std::string string;          ///< Human-readable label
    bool        required    = false;
    bool        readonly    = false;
    bool        store       = true;
    bool        searchable  = true;
    std::string relation;        ///< For relational fields: target model name
    std::string relationField;   ///< For One2many: inverse Many2one field name

    // Selection field choices: [["value","Label"], ...]
    std::vector<std::pair<std::string,std::string>> selection;

    nlohmann::json toJson() const {
        nlohmann::json j = {
            {"type",       fieldTypeName(type)},
            {"string",     string.empty() ? name : string},
            {"required",   required},
            {"readonly",   readonly},
            {"store",      store},
            {"searchable", searchable},
        };
        if (!relation.empty())
            j["relation"] = relation;
        if (!relationField.empty())
            j["relation_field"] = relationField;
        if (!selection.empty()) {
            auto arr = nlohmann::json::array();
            for (const auto& [val, lbl] : selection)
                arr.push_back({val, lbl});
            j["selection"] = std::move(arr);
        }
        return j;
    }
};

// ============================================================
// FieldRegistry
// ============================================================
/**
 * @brief Ordered collection of FieldDef for one model.
 *
 * Used by BaseModel to:
 *   - Produce fields_get() JSON responses
 *   - Build SELECT column lists for SQL queries
 *   - Validate that incoming JSON only touches known fields
 *
 * Registration (inside BaseModel::registerFields()):
 * @code
 *   fieldRegistry_.add({ "name",  FieldType::Char,    "Name",  true });
 *   fieldRegistry_.add({ "email", FieldType::Char,    "Email" });
 *   fieldRegistry_.add({ "partner_id", FieldType::Many2one, "Partner",
 *                         false, false, true, false, "res.partner" });
 * @endcode
 */
class FieldRegistry {
public:
    void add(FieldDef def) {
        if (index_.count(def.name))
            throw std::invalid_argument(
                "FieldRegistry: duplicate field '" + def.name + "'");
        index_[def.name] = fields_.size();
        fields_.push_back(std::move(def));
    }

    bool has(const std::string& name) const {
        return index_.count(name) > 0;
    }

    const FieldDef& get(const std::string& name) const {
        return fields_.at(index_.at(name));
    }

    const std::vector<FieldDef>& all() const { return fields_; }

    /**
     * @brief Produce an Odoo-compatible fields_get() JSON object.
     *
     * @param filter  If non-empty, only include these field names.
     * @param attrs   If non-empty, only include these attribute keys per field.
     */
    nlohmann::json toJson(
        const std::vector<std::string>& filter = {},
        const std::vector<std::string>& attrs  = {}) const
    {
        nlohmann::json out = nlohmann::json::object();

        for (const auto& f : fields_) {
            if (!filter.empty()) {
                bool found = false;
                for (const auto& n : filter) if (n == f.name) { found = true; break; }
                if (!found) continue;
            }

            auto fj = f.toJson();

            if (!attrs.empty()) {
                nlohmann::json filtered = nlohmann::json::object();
                for (const auto& a : attrs)
                    if (fj.contains(a)) filtered[a] = fj[a];
                out[f.name] = std::move(filtered);
            } else {
                out[f.name] = std::move(fj);
            }
        }

        // Always include the built-in 'id' field
        if (filter.empty() || std::find(filter.begin(), filter.end(), "id") != filter.end()) {
            out["id"] = {
                {"type",       "integer"},
                {"string",     "ID"},
                {"required",   false},
                {"readonly",   true},
                {"store",      true},
                {"searchable", true},
            };
        }

        return out;
    }

    /** @brief Column name list for SELECT (store=true fields only). */
    std::vector<std::string> storedColumnNames() const {
        std::vector<std::string> cols = {"id"};
        for (const auto& f : fields_)
            if (f.store && f.type != FieldType::One2many && f.type != FieldType::Many2many)
                cols.push_back(f.name);
        return cols;
    }

private:
    std::vector<FieldDef>                    fields_;
    std::unordered_map<std::string, std::size_t> index_;
};

} // namespace odoo::core