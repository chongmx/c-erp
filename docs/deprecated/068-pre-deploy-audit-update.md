# 068 — Pre-deployment audit (update to 062)

**Date:** 2026-08-08
**Supersedes:** `062` as the current readiness view. `062`'s §1 (S-49) and its method still stand;
this re-audits the codebase after the large inventory/manufacturing/costing build (`063`–`067`).
**Method:** static sweep + live probing of the **new** code (none of `063`–`067` had been
security-reviewed) + re-scoring `062`'s findings. Findings are proved, not asserted.

---

## 0. Headline

- **One S-49-class gap found and fixed** this audit (LOW severity): two custom `search_read`s
  (`stock.picking`, `stock.move`) compiled the user domain **without** the field allowlist. Same
  class as the original HIGH S-49, but these tables hold no secret columns, so it was
  defence-in-depth, not an active leak. Now allowlisted; regression green.
- **One latent access-control finding, not fixed** (LOW now / MEDIUM under multi-company):
  custom `search_read`s bypass the `RuleEngine`, so record rules (`ir.rule`) are not enforced on
  them. Today this affects exactly one defined rule — *"Stock Picking: Own Company"* — which is a
  **no-op under single-company**. The four user-scoped rules are on Generic/BaseModel models and
  **are** enforced. See §1.2.
- **The rest of the new code is clean.** No new HTTP routes (everything goes through the central
  ACL dispatcher), no `std::system`/shell, all SQL values `$N`-bound, all 12 new models gated in
  the deny-by-default ACL at the correct group, and every new exception is gated behind `devMode`.
- **Feature coverage grew enormously** and is honestly catalogued in §2. The inventory/
  manufacturing side is now essentially complete; the real remaining gaps are a parts catalogue,
  `product.supplierinfo`, second-tier accounting, and SMTP.

**Verdict: safe to deploy for the (now much larger) implemented subset, single-company.** The
one item to respect before enabling **multi-company** is the record-rule bypass (§1.2).

---

## 1. Security sweep of the new code (`063`–`067`)

### 1.1 S-49 allowlist gap — FOUND + FIXED (LOW)

`Domain::toSql()` takes an optional stored-column allowlist (the `062` S-49 fix). BaseModel reads
pass it; the new `mrp.production` custom read passes it. But the pre-existing `stock.picking` and
`stock.move` custom `search_read`s called `.toSql()` **with no allowlist** — so an authenticated
inventory user could filter those lists on any *real* column of the table, not just a registered
field. Unlike the original S-49 (which leaked `res_users.password`), `stock_picking`/`stock_move`
have no sensitive columns, so exploitability was ~nil — but it is the same class and was closed:

```cpp
// stock.picking / stock.move search_read now pass an explicit field allowlist
static const std::set<std::string> kCols = { "id","name","state","picking_type_id", ... };
domainFromJson(call.domain()).toSql(&kCols);   // rejects unregistered columns pre-SQL
```

Verified by the existing `verify_domain_field_allowlist.sh` plus the full suite (stock/mrp list
filtering unchanged).

### 1.2 Record-rule bypass on custom search_reads — NOT FIXED (LOW now / MEDIUM multi-company)

`ir.rule` record rules are applied by the `RuleEngine`, which BaseModel threads into its reads
(`BaseModel.hpp:356`). **Custom viewmodel `search_read`s build raw SQL and never call it.** Five
record rules exist:

| Rule | Model | Enforced? |
|---|---|---|
| Sale Order: Personal Orders | sale.order | ✅ Generic → RuleEngine applied |
| Purchase Order: Personal RFQs | purchase.order | ✅ Generic → applied |
| Account Move: Own Invoices | account.move | ✅ applied |
| HR Employee: See Own Record | hr.employee | ✅ applied |
| **Stock Picking: Own Company** | stock.picking | ❌ **custom read → bypassed** |

Impact **today is nil**: the only bypassed rule scopes by `company_id`, and the deployment is
single-company, so it matches every row anyway. The finding is the **pattern**: any record rule
placed on `stock.picking`, `stock.move`, or the new inventory/manufacturing models (all custom
reads) will be **silently unenforced**. This becomes a real data-isolation hole the day
multi-company (`057` §3) is enabled.

> **Recommendation** (before multi-company): thread `RuleEngine::buildRuleDomain()` into the
> custom `search_read`s (merge it into the compiled domain, exactly as BaseModel does), or route
> these models' lists through BaseModel. Not required for a single-company launch.

### 1.3 Everything else swept — no holes

| Surface | State in the new code |
|---|---|
| SQL injection | All values `$N`-bound. The only string-built identifier is `"SELECT " + col` in the valuation GL poster, where `col` is one of four **hard-coded** `property_*` literals — never user input. |
| New HTTP routes | **None.** `StockModule`/`MrpModule` `registerRoutes()` are empty; every new action is a viewmodel method dispatched through `JsonRpcDispatcher` with `checkModelAccess_`. |
| Model ACL | All 12 new models are in `kRequired` — inventory ones at `INVENTORY_USER` (11), manufacturing at `MRP_USER` (13). None fall through to the base-internal default. |
| Error disclosure | New validations throw `std::runtime_error`, gated behind `devMode` by the RPC catch (SEC-25/28). No SQL/schema leak. |
| Shell / path / XSS | No `std::system`, no new file handling, no new server-rendered HTML. |

