#pragma once
// ============================================================
// core/RuleEngine.hpp  —  S-30: Record-Level Authorization
//
// Singleton service that implements Odoo-style ir.rule record
// filtering.  Loaded once at startup from IrModule::initialize();
// called automatically by BaseModel before every CRUD operation.
//
// Odoo rule semantics:
//   Global rules  (global=true)  — subtractive: ALL must match
//   Group rules   (global=false) — additive:    user needs ≥1 group match
//   Final filter  = AND(global_rules) AND OR(matching_group_rules)
//
// Admin users (isAdmin=true) bypass all rules.
// Models with no active rules are unrestricted (returns []).
//
// Variable substitution in domain_force JSON:
//   "user.id"         → ctx.uid
//   "user.company_id" → ctx.companyId
//   "user.partner_id" → ctx.partnerId
// ============================================================
#include "DbConnection.hpp"
#include "TtlCache.hpp"
#include "UserContext.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace odoo::core {

enum class RuleOp { Read, Write, Create, Unlink };

class RuleEngine {
public:
    // ── Lifecycle ─────────────────────────────────────────────
    /// Call once at application startup (IrModule::initialize()).
    static void initialize(std::shared_ptr<infrastructure::DbConnection> db);

    /// Returns the singleton. Throws if not initialized.
    static RuleEngine& instance();

    /// True after initialize() has been called.
    static bool ready();

    // ── Core API (called by BaseModel) ────────────────────────
    /// Build the combined domain filter for (model, op, user).
    /// Returns [] when no restriction applies (admin, no rules).
    /// Returns a non-empty domain array when filtering is required.
    nlohmann::json buildRuleDomain(const std::string& modelName,
                                    RuleOp             op,
                                    const UserContext&  ctx) const;

    // ── Cache management ──────────────────────────────────────
    /// Invalidate cached rules for one model (call after ir.rule writes).
    void invalidate(const std::string& modelName);
    void invalidateAll();

private:
    struct IrRuleRecord {
        int              id;
        std::string      name;
        std::string      modelName;
        nlohmann::json   domainForce;
        bool             permRead;
        bool             permWrite;
        bool             permCreate;
        bool             permUnlink;
        bool             globalRule;  ///< true = no group check (applies to all)
        bool             active;
        std::vector<int> groupIds;
    };

    explicit RuleEngine(std::shared_ptr<infrastructure::DbConnection> db);

    std::vector<IrRuleRecord> loadRules_(const std::string& modelName) const;
    nlohmann::json substituteVars_(nlohmann::json domain,
                                    const UserContext& ctx) const;
    static bool    appliesToOp_(const IrRuleRecord& r, RuleOp op);

    // Domain combinators
    static nlohmann::json domainAnd_(const nlohmann::json& d1,
                                      const nlohmann::json& d2);
    static nlohmann::json domainOr_(const nlohmann::json& d1,
                                     const nlohmann::json& d2);

    std::shared_ptr<infrastructure::DbConnection>           db_;
    mutable infrastructure::TtlCache<std::string,
                std::vector<IrRuleRecord>>                  cache_;

    static std::once_flag              s_once_;
    static std::unique_ptr<RuleEngine> s_instance_;
};

} // namespace odoo::core
