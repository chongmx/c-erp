#pragma once
// ============================================================
// modules/base/RecordRuleSql.hpp  —  S-30 record rules for CUSTOM reads
//
// BaseModel threads ir.rule filtering into every CRUD path (mergeRuleDomain_
// / appendRuleClause_). Custom viewmodel search_reads build raw SQL by hand
// and never went through BaseModel, so record rules were SILENTLY UNENFORCED
// on them (071 §1.2 / 068 §1.2). This helper closes that: call it once per
// custom read to append the applicable ir.rule filter.
//
// It emits the rule as an ID-MEMBERSHIP SUBQUERY:
//
//     AND <idExpr> IN (SELECT id FROM <table> WHERE <ir.rule domain>)
//
// rather than concatenating the rule's leaves into the outer WHERE. That is
// deliberate: the outer query aliases and JOINs other tables (e.g. the
// picking read LEFT JOINs res_partner, which ALSO has company_id), so a bare
// `company_id = $N` in the outer WHERE would be AMBIGUOUS and fail. The
// subquery's FROM is the single base table, so its bare columns resolve
// unambiguously, whatever the outer query looks like.
//
// No-op when: RuleEngine not ready, user is admin, or the model has no active
// rule for this op (buildRuleDomain returns []). Safe to call on models that
// have no rules today — it costs one buildRuleDomain lookup and emits nothing.
// ============================================================
#include "RuleEngine.hpp"
#include "Domain.hpp"
#include "UserContext.hpp"
#include <pqxx/pqxx>
#include <cctype>
#include <string>

namespace cerp::core {

// Shift every $N placeholder in `sql` up by `offset` (so a self-contained
// clause can be spliced after params that are already bound). The string is
// engine-generated — no user text — so a plain scan is safe.
inline std::string offsetSqlParams(const std::string& sql, int offset) {
    if (offset == 0) return sql;
    std::string result;
    result.reserve(sql.size() + 8);
    for (std::size_t i = 0; i < sql.size();) {
        if (sql[i] == '$' && i + 1 < sql.size() &&
            std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
            std::size_t j = i + 1;
            while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
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

// Append the ir.rule filter for (model, op, user) to `sql`/`params`, as an
// id-membership subquery keyed off `idExpr` (e.g. "sp.id") against `table`
// (e.g. "stock_picking"). `existingParamCount` is the number of $N already
// bound in `sql`. Returns the new total param count.
//
// MUST be called after the outer WHERE text and BEFORE any ORDER BY / LIMIT.
inline int appendRecordRuleSubquery(std::string&       sql,
                                    pqxx::params&      params,
                                    const std::string& model,
                                    RuleOp             op,
                                    const UserContext& ctx,
                                    const std::string& table,
                                    const std::string& idExpr,
                                    int                existingParamCount) {
    if (!RuleEngine::ready()) return existingParamCount;
    const auto ruleDomain = RuleEngine::instance().buildRuleDomain(model, op, ctx);
    if (ruleDomain.empty()) return existingParamCount;
    auto res = domainFromJson(ruleDomain).toSql();  // no allowlist: system-generated
    if (res.clause.empty() || res.clause == "TRUE") return existingParamCount;
    sql += " AND " + idExpr + " IN (SELECT id FROM " + table + " WHERE " +
           offsetSqlParams(res.clause, existingParamCount) + ")";
    for (auto& s : res.params) params.append(s);
    return existingParamCount + static_cast<int>(res.params.size());
}

} // namespace cerp::core
