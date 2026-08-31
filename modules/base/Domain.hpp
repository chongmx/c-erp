#pragma once
#include <nlohmann/json.hpp>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace cerp::core {

// ============================================================
// Domain
// ============================================================
/**
 * @brief Represents an the reference ERP domain expression as a typed C++ value.
 *
 * An the reference ERP domain is a JSON array of leaves and logical operators:
 * @code
 *   [["active","=",true], "|", ["name","ilike","acme"], ["email","ilike","acme"]]
 * @endcode
 *
 * This class stores the raw JSON domain and provides:
 *   - domainFromJson()   — parse a JSON array into a Domain
 *   - toSql()           — compile to a parameterised WHERE clause
 *
 * The SQL compiler covers the operators the reference ERP's OWL frontend commonly sends.
 * Unsupported operators throw std::runtime_error at compile time, not silently.
 */

// ---------------------------------------------------------------
// DomainLeaf — a single (field, operator, value) triple
// ---------------------------------------------------------------
struct DomainLeaf {
    std::string    field;
    std::string    op;      ///< "=", "!=", "<", "<=", ">", ">=", "like", "ilike",
                            ///  "not like", "not ilike", "in", "not in", "=?", "child_of"
    nlohmann::json value;
};

// ---------------------------------------------------------------
// DomainNode — leaf | logical combinator
// ---------------------------------------------------------------
struct DomainNode {
    enum class Kind { Leaf, And, Or, Not };
    Kind kind = Kind::And;

    // For Leaf
    DomainLeaf leaf;

    // For And / Or / Not
    std::vector<DomainNode> children;

    static DomainNode makeLeaf(DomainLeaf l) {
        DomainNode n; n.kind = Kind::Leaf; n.leaf = std::move(l); return n;
    }
    static DomainNode makeAnd(std::vector<DomainNode> ch) {
        DomainNode n; n.kind = Kind::And; n.children = std::move(ch); return n;
    }
    static DomainNode makeOr(std::vector<DomainNode> ch) {
        DomainNode n; n.kind = Kind::Or;  n.children = std::move(ch); return n;
    }
    static DomainNode makeNot(DomainNode child) {
        DomainNode n; n.kind = Kind::Not; n.children = {std::move(child)}; return n;
    }
};

// ---------------------------------------------------------------
// Domain — top-level wrapper
// ---------------------------------------------------------------
class Domain {
public:
    Domain() = default;
    explicit Domain(DomainNode root) : root_(std::move(root)) {}

    bool isEmpty() const {
        return root_.kind == DomainNode::Kind::And && root_.children.empty();
    }

    const DomainNode& root() const { return root_; }

    // ----------------------------------------------------------
    // SQL compilation
    // ----------------------------------------------------------

    struct SqlResult {
        std::string              clause;  ///< WHERE clause text (uses $1,$2,… placeholders)
        std::vector<std::string> params;  ///< Bound parameter values (as strings)
    };

    /**
     * @brief Compile this domain to a parameterised SQL WHERE clause.
     *
     * @returns SqlResult with clause and params. If domain is empty,
     *          clause = "TRUE" and params is empty.
     *
     * Example:
     * @code
     *   auto [clause, params] = domain.toSql();
     *   // clause = "(name ILIKE $1 AND active = $2)"
     *   // params = {"%acme%", "true"}
     * @endcode
     */
    SqlResult toSql() const {
        return toSql(nullptr);
    }

    /**
     * @brief Compile, restricting filterable columns to an allowlist.
     *
     * @param allowed  columns a caller may filter on — normally the
     *                 model's stored fields. When non-null, a leaf naming
     *                 anything outside the set is REJECTED.
     *
     * S-49: without this, sanitizeColumn_ only charset-checked the field
     * name, so an authenticated user could filter on ANY column —
     * including `password` — and blind-extract it one `like` substring at
     * a time (the SELECT list is restricted, but the WHERE clause was
     * not). Proven with `password like 'pbkdf2'` -> 1 row vs
     * `like 'ZZZZZ'` -> 0 rows. See verify_domain_field_allowlist.sh.
     */
    SqlResult toSql(const std::set<std::string>* allowed) const {
        return toSql(allowed, std::string{});
    }

