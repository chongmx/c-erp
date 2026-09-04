# 071 — Pre-deployment audit (update to 068)

**Date:** 2026-08-10
**Supersedes:** `068` as the current readiness view. `068`'s method and its two live findings
(S-49 fix, record-rule bypass) still stand; this re-audits the code added *since* `068` —
`069` (vendor pricelists, PartKeepr PK2–PK4, analytic accounting, bank reconciliation + 3 OWL
UIs) and `070` (fresh-database provisioning fix) — and re-scores `068`'s open items.
**Method:** static sweep + live probing of the **new** code (none of `069` had been
security-reviewed; `070` is infra) + a full re-run of the suite + re-scoring `068`. Findings are
proved, not asserted.

---

## 0. Headline

- **No new security holes in `069`/`070`.** The new code is `$N`-bound (or integer-coerced)
  everywhere, ACL-gated deny-by-default at the right group, adds **zero HTTP routes**, has **no
  XSS** in the three OWL components (no `t-raw`/`innerHTML`; every backend call goes through the
  ACL-checked `RpcService`), and `devMode`-gates all new error paths.
- **Deployability materially improved (`070`).** The fresh-DB boot crash (v809, `ir_sequence`
  missing) is fixed — an empty database now provisions cleanly (81 tables, 37 migrations, base
  currency **MYR**, no demo data) and a read-only `tools/db_preflight.sh` gates the operator
  before start. This is the single biggest readiness gain since `068`.
- **UI now verified in a real browser.** `069` §5 could only *assert* the three custom screens
  because there was no headless browser then. This session installed one: all six screens
  (parametric search, bank reconciliation, barcode, vendor pricelists, on-hand, analytic) were
  click-tested and render with **zero console/page errors**.
- **Record-rule bypass (`068` §1.2) is still open — and its surface GREW.** `069` added four more
  custom-`search_read` models that build raw SQL and never call the `RuleEngine`. Still a **no-op
  under single-company** (no `ir.rule` targets them today); still the one item to close before
  enabling **multi-company**.
- **Top deployment blocker is now precisely characterized:** `config/system.cfg` is **committed to
  git** with `db_password = odoo` in plaintext. The deploy README documents this *and* the fix,
  but the repo still ships the default.

**Verdict: safe to deploy the (now much larger) implemented subset, single-company.** Before a
real launch: rotate + untrack the DB password and prove a restore. Before **multi-company**: close
the record-rule bypass (`§1.2`).

---

## 1. Security sweep of the new code (`069`)

### 1.1 S-49 (column allowlist) — clean in the new reads

The new viewmodels never compile a user-supplied domain: `grep` finds **no `toSql(` call** in
`ProductModule.cpp`/`AccountModule.cpp`. Their custom `search_read`s filter by a single parsed
scalar or a bound name, never a raw column:

| New custom read | Filter | Parameterization |
|---|---|---|
| `product.supplierinfo` / `part.manufacturer.info` | `product_id`, `partner_id` | `s.product_id=$N` bound |
| `part.parameter.search_parts` | name + numeric min/max | `pa.name=$N`, `value_numeric >= $N` bound |
| `account.analytic.line` | `account_id` | bound |
| `account.bank.statement.line` | `statement_id` | `l.statement_id=$1` bound |

The pre-existing `stock.picking`/`stock.move` reads keep the `068` `kCols` allowlist
(`toSql(&kCols)`, lines 443/1032). No regression.

### 1.2 Record-rule bypass — STILL OPEN, surface grew (LOW now / MEDIUM multi-company)

`ir.rule` is applied by the `RuleEngine`, which BaseModel threads into its reads. Custom
viewmodel `search_read`s build raw SQL and never call it — `grep` for `RuleEngine`/`buildRuleDomain`
across `stock`/`mrp`/`product`/`account` modules returns **nothing**. `069` added **four more**
models on this bypassed path:

| Model | Custom read? | Any `ir.rule` on it today? |
|---|---|---|
| product.supplierinfo | ✅ raw SQL | no |
| part.parameter | ✅ raw SQL | no |
| account.analytic.line | ✅ raw SQL | no |
| account.bank.statement.line | ✅ raw SQL | no |
| (068) stock.picking | ✅ raw SQL | yes — *"Own Company"*, a no-op single-company |

Impact **today is still nil** (single-company; no user-scoped rule targets these). The finding is
the *pattern*: any record rule later placed on these models is **silently unenforced**. Same
recommendation as `068` — thread `RuleEngine::buildRuleDomain()` into the custom reads (or route
them through BaseModel) **before** enabling multi-company. Not required for single-company launch.

### 1.3 Frontend (3 new OWL components) — no XSS, ACL-checked calls

`BarcodeScan.js`, `PartSearch.js`, `BankReconcile.js`:

- **No `t-raw` / `t-out` / `innerHTML`** anywhere in `web/static/src/components/` — OWL's `t-esc`
  auto-escapes, so seeded/DB text (product names, partner names, memos) cannot break out.
