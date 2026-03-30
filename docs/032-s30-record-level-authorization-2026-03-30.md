# S-30 — Record-Level Authorization — 2026-03-30

## Summary

| ID | Severity | Status | Description |
|----|----------|--------|-------------|
| S-30 | MEDIUM | **Fixed** | No record-level authorization (ir.rule equivalent) |

---

## Problem

Access control operated only at the model level ("can user X read `sale.order` at all?").
Once a user had model permission, they could read and write **every record** in that model
regardless of ownership, company, or sales team.  In Odoo this layer is provided by
`ir.rule` (record rules) which inject `WHERE` clauses into queries based on the calling
user's identity.

---

## Solution

Implemented a full Odoo-compatible record-rule engine.  The implementation follows the
same semantics as Odoo's Python `ir.rule`:

| Odoo concept | This implementation |
|---|---|
| `ir.rule` table | `ir_rule` PostgreSQL table |
| `ir_rule_group_rel` | `ir_rule_group_rel` table |
| `domain_force` | JSONB column; variables substituted at query time |
| Global rules (subtractive) | All must match — AND-combined |
| Group rules (additive) | Matching group rules OR-combined |
| Superuser bypass | `isAdmin=true` skips all rules |
| Variable tokens | `user.id`, `user.company_id`, `user.partner_id` |

---

## Architecture

### New files

| File | Purpose |
|------|---------|
| `core/UserContext.hpp` | Lightweight value type: `uid`, `companyId`, `partnerId`, `groupIds`, `isAdmin` |
| `core/RuleEngine.hpp` | Singleton class declaration |
| `core/RuleEngine.cpp` | Loads rules from DB, caches with 60 s TTL, builds combined domain |

### Modified files

| File | Change |
|------|--------|
| `core/interfaces/IModel.hpp` | Added `virtual void setUserContext(const UserContext&)` |
| `modules/base/BaseModel.hpp` | `ctx_` member; `setUserContext`; rule injection in all 5 operations |
| `modules/base/GenericViewModel.hpp` | `extractContext_()` + `proto.setUserContext()` before every call |
| `core/infrastructure/JsonRpcDispatcher.hpp` | Injects `company_id`, `partner_id`, `is_admin`, `group_ids` into `call.kwargs["context"]` |
| `modules/ir/IrModule.hpp` | Added `seedRules_()` |
| `modules/ir/IrModule.cpp` | `ir_rule` + `ir_rule_group_rel` tables; `seedRules_()`; `RuleEngine::initialize()` |
| `CMakeLists.txt` | Added `modules/ir` to include directories |

---

## Data Flow

```
HTTP request
  │
  ▼
JsonRpcDispatcher::handleCallKw_()
  │  Injects into call.kwargs["context"]:
  │    uid, company_id, partner_id, is_admin, group_ids
  │
  ▼
GenericViewModel::handleSearchRead() (and all other methods)
  │  extractContext_(call) → UserContext
  │  proto.setUserContext(ctx)
  │
  ▼
BaseModel::searchRead() / search() / searchCount()
  │  mergeRuleDomain_(userDomain, RuleOp::Read)
  │    → RuleEngine::buildRuleDomain(modelName, op, ctx)
  │    → concatenates rule domain to user domain (implicit AND)
  │
  ▼
BaseModel::read() / write() / unlink()
  │  appendRuleClause_(sql, params, op, existingParamCount)
  │    → RuleEngine::buildRuleDomain(...)
  │    → domainFromJson(ruleDomain).toSql()
  │    → offsetParams_($N → $(N+offset)) to avoid collision
  │
  ▼
PostgreSQL query (rule filter is a compiled WHERE clause)
```

---

## Rule Evaluation Logic