### 1.4 Minor notes (not blockers)

- **Validation messages are generic in production.** New user-facing checks ("a lot/serial is
  required", "serial must be one unit", "set the receipt", "positive quantity required") throw
  `std::runtime_error`, which `devMode`-gates to *"An internal error occurred."* They should be
  `ValidationError` (which passes through, like `AccessDeniedError`) so the operator sees *why*.
  Safe direction (no leak); a usability gap. Same pattern as the existing `mrp` mark-done.
- **Authorization crossover (by design).** An `INVENTORY_USER` can run the reorder scheduler
  (which drafts **purchase** orders and **manufacturing** orders) and cause **accounting** journal
  entries (perpetual valuation on every validated move, and landed-cost postings). This mirrors
  the reference ERP's perpetual model and is intended, but note that inventory actions have purchase/GL
  side-effects.
- **CSV formula-injection** (from `062` §1) — still unguarded, still low, still not a blocker.

---

## 2. Feature coverage — updated

### 2.1 What changed since `062`

`062` listed on-hand, valuation, lots/serials, landed costs, work orders, routings, MPS and
subcontracting as **absent**. All are now built and tested (`064`–`067`):

| Area | Now covered | Doc |
|---|---|---|
| On-hand | `stock.quant` — per (product, location, lot), reservation, inventory adjustment | 064 |
| Manufacturing | MO + work centers + BOM routing operations + work orders + subcontracting + MPS | 064 |
| Costing | valuation layers — standard / average / FIFO — with real-time GL postings | 065 |
| Traceability | lots & serial numbers + per-lot on-hand + move-history traceability | 066 |
| Landed costs | freight/duty/handling capitalised into value + GL, five split methods | 066 |
| Replenishment | reorder rules (min/max → draft PO/MO), putaway, barcode + resolver | 067 |

One of `062`'s two **dead ACL entries** is resolved: `mrp.production` is now a real model.
`product.supplierinfo` is **still a dead ACL entry** (allowlisted, no model) — strip or implement.

### 2.2 What is still genuinely absent (verified by source/DB probe)

> **Update (`069`, 2026-08-08):** five of the rows below are now **built** — `product.supplierinfo`
> (+ reorder integration), PartKeepr **PK2–PK4** (+ parametric search), **analytic accounting**,
> **bank reconciliation**, and the **barcode scanning UI**. The `product.supplierinfo` dead-ACL
> entry is resolved. See `069`. What remains genuinely absent:

| Missing | Consequence | Note |
|---|---|---|
| `account.fiscal.position` | no tax mapping per customer/region | absent |
| `ir.mail_server` (SMTP) | portal reset still hardcodes `"Welcome1"` (S-31); no emailing | deferred |
| Multi-company | single company only; see §1.2 before enabling | `057` §3 |
| Deep product-form tabs | vendor/spec/MPN lists are managed via their own menus, not embedded in the product form; its "On Hand — coming soon" placeholders are stale | `069` §5 |

~~`product.supplierinfo`~~, ~~PartKeepr **PK2–PK4**~~, ~~`account.analytic.account`~~,
~~`account.bank.statement` + reconciliation~~, ~~barcode **scanning UI**~~ — **built (`069`).**

---

## 3. Deployment readiness — re-scored

| | Item | Status |
|---|---|---|
| ✅ | S-49-class gap (§1.1) | fixed this audit |
| ✅ | `secure_cookies = True` + `SameSite=Lax` | cfg + code |
| ✅ | Test suite | 44 suites green (was 27 at `062`, 40 at this audit; +4 in `069`) |
| ✅ | `mrp.production` dead ACL entry | resolved (real model now) |
| ✅ | `product.supplierinfo` dead ACL entry | resolved — real model + reorder integration (`069`) |
| ◐ | `devMode` off in prod | no `dev_mode` key → defaults false; confirm the server cfg |
| ☐ | **DB password still the default `odoo`** | rotate; add `system.cfg.example` + `.gitignore` |
| ☐ | Sessions in-memory (PERF-B) | still `unordered_map`; a restart logs everyone out; blocks a 2nd replica |
| ☐ | Record-rule bypass (§1.2) | fix before enabling multi-company |
| ☐ | Backup + one real restore drill | before real data |
| ☐ | SMTP / S-31 `"Welcome1"` | deferred (needs `ir.mail_server`) |
| ☐ | CSV formula-injection prefix | low |

**Cut line for a single-company production launch:** rotate the DB password, confirm `devMode`
off, install the systemd unit, prove a restore. The security posture is good — the one HIGH-class
item (S-49) is closed; the record-rule bypass is a no-op until multi-company.

---

## 4. What was verified live, not assumed

- S-49 gap reproduced in the two custom reads, fixed, full suite green afterwards.
- No new routes / no `std::system` (source grep); all 12 new models present in the ACL (probe).
- `ir_rule` has 5 rows; only the `stock.picking` (company) rule sits on a custom read; the four
  user-scoped rules are on Generic models and enforced (`BaseModel.hpp:356`).
- Absent models confirmed by `to_regclass` probe (`part_*`, `product_supplierinfo`,
  `account_analytic_account`, `account_bank_statement`, `account_fiscal_position`).
- Config probe: `secure_cookies=True`, `db_password=odoo` (default), sessions in-memory.