- Every backend call is `RpcService.call(model, method, …)` — i.e. through the central JSON-RPC
  dispatcher with `checkModelAccess_`. No raw `fetch`, `XMLHttpRequest`, or `eval`.
- The one `window.location.hash = '#action=products…&id='+id` (PartSearch → product form) is
  client-side navigation, not an injection sink.

### 1.4 ACL & routes — all new models gated, no new surface

- `registerRoutes()` is **empty (`{}`)** in all of product/account/stock/mrp — no new HTTP routes.
- Deny-by-default ACL carries every new model: `product.supplierinfo` + `part.*` in `kAllowed`
  (any authenticated internal user — product-adjacent); `account.analytic.{account,line}` and
  `account.bank.statement{,.line}` in `kRequired` at group **5** (`ACCOUNT_BILLING`). The
  pseudo-model screens (`part.search`, `bank.reconcile`) render via already-allowlisted
  `ir.actions.act_window` and call the real, gated models.

### 1.5 Bank `reconcile()` — sound, one within-role correctness gap (LOW, not security)

`handleReconcile` is fully bound, posts a **balanced** entry (Dr Bank / Cr counterpart, inflow vs
outflow handled), and guards against double-reconcile (`is_reconciled`). Gap: it does **not
re-verify the target move** is an actually-open invoice of the **same partner/company** before
driving its `amount_residual` to `paid` — a billing user could reconcile a line against a wrong or
already-paid move. This is **within the billing role** (that user can already post manual journal
entries), so it is a data-integrity/robustness issue, not a boundary crossing. It becomes a
cross-company hole only under multi-company (same gate as `§1.2`). Recommend: revalidate the move
(`state='posted' AND amount_residual>0 AND partner/company match`) inside `reconcile`.

### 1.6 Error disclosure — `devMode` confirmed OFF by default

`cfg.http.devMode` parses from the `dev_mode` key (`Container.hpp:589`); the key is **absent** from
`config/system.cfg`, and the default is `"false"`. Production therefore runs `devMode=false` — the
`069` validations (`std::runtime_error`) gate to generic text. (Minor, carried from `068` §1.4:
these should be `ValidationError` so the operator sees *why* — usability, not a leak.)

---

## 2. `070` provisioning + test-suite integrity

### 2.1 Fresh-DB provisioning — fixed and infra-only

`070` swapped boot stages so `ensureSchema_()` (DDL) runs **before** `runMigrations_()` (ALTERs),
and moved `ir_sequence`/`ir_cron` creation into `AuthModule` (module #2). Security-neutral: no new
input surface. The read-only `tools/db_preflight.sh` (connectivity / provisioned? / critical
tables / code-vs-DB migration gap) is pure `SELECT` — it never writes. A latent USD-base-currency
bug surfaced and was fixed (company seeds `currency_id=NULL`; migration 901 sets MYR).

### 2.2 Test-suite honesty note

I re-ran the **whole** suite (`run_tests.sh` globs all 43 `verify_*.sh`; all have valid shebangs
and verdict lines — none is silently skipped). Result:

```
38 passed, 6 failed.

All 18 tests for the 063–069 features PASS:
  stock.quant(18) stock.valuation(13) valuation_gl(11) landed_cost(7) lot_serial(11)
  reorder(6) putaway(5) barcode(5) ir_primitives(18) mrp_production(19) mrp_workorder(13)
  mrp_subcontract(9) mrp_mps(9) analytic(6) bank_recon(7) partkeepr(10) supplierinfo(6)
  domain_field_allowlist(9)

The 6 failures are ALL pre-062 tests — none caused by 069/070:
  • money_display / money_recompute / money_roundtrip / tax_engine — NON-HERMETIC. Their logs
    show empty fields / result:[] / "Invalid JSON" from an empty id interpolation: they read
    hand-seeded baseline records (product id≈3, a baseline sale_order/move) that the now-
    reprovisioned dev DB no longer holds. Exactly the symptom 070 §7 predicted.
  • no_double_audit — its audit checks PASS; it fails only a meta-check ("<4 models probed"),
    which is DB-state dependent.
  • security_fixes — its audit checks PASS; it fails an S-40 X-Forwarded-For last-element
    bucketing sub-check. The dedicated S-40 test (verify_s40_buckets) PASSES, so core proxy-IP
    bucketing works; this stricter case wants a look before relying on XFF behind Cloudflare
    (§4). Test-harness nit: the script prints "*** ONE OR MORE CHECKS FAILED ***", which the
    runner greps past (it looks for "FAILURES"), so it is mislabelled "no verdict".
```

So the caveat from `070` §7 is now **observed on the dev DB itself**, not just predicted: because
the database was reprovisioned during the `070` work, it no longer carries the hand-seeded baseline
(`product id≈3`, a baseline `sale_order`/`account_move`), so the four non-hermetic money/tax
scripts fail on it too. Making those scripts create their own fixtures — and giving
`verify_security_fixes` the runner's expected verdict string — is the clean follow-up (test-quality,
separate from the deploy). **No feature test and no `069`/`070` code is implicated.**

