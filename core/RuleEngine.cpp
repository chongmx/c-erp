// ============================================================
// core/RuleEngine.cpp  —  S-30: Record-Level Authorization
// ============================================================
#include "RuleEngine.hpp"
#include <pqxx/pqxx>
#include <sstream>
#include <stdexcept>

namespace odoo::core {

// ── Static members ────────────────────────────────────────────
std::once_flag              RuleEngine::s_once_;
std::unique_ptr<RuleEngine> RuleEngine::s_instance_;

// ── Lifecycle ─────────────────────────────────────────────────
void RuleEngine::initialize(std::shared_ptr<infrastructure::DbConnection> db) {
    std::call_once(s_once_, [&db]() {
        s_instance_.reset(new RuleEngine(std::move(db)));
    });
}

RuleEngine& RuleEngine::instance() {
    if (!s_instance_)
        throw std::runtime_error("RuleEngine: not initialized");
    return *s_instance_;
}

bool RuleEngine::ready() { return s_instance_ != nullptr; }

RuleEngine::RuleEngine(std::shared_ptr<infrastructure::DbConnection> db)
    : db_(std::move(db)) {}

void RuleEngine::invalidate(const std::string& modelName) {
    cache_.invalidate(modelName);
}
void RuleEngine::invalidateAll() { cache_.invalidateAll(); }

// ── buildRuleDomain ───────────────────────────────────────────
nlohmann::json RuleEngine::buildRuleDomain(const std::string& modelName,
                                             RuleOp             op,
                                             const UserContext&  ctx) const {
    // Admins bypass all record rules (mirrors Odoo superuser behaviour)
    if (ctx.isAdmin) return nlohmann::json::array();

    const auto rules = loadRules_(modelName);

    // Collect rules that apply to this operation and are active
    std::vector<const IrRuleRecord*> applicable;
    for (const auto& r : rules) {
        if (!r.active || !appliesToOp_(r, op)) continue;
        applicable.push_back(&r);
    }
    if (applicable.empty()) return nlohmann::json::array();  // no restriction

    std::vector<nlohmann::json> globalDomains;
    std::vector<nlohmann::json> groupDomains;

    for (const auto* r : applicable) {
        nlohmann::json dom = substituteVars_(r->domainForce, ctx);
        if (r->globalRule) {
            globalDomains.push_back(std::move(dom));
        } else {
            // Group rule: only applies if user belongs to at least one group
            bool matches = false;
            for (int gid : r->groupIds)
                if (ctx.hasGroup(gid)) { matches = true; break; }
            if (matches) groupDomains.push_back(std::move(dom));
            // If user matches none of the rule's groups → rule is invisible to
            // this user (Odoo "additive" group rule semantics)
        }
    }

    // AND all global domains
    nlohmann::json result = nlohmann::json::array();
    for (const auto& d : globalDomains)
        result = domainAnd_(result, d);

    // OR all matching group domains, then AND with global result
    if (!groupDomains.empty()) {
        nlohmann::json grp = groupDomains[0];
        for (std::size_t i = 1; i < groupDomains.size(); ++i)
            grp = domainOr_(grp, groupDomains[i]);
        result = domainAnd_(result, grp);
    }

    return result;
}

// ── loadRules_ ────────────────────────────────────────────────
std::vector<RuleEngine::IrRuleRecord>
RuleEngine::loadRules_(const std::string& modelName) const {
    if (auto cached = cache_.get(modelName)) return *cached;

    std::vector<IrRuleRecord> rules;
    try {
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto res = txn.exec_params(
            R"(SELECT r.id, r.name, r.domain_force,
                      r.perm_read, r.perm_write, r.perm_create, r.perm_unlink,
                      r.global, r.active,
                      COALESCE(
                          array_agg(g.group_id) FILTER (WHERE g.group_id IS NOT NULL),
                          '{}'::int[]
                      ) AS group_ids
               FROM   ir_rule r
               LEFT   JOIN ir_rule_group_rel g ON g.rule_id = r.id
               WHERE  r.model_name = $1
               GROUP  BY r.id)",
            modelName);

        for (const auto& row : res) {
            IrRuleRecord rec;
            rec.id         = row[0].as<int>();
            rec.name       = row[1].c_str();
            rec.modelName  = modelName;

            try   { rec.domainForce = nlohmann::json::parse(row[2].c_str()); }
            catch (...) { rec.domainForce = nlohmann::json::array(); }

            rec.permRead   = (row[3].c_str()[0] == 't');
            rec.permWrite  = (row[4].c_str()[0] == 't');
            rec.permCreate = (row[5].c_str()[0] == 't');
            rec.permUnlink = (row[6].c_str()[0] == 't');
            rec.globalRule = (row[7].c_str()[0] == 't');
            rec.active     = (row[8].c_str()[0] == 't');

            // Parse PostgreSQL array literal  "{1,2,3}"  →  vector<int>
            std::string arrStr = row[9].c_str();
            if (arrStr.size() > 2) {                // not "{}"
                arrStr = arrStr.substr(1, arrStr.size() - 2);
                std::istringstream ss(arrStr);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    try { rec.groupIds.push_back(std::stoi(tok)); } catch (...) {}
                }
            }
            rules.push_back(std::move(rec));
        }
    } catch (const std::exception&) {
        // Table may not exist yet during first-boot schema migration — treat as
        // "no rules" so startup isn't blocked.
    }

    cache_.set(modelName, rules, 60);  // 60 s TTL
    return rules;
}

