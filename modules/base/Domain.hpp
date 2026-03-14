#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace odoo::core {

// ============================================================
// Domain
// ============================================================
/**
 * @brief Represents an Odoo domain expression as a typed C++ value.
 *
 * An Odoo domain is a JSON array of leaves and logical operators:
 * @code
 *   [["active","=",true], "|", ["name","ilike","acme"], ["email","ilike","acme"]]
 * @endcode
 *
 * This class stores the raw JSON domain and provides:
 *   - domainFromJson()   — parse a JSON array into a Domain
 *   - toSql()           — compile to a parameterised WHERE clause
 *
 * The SQL compiler covers the operators Odoo's OWL frontend commonly sends.
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
        if (isEmpty()) return {"TRUE", {}};
        SqlResult r;
        r.clause = compileNode_(root_, r.params);
        return r;
    }

private:
    DomainNode root_;

    static std::string compileNode_(const DomainNode&        node,
                                    std::vector<std::string>& params) {
        switch (node.kind) {
            case DomainNode::Kind::Leaf:
                return compileLeaf_(node.leaf, params);

            case DomainNode::Kind::And: {
                if (node.children.empty()) return "TRUE";
                std::string s = "(";
                for (std::size_t i = 0; i < node.children.size(); ++i) {
                    if (i) s += " AND ";
                    s += compileNode_(node.children[i], params);
                }
                return s + ")";
            }

            case DomainNode::Kind::Or: {
                if (node.children.empty()) return "FALSE";
                std::string s = "(";
                for (std::size_t i = 0; i < node.children.size(); ++i) {
                    if (i) s += " OR ";
                    s += compileNode_(node.children[i], params);
                }
                return s + ")";
            }

            case DomainNode::Kind::Not:
                return "(NOT " + compileNode_(node.children[0], params) + ")";
        }
        return "TRUE";
    }

    static std::string compileLeaf_(const DomainLeaf&        leaf,
                                    std::vector<std::string>& params) {
        const std::string col = sanitizeColumn_(leaf.field);
        const std::string op  = leaf.op;

        auto placeholder = [&]() -> std::string {
            params.push_back(jsonToSqlParam_(leaf.value));
            return "$" + std::to_string(params.size());
        };

        if (op == "=" || op == "=?") {
            if (leaf.value.is_null() || (leaf.value.is_boolean() && !leaf.value.get<bool>()))
                return col + " IS NULL";
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

    static std::string sanitizeColumn_(const std::string& field) {
        // Allow only alphanumeric + underscore to prevent injection.
        for (char c : field)
            if (!std::isalnum(c) && c != '_')
                throw std::runtime_error("Domain: invalid field name '" + field + "'");
        return field;  // pqxx will quote if needed at execute time
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
// domainFromJson — parse a raw Odoo domain JSON array
// ============================================================
/**
 * @brief Parse a JSON domain expression into a typed Domain.
 *
 * Implements the standard Odoo domain stack-based parser:
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

    // Stack-based Odoo domain parser
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

} // namespace odoo::core