    /**
     * Compile with every column qualified by a table ALIAS.
     *
     * Needed wherever the SELECT joins more than one table: an unqualified
     * `state = $1` is ambiguous the moment stock_move is joined to
     * stock_picking, and PostgreSQL rejects the whole query. The symptom is a
     * masked "internal error" on any FILTERED read while the unfiltered one
     * works perfectly -- which is how it survived, because nothing filters in
     * a smoke test. Found by the 08-warehouse journey, against
     * Inventory -> Reporting -> Moves History.
     *
     * The alias is the caller's own literal (e.g. "sm"), never user input.
     */
    SqlResult toSql(const std::set<std::string>* allowed, const std::string& alias) const {
        if (isEmpty()) return {"TRUE", {}};
        SqlResult r;
        r.clause = compileNode_(root_, r.params, allowed, alias);
        return r;
    }

private:
    DomainNode root_;

    static std::string compileNode_(const DomainNode&           node,
                                    std::vector<std::string>&    params,
                                    const std::set<std::string>* allowed,
                                    const std::string&           alias = {}) {
        switch (node.kind) {
            case DomainNode::Kind::Leaf:
                return compileLeaf_(node.leaf, params, allowed, alias);

            case DomainNode::Kind::And: {
                if (node.children.empty()) return "TRUE";
                std::string s = "(";
                for (std::size_t i = 0; i < node.children.size(); ++i) {
                    if (i) s += " AND ";
                    s += compileNode_(node.children[i], params, allowed, alias);
                }
                return s + ")";
            }

            case DomainNode::Kind::Or: {
                if (node.children.empty()) return "FALSE";
                std::string s = "(";
                for (std::size_t i = 0; i < node.children.size(); ++i) {
                    if (i) s += " OR ";
                    s += compileNode_(node.children[i], params, allowed, alias);
                }
                return s + ")";
            }

            case DomainNode::Kind::Not:
                return "(NOT " + compileNode_(node.children[0], params, allowed, alias) + ")";
        }
        return "TRUE";
    }

    static std::string compileLeaf_(const DomainLeaf&            leaf,
                                    std::vector<std::string>&    params,
                                    const std::set<std::string>* allowed,
                                    const std::string&           alias = {}) {
        // Qualified AFTER the allowlist check, so an alias can never become a
        // way of smuggling an unregistered column past it.
        const std::string bare = sanitizeColumn_(leaf.field, allowed);
        const std::string col  = alias.empty() ? bare : (alias + "." + bare);
        const std::string op  = leaf.op;

        auto placeholder = [&]() -> std::string {
            params.push_back(jsonToSqlParam_(leaf.value));
            return "$" + std::to_string(params.size());
        };

        if (op == "=" || op == "=?") {
            if (leaf.value.is_null())
                return col + " IS NULL";
            // Boolean false is a real value (NOT NULL columns), not "IS NULL"
            return col + " = " + placeholder();
        }
        if (op == "!=") {
            if (leaf.value.is_null())
                return col + " IS NOT NULL";
            return col + " != " + placeholder();
        }
        if (op == "<")  return col + " < "  + placeholder();
        if (op == "<=") return col + " <= " + placeholder();
        if (op == ">")  return col + " > "  + placeholder();
        if (op == ">=") return col + " >= " + placeholder();

        if (op == "like") {
            params.push_back("%" + jsonToSqlParam_(leaf.value) + "%");
            return col + " LIKE $" + std::to_string(params.size());
        }
        if (op == "ilike") {
            params.push_back("%" + jsonToSqlParam_(leaf.value) + "%");
            return col + " ILIKE $" + std::to_string(params.size());
        }
        if (op == "not like") {
            params.push_back("%" + jsonToSqlParam_(leaf.value) + "%");
            return col + " NOT LIKE $" + std::to_string(params.size());
        }
        if (op == "not ilike") {
            params.push_back("%" + jsonToSqlParam_(leaf.value) + "%");
            return col + " NOT ILIKE $" + std::to_string(params.size());
        }

        if (op == "in" || op == "not in") {
            if (!leaf.value.is_array() || leaf.value.empty())
                return op == "in" ? "FALSE" : "TRUE";
            std::string list = "(";
            for (std::size_t i = 0; i < leaf.value.size(); ++i) {
                if (i) list += ",";
                params.push_back(jsonToSqlParam_(leaf.value[i]));
                list += "$" + std::to_string(params.size());
            }
            list += ")";
            return col + (op == "in" ? " IN " : " NOT IN ") + list;
        }

        throw std::runtime_error("Domain: unsupported operator '" + op + "'");
    }