// ── substituteVars_ ───────────────────────────────────────────
// Replaces "user.id", "user.company_id", "user.partner_id" in
// the value position of domain leaves [field, op, VALUE].
nlohmann::json RuleEngine::substituteVars_(nlohmann::json domain,
                                             const UserContext& ctx) const {
    if (!domain.is_array()) return domain;
    for (auto& item : domain) {
        if (!item.is_array() || item.size() != 3) continue;
        auto& val = item[2];
        if (!val.is_string()) continue;
        const auto s = val.get<std::string>();
        if      (s == "user.id")          val = ctx.uid;
        else if (s == "user.company_id")  val = ctx.companyId;
        else if (s == "user.partner_id")  val = ctx.partnerId;
    }
    return domain;
}

// ── appliesToOp_ ──────────────────────────────────────────────
bool RuleEngine::appliesToOp_(const IrRuleRecord& r, RuleOp op) {
    switch (op) {
        case RuleOp::Read:   return r.permRead;
        case RuleOp::Write:  return r.permWrite;
        case RuleOp::Create: return r.permCreate;
        case RuleOp::Unlink: return r.permUnlink;
    }
    return false;
}

// ── domainAnd_ ────────────────────────────────────────────────
// Concatenates two JSON domain arrays (implicit AND at the top level).
// domainFromJson treats a flat list of leaves as AND, so this is correct
// for any list of simple leaf expressions.
nlohmann::json RuleEngine::domainAnd_(const nlohmann::json& d1,
                                       const nlohmann::json& d2) {
    if (!d1.is_array() || d1.empty()) return d2;
    if (!d2.is_array() || d2.empty()) return d1;
    nlohmann::json result = d1;
    for (const auto& item : d2) result.push_back(item);
    return result;
}

// ── domainOr_ ─────────────────────────────────────────────────
// Combines two JSON domains with OR using Odoo prefix notation:
//   ["|", ...d1_items, ...d2_items]
// Multi-leaf sub-domains are wrapped with "&" grouping operators so
// the stack-based parser in domainFromJson handles them correctly.
//
// Example: OR( [["active","=",true],["uid","=",5]], [["uid","=",7]] )
//   → ["|", "&", ["active","=",true], ["uid","=",5], ["uid","=",7]]
// Trace (reversed): uid=7, uid=5, active=T, "&", "|"
//   push uid=7; push uid=5; push active=T
//   "&" → pop active=T, pop uid=5 → push AND(active,uid=5)
//   "|" → pop AND(...), pop uid=7 → push OR(AND, uid=7)  ✓
nlohmann::json RuleEngine::domainOr_(const nlohmann::json& d1,
                                      const nlohmann::json& d2) {
    if (!d1.is_array() || d1.empty()) return d2;
    if (!d2.is_array() || d2.empty()) return d1;
    nlohmann::json result = nlohmann::json::array();
    result.push_back("|");
    // Add grouping "&" operators for multi-leaf d1
    for (std::size_t i = 0; i + 1 < d1.size(); ++i) result.push_back("&");
    for (const auto& item : d1) result.push_back(item);
    // Add grouping "&" operators for multi-leaf d2
    for (std::size_t i = 0; i + 1 < d2.size(); ++i) result.push_back("&");
    for (const auto& item : d2) result.push_back(item);
    return result;
}

} // namespace odoo::core