```
applicable = active rules for (model_name, operation)

global_domains = [rule.domain_force for rule in applicable if rule.global]
group_domains  = [rule.domain_force for rule in applicable
                  if NOT rule.global AND user.hasGroup(any rule.groupIds)]

result = AND(global_domains) AND OR(group_domains)

if applicable is empty  →  no restriction (return [])
if isAdmin              →  no restriction (return [])
```

Domain operators:
- **AND** of two domains: simple concatenation (domainFromJson treats flat list as implicit AND)
- **OR** of two domains: Odoo prefix notation `["|", ...d1_items, ...d2_items]` with `"&"` grouping for multi-leaf sub-domains

---

## Database Schema

```sql
CREATE TABLE ir_rule (
    id           SERIAL PRIMARY KEY,
    name         VARCHAR(128) NOT NULL,
    model_name   VARCHAR(128) NOT NULL,
    domain_force JSONB        NOT NULL DEFAULT '[]',
    perm_read    BOOLEAN      NOT NULL DEFAULT TRUE,
    perm_write   BOOLEAN      NOT NULL DEFAULT TRUE,
    perm_create  BOOLEAN      NOT NULL DEFAULT TRUE,
    perm_unlink  BOOLEAN      NOT NULL DEFAULT TRUE,
    global       BOOLEAN      NOT NULL DEFAULT TRUE,
    active       BOOLEAN      NOT NULL DEFAULT FALSE,  -- inactive by default
    create_date  TIMESTAMP    DEFAULT now(),
    write_date   TIMESTAMP    DEFAULT now()
);
CREATE INDEX ir_rule_model_idx ON ir_rule (model_name);

CREATE TABLE ir_rule_group_rel (
    rule_id  INTEGER NOT NULL REFERENCES ir_rule(id) ON DELETE CASCADE,
    group_id INTEGER NOT NULL,
    PRIMARY KEY (rule_id, group_id)
);
```

---

## Seeded Rules (active = FALSE by default)

| ID | Name | Model | Domain | Type |
|----|------|-------|--------|------|
| 1 | Sale Order: Personal Orders | `sale.order` | `[["user_id","=","user.id"]]` | global |
| 2 | Purchase Order: Personal RFQs | `purchase.order` | `[["user_id","=","user.id"]]` | global |
| 3 | Account Move: Own Invoices | `account.move` | `[["invoice_user_id","=","user.id"]]` | global |
| 4 | HR Employee: See Own Record | `hr.employee` | `[["user_id","=","user.id"]]` | global (read-only) |
| 5 | Stock Picking: Own Company | `stock.picking` | `[["company_id","=","user.company_id"]]` | global |

To activate a rule:
```sql
UPDATE ir_rule SET active = TRUE WHERE id = 1;
```

To create a group rule (restricts only users in a specific group):
```sql
INSERT INTO ir_rule (name, model_name, domain_force, global, active)
VALUES ('Sale: Team Orders', 'sale.order', '[["team_id","=",5]]', FALSE, TRUE);

INSERT INTO ir_rule_group_rel (rule_id, group_id)
VALUES (currval('ir_rule_id_seq'), <group_id>);
```

---

## Performance

- Rules are loaded from DB once per model name and cached with a **60-second TTL**.
- For models with no active rules, `buildRuleDomain()` returns `[]` immediately after the
  first (empty) DB query — zero overhead on all subsequent requests within the TTL window.
- Cache can be invalidated programmatically: `RuleEngine::instance().invalidate("sale.order")`
  — call this after any write to `ir_rule`.
- The rule WHERE clause is injected at the SQL level — no post-query filtering in memory.

---

## Security Notes

- Admin users (`isAdmin=true`, group id=3) bypass all rules unconditionally.
  This prevents admins from locking themselves out and matches Odoo superuser semantics.
- All rule `domain_force` variable substitution uses typed JSON values (int, not string),
  so injected values are passed as `pqxx` bound parameters — no SQL injection risk.
- Rules seeded with `active=FALSE` preserve all existing behaviour on upgrade;
  no records become inaccessible unless an admin explicitly activates a rule.