    static std::string sanitizeColumn_(const std::string&           field,
                                        const std::set<std::string>* allowed) {
        // Charset check first — defence in depth even behind the allowlist,
        // and the only guard when no allowlist is supplied.
        for (char c : field)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                throw std::runtime_error("Domain: invalid field name '" + field + "'");

        // S-49: reject any column that is not a registered, stored field.
        // Without this an authenticated user can filter on `password` and
        // extract it blind. `allowed` is null only for internal callers
        // that build domains from trusted, fixed field names.
        if (allowed && !allowed->count(field))
            throw std::runtime_error("Domain: field '" + field +
                                     "' is not filterable on this model");
        return field;  // pqxx binds the VALUE; the column name is now allowlisted
    }

    static std::string jsonToSqlParam_(const nlohmann::json& v) {
        if (v.is_string())  return v.get<std::string>();
        if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
        if (v.is_number())  return v.dump();
        if (v.is_null())    return "";
        return v.dump();
    }
};

// ============================================================
// domainFromJson — parse a raw the reference ERP domain JSON array
// ============================================================
/**
 * @brief Parse a JSON domain expression into a typed Domain.
 *
 * Implements the standard the reference ERP domain stack-based parser:
 *   - Strings "&", "|", "!" are logical operators.
 *   - Arrays of 3 elements are leaves: [field, op, value].
 *   - Default implicit AND between all top-level leaves.
 *
 * @param j  JSON array, e.g. [["name","ilike","acme"],["active","=",true]]
 * @returns  Parsed Domain. Empty JSON array → isEmpty() Domain.
 */
inline Domain domainFromJson(const nlohmann::json& j) {
    if (!j.is_array() || j.empty())
        return Domain{};

    // Stack-based the reference ERP domain parser
    // Each element is either an operator string or a 3-element leaf array.
    std::vector<DomainNode> stack;

    auto parseLeaf = [](const nlohmann::json& item) -> DomainNode {
        if (!item.is_array() || item.size() != 3)
            throw std::runtime_error("domainFromJson: leaf must be [field, op, value]");
        DomainLeaf leaf;
        leaf.field = item[0].get<std::string>();
        leaf.op    = item[1].get<std::string>();
        leaf.value = item[2];
        return DomainNode::makeLeaf(leaf);
    };

    // Process in reverse (stack reduces from right)
    std::vector<nlohmann::json> items(j.begin(), j.end());
    std::reverse(items.begin(), items.end());

    for (const auto& item : items) {
        if (item.is_string()) {
            const auto op = item.get<std::string>();
            if (op == "&") {
                if (stack.size() < 2) throw std::runtime_error("domainFromJson: '&' needs 2 operands");
                auto r = std::move(stack.back()); stack.pop_back();
                auto l = std::move(stack.back()); stack.pop_back();
                stack.push_back(DomainNode::makeAnd({std::move(l), std::move(r)}));
            } else if (op == "|") {
                if (stack.size() < 2) throw std::runtime_error("domainFromJson: '|' needs 2 operands");
                auto r = std::move(stack.back()); stack.pop_back();
                auto l = std::move(stack.back()); stack.pop_back();
                stack.push_back(DomainNode::makeOr({std::move(l), std::move(r)}));
            } else if (op == "!") {
                if (stack.empty()) throw std::runtime_error("domainFromJson: '!' needs 1 operand");
                auto operand = std::move(stack.back()); stack.pop_back();
                stack.push_back(DomainNode::makeNot(std::move(operand)));
            } else {
                throw std::runtime_error("domainFromJson: unknown operator '" + op + "'");
            }
        } else {
            stack.push_back(parseLeaf(item));
        }
    }

    if (stack.size() == 1)
        return Domain{std::move(stack[0])};

    // Multiple top-level nodes — implicit AND
    return Domain{DomainNode::makeAnd(std::move(stack))};
}

} // namespace cerp::core