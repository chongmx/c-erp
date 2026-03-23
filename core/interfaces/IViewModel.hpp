#pragma once
#include <nlohmann/json.hpp>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

namespace odoo::core {

// ============================================================
// CallKwArgs — parsed JSON-RPC call_kw payload
// ============================================================
/**
 * @brief Structured representation of an Odoo JSON-RPC call_kw request.
 *
 * Odoo's frontend sends:
 * @code
 * {
 *   "model":  "res.partner",
 *   "method": "search_read",
 *   "args":   [[["active","=",true]]],
 *   "kwargs": { "fields": ["name","email"], "limit": 80 }
 * }
 * @endcode
 *
 * JsonRpcDispatcher parses the raw JSON into this struct and passes it
 * to the matching ViewModel's callKw().
 */
struct CallKwArgs {
    std::string   model;   ///< Odoo model name, e.g. "res.partner"
    std::string   method;  ///< Method name, e.g. "search_read", "create"
    nlohmann::json args;   ///< Positional arguments (JSON array)
    nlohmann::json kwargs; ///< Keyword arguments (JSON object)

    // Convenience accessors ---------------------------------------------------

    /** @brief kwargs["fields"] or empty array if not present. */
    std::vector<std::string> fields() const {
        if (kwargs.contains("fields") && kwargs["fields"].is_array())
            return kwargs["fields"].get<std::vector<std::string>>();
        return {};
    }

    /** @brief kwargs["limit"] or 0 if not present. */
    int limit() const {
        return kwargs.value("limit", 0);
    }

    /** @brief kwargs["offset"] or 0 if not present. */
    int offset() const {
        return kwargs.value("offset", 0);
    }

    /**
     * @brief kwargs["order"] or empty string if not present.
     *
     * Validates syntax before returning:
     *   col_name [ASC|DESC] [, col_name [ASC|DESC]] ...
     *
     * Hand-written parser — no regex, no heap allocation beyond the return
     * value.  Throws std::invalid_argument on anything that doesn't match
     * (semicolons, quotes, parens, UNION injections, etc. are all rejected).
     */
    std::string order() const {
        const std::string raw = kwargs.value("order", std::string{});
        if (raw.empty()) return raw;

        const char* p   = raw.c_str();
        const char* end = p + raw.size();

        auto skipSpaces = [&]{ while (p < end && (*p == ' ' || *p == '\t')) ++p; };

        // Case-insensitive match for a literal keyword
        auto matchWord = [&](const char* kw) -> bool {
            std::size_t n = std::strlen(kw);
            if (static_cast<std::size_t>(end - p) < n) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (std::toupper(static_cast<unsigned char>(p[i])) !=
                    static_cast<unsigned char>(kw[i]))               return false;
            // Must not be followed by an identifier char
            if (p + n < end && (std::isalnum(static_cast<unsigned char>(p[n])) || p[n] == '_'))
                return false;
            p += n;
            return true;
        };

        while (p < end) {
            skipSpaces();
            // Require identifier start: [a-zA-Z_]
            if (p >= end || (!std::isalpha(static_cast<unsigned char>(*p)) && *p != '_'))
                throw std::invalid_argument("Invalid order clause: " + raw);
            // Consume identifier: [a-zA-Z0-9_]*
            while (p < end && (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_'))
                ++p;
            // Optional ASC / DESC
            skipSpaces();
            if (matchWord("ASC") || matchWord("DESC")) {}
            skipSpaces();
            // Expect comma or end-of-string
            if (p < end) {
                if (*p != ',')
                    throw std::invalid_argument("Invalid order clause: " + raw);
                ++p; // consume comma
            }
        }
        return raw;
    }

    /** @brief args[0] as domain, or empty array if not present. */
    nlohmann::json domain() const {
        if (args.is_array() && !args.empty()) return args[0];
        return nlohmann::json::array();
    }

    /** @brief args[0] as record ids, or empty array if not present. */
    std::vector<int> ids() const {
        if (args.is_array() && !args.empty() && args[0].is_array())
            return args[0].get<std::vector<int>>();
        return {};
    }

    /** @brief args[N] as a JSON value, or null if out-of-range. */
    nlohmann::json arg(std::size_t n) const {
        if (args.is_array() && n < args.size()) return args[n];
        return nlohmann::json{};
    }
};


// ============================================================
// IViewModel
// ============================================================
/**
 * @brief Interface every ViewModel must satisfy.
 *
 * ViewModels are the request-handling layer. For each incoming JSON-RPC
 * call_kw, JsonRpcDispatcher:
 *   1. Looks up the ViewModel by model name via ViewModelFactory.
 *   2. Calls callKw() with the parsed arguments.
 *   3. Returns the JSON result directly to the HTTP response.
 *
 * ViewModels own:
 *   - Dispatching method names to typed handler functions.
 *   - Translating raw JSON args/kwargs into typed service calls.
 *   - Formatting service results into Odoo-compatible JSON responses.
 *
 * They do NOT own:
 *   - Business logic (delegate to IService).
 *   - Database access (delegate through IService → IModel).
 *   - View rendering (delegate to IView via ViewFactory).
 *
 * Lifetime: Transient — one instance per request; no cross-request state.
 *
 * Method registration is done in BaseViewModel via REGISTER_METHOD macro,
 * which builds a compile-time dispatch table.
 */
class IViewModel {
public:
    virtual ~IViewModel() = default;

    // ----------------------------------------------------------
    // Identity
    // ----------------------------------------------------------

    /**
     * @brief Odoo model name this ViewModel handles, e.g. "res.partner".
     * Must match the key used at ViewModelFactory registration.
     */
    virtual std::string modelName() const = 0;

    // ----------------------------------------------------------
    // Dispatch
    // ----------------------------------------------------------

    /**
     * @brief Dispatch an Odoo JSON-RPC call_kw request.
     *
     * The dispatcher calls this for every request whose model name matches
     * this ViewModel. Implementations typically delegate to a method dispatch
     * table built by BaseViewModel::REGISTER_METHOD.
     *
     * @param call  Parsed call_kw arguments.
     * @returns     JSON result in Odoo wire format.
     * @throws      std::runtime_error if method is unknown or arguments invalid.
     */
    virtual nlohmann::json callKw(const CallKwArgs& call) = 0;

    // ----------------------------------------------------------
    // Introspection
    // ----------------------------------------------------------

    /**
     * @brief Return the list of method names this ViewModel handles.
     *
     * Used by the dispatcher to validate requests early and by developer
     * tooling to enumerate available methods without calling them.
     */
    virtual std::vector<std::string> supportedMethods() const = 0;

    /**
     * @brief Return true if this ViewModel can handle the given method.
     * Default: linear scan of supportedMethods(). Override for O(1) lookup.
     */
    virtual bool supportsMethod(const std::string& method) const {
        for (const auto& m : supportedMethods())
            if (m == method) return true;
        return false;
    }
};

} // namespace odoo::core