---

## 3. Feature coverage — updated since `068`

`069` closed the four remaining feature gaps `068` §2.2 had flagged: `product.supplierinfo` (+
reorder integration), PartKeepr **PK2–PK4** (+ parametric search), **analytic accounting**, and
**bank reconciliation** — each with a UI, now **browser-verified** this session.

Still genuinely absent (unchanged from `068`):

| Missing | Consequence | Note |
|---|---|---|
| `account.fiscal.position` | no per-customer/region tax mapping | absent |
| `ir.mail_server` (SMTP) | portal reset still hardcodes `"Welcome1"` (S-31); no emailing | deferred; confirmed still present (`PortalModule` `handleResetPassword`) |
| Multi-company | single-company only; see §1.2 first | `057` §3 |
| Deep product-form tabs | vendor/spec/MPN lists live on their own menus, not embedded in the product form; its "On Hand — coming soon" placeholders are stale | `069` §5 |

---

## 4. Deployment readiness — re-scored

| | Item | Status |
|---|---|---|
| ✅ | S-49 (col allowlist) | fixed `068`; new `069` reads clean (§1.1) |
| ✅ | No new routes / ACL deny-by-default / bound SQL | verified across `069` (§1.4) |
| ✅ | Fresh-DB provisioning | **fixed `070`** — empty DB boots (81 tables, MYR, no demo) |
| ✅ | `tools/db_preflight.sh` | read-only pre-start gate |
| ✅ | UI renders | 6 screens click-tested in headless Chrome, 0 JS errors |
| ✅ | `secure_cookies=True`, `trusted_proxies` set | cfg |
| ✅ | `devMode` off in prod | `dev_mode` key absent → default false (§1.6) |
| ✅ | Test suite | **38 pass / 6 fail** — all 18 feature tests (063–069) green; the 6 failures are pre-062 non-hermetic / meta-check / S-40 tests, not regressions (§2.2) |
| ☐ | **DB password `odoo` committed in `config/system.cfg`** | **top blocker** — rotate + untrack + add `.example`; README documents it |
| ✅ | Record-rule bypass (§1.2) | **fixed** — `ir.rule` now enforced on the custom reads via `RecordRuleSql.hpp`; proven by `verify_multicompany_hardening.sh` (`072`) |
| ☐ | Backup + one real restore drill | README has the `pg_dump` cron; the restore drill is still the operator's TODO |
| ☐ | systemd unit | template in README, not committed as a file |
| ☐ | Deploy README OS mismatch | README assumes **Ubuntu 24.04**; prod is **Debian 13 + Cloudflare** — recheck the nginx `http2 on;` directive and the Cloudflare→nginx `X-Forwarded-For` chain for S-40 |
| ☐ | Sessions in-memory (PERF-B) | restart logs everyone out; blocks a 2nd replica |
| ☐ | SMTP / S-31 `"Welcome1"` | deferred (needs `ir.mail_server`) |
| ✅ | `reconcile` target-move revalidation (§1.5) | **fixed** — posted/open/same-company/partner guard; proven by `verify_multicompany_hardening.sh` (`072`) |
| ☐ | S-40 XFF last-element bucketing (`verify_security_fixes`) | low; the dedicated `verify_s40_buckets` passes — confirm rightmost-hop bucketing before trusting XFF behind Cloudflare |
| ☐ | CSV formula-injection prefix | low |

**Cut line for single-company production:** rotate + untrack the DB password, run `db_preflight.sh`,
install the systemd unit from the README, prove a restore. Security posture is good — the one
HIGH-class item (S-49) stays closed, the new code adds no holes, and the record-rule bypass is a
no-op until multi-company.

---

## 5. What was verified live, not assumed

- No `toSql` in the new viewmodels; new reads bind `$N` / coerce ints (source grep, §1.1).
- No `RuleEngine` call in any custom-read module → bypass still open, +4 models (grep, §1.2).
- No `t-raw`/`innerHTML` in the OWL components; all calls via `RpcService` (grep, §1.3).
- `registerRoutes()` empty in all four modules; all new models present in the ACL at the stated
  group (source, §1.4).
- `reconcile()`/`suggest_matches()`/analytic post-hook read directly — bound, balanced,
  idempotent (`NOT EXISTS`) (§1.5, §2).
- `config/system.cfg`: `db_password=odoo`, `secure_cookies=True`, `dev_mode` absent; the file is
  **tracked** in git (`git ls-files`), no `.example` (§4).
- Full suite re-run from a clean launch (§2.2) — the earlier "17 skipped" was a **stale log**
  (`/tmp/audit_suite.log` dated 2026-08-06, pre-dating the `063`+ scripts), not a runner bug; the
  glob picks up all 43.
