# 062 — Pre-deployment audit: security, feature gaps, readiness

> ⚠️ **Superseded as the readiness view by `068` (2026-08-08)**, which re-audits the codebase
> after the `063`–`067` inventory/manufacturing/costing build. Read `068` for the current
> security findings and readiness scoring. The feature-coverage tables below are kept current
> via inline `064`–`067` notes.

**Date:** 2026-08-06
**Updated:** 2026-08-06 — the operational deploy blockers from `037` §4–§5 are folded into §4
below and re-scored against the current tree, after `063` (ir.sequence INV numbering,
ir.attachment, ir.model.data) landed. `037` is otherwise superseded; this doc is the live one.
**Scope:** the codebase as it stands — security holes, feature parity against
`zzref2/odoo14` and `zzref3/PartKeepr`, and whether it is safe to deploy.
**Method:** static sweep + live probing. Findings are proved, not asserted.

---

## 0. Headline

- **One real, serious SQL-class hole was found and fixed** (S-49 — the domain filter let
  any authenticated user extract password hashes). It is closed, tested, and the test is in
  the suite.
- **No other injection holes found.** No `std::system`, argv-only process spawning, bound
  parameters throughout, output escaped, routes authenticated.
- **Feature parity with "all of the reference ERP" is not real and was never going to be** — the reference ERP is
  361 addons. This is a 15-module core-ERP subset. Within that subset the core flows work;
  the advanced accounting/inventory features do not exist. This is stated honestly below
  rather than papered over.
- **The PartKeepr parametric-parts catalogue (PK2–PK4) is not implemented.** That is the one
  genuinely missing *product* capability, and it is the differentiator for an electronics
  business.
- **The rental module is complete and integrated.**
- **The framework primitives `037` flagged as the highest-leverage gap are now in.** `037` §2.1
  listed five missing `ir`/`base` primitives; four are built and tested —
  `ir.sequence` (P4, + INV invoice numbering `063`), `ir.cron` (P5), `ir.attachment`
  and `ir.model.data` (`063`). Only `ir.mail_server` (SMTP) remains, deferred by choice.

**Verdict: safe to deploy for the flows it implements, now that S-49 is fixed.** Not a
drop-in the reference ERP replacement, and not a parts catalogue yet. Deploy decision depends on whether
your workflows live inside the implemented subset — §4.

---

## 1. Security sweep

### S-49 (HIGH, FIXED) — domain filter allowed reading any column

The one that matters. `Domain::sanitizeColumn_` charset-checked a filter field name but never
checked it against the model's field registry. The SELECT list *was* restricted
(`buildSelectCols_`), so `password` never appeared in output — but the **WHERE clause was
not**, so an authenticated user could filter on it and extract it blind:

```
password like 'pbkdf2'  -> 1 row     (substring present in the admin hash)
password like 'ZZZZZ'   -> 0 rows    (substring absent)
```

The difference is the leak: the hash comes out one substring at a time. Any unexposed
column on any model was readable this way.

**Fix.** `toSql()` now takes the model's stored-column allowlist; a leaf naming anything
outside it is rejected before SQL. Threaded through `And`/`Or`/`Not` so a leaf buried in an
`OR` is checked too. Record-rule domains keep the null-allowlist path because they are
trusted server config, not user input.

**Proof it is closed** (`verify_domain_field_allowlist.sh`, 6 sections):

```
password = x / like pbkdf2 / like ZZZZZ   -> all ERR (rejected pre-SQL)
present vs absent substring               -> indistinguishable (channel closed)
bad leaf inside OR                        -> rejected
res.partner (different model)             -> rejected
registered fields                         -> still filter correctly
```

> Why it existed: `ORDER BY` was *already* registry-validated (`validateOrder_` checks
> `fieldRegistry_.has`). The domain path simply never got the same treatment. The fix brings
> them level.

### Everything else swept — no holes

| Surface | State |
|---|---|
| **SQL injection** | Bound `pqxx::params` everywhere. The only string-built SQL is id-lists from `ids()` (which throws on non-integers via `get<vector<int>>`), a fixed expense-name allowlist, and dynamic `WHERE`/`ORDER BY` that are `$N`-bound or registry-validated. S-49 was the sole real gap. |
| **Shell / command injection** | No `std::system` anywhere. `ProcessRunner` uses `fork` + `execvp` with an argv array (SEC-31) — no shell, so no argument can be reparsed as syntax. wkhtmltopdf paper-format is allowlisted (SEC-29); `--enable-local-file-access` was removed (S-44). |
| **HTTP header injection** | `Content-Disposition` filenames come from validated model names and parsed integers, charset-filtered (S-39). |
| **Path traversal** | Export filenames derive from `has()`-validated model names with dots stripped; PDF temp files use `SecureTempDir` (`mkdtemp` 0700, RAII). |
| **XSS** | OWL auto-escapes via `t-esc`. The portal uses `escHtml`/`esc` on every server value. The one `document.write` is an admin editing their own report template (self-XSS at worst). The portal is partner-scoped, so even a missed escape is self-inflicted. |
| **Route auth** | All `/rental/*` routes now authenticate (docs/061); all `/portal/*` are cookie-scoped and partner-filtered; `/report/*` uses `checkAuth`. |
| **Error disclosure** | `ex.what()` gated behind `devMode` (SEC-28); `AccessDeniedError`/`ValidationError` pass through by design. |
| **Client IP / rate limiting** | Proxy-aware `clientIp()` reads the last XFF element (S-40/SEC-32). |
| **Sessions** | TTL expiry, store cap, fixation-resistant rotation (S-42/S-43). |

### One minor item, not a blocker

**CSV export has no formula-injection guard.** A cell beginning `=`, `+`, `-` or `@` is
executed as a formula if the CSV is opened in Excel. Severity is low (it needs a user to
open the file in a spreadsheet and click through a warning), and stock the reference ERP does not guard it
either. Worth a one-line prefix-with-apostrophe fix eventually; not a deploy blocker.

---

## 2. Feature gap vs the reference ERP

**Framing.** `zzref2/odoo14` is the full Community edition — **361 addons** (CRM, website,
events, livechat, POS, elearning, …). "Have all of the reference ERP" cannot mean porting 361 apps.
The honest measure is depth *within the modules we chose to implement*.

### What we implement, and how deep

| Module | Lines | Covers | Does NOT cover (vs the reference ERP) |
|---|---:|---|---|
| account | 1876 | invoices, payments, partial reconcile (P1), tax engine (P3), journals, multi-currency + FX (P2) | **analytic accounting, bank statements & reconciliation, fiscal positions, cash rounding, reconcile models, tax reports, EDI** |
| sale | 1443 | quotation→order→invoice, tax lines | pricelists beyond basic, coupons, subscriptions, margins |
| purchase | 1302 | RFQ→order→bill | requisitions, vendor bill matching rules |
| stock | 1335 | pickings, moves, locations, warehouses, **`stock.quant` on-hand (`064`), valuation — std/avg/FIFO + real-time GL (`065`), lots/serial + landed costs (`066`), reorder rules + putaway + barcode (`067`)** | barcode **scanning UI**, packages, multi-step routes (pick/pack/ship) |
| mrp | 655 | BOMs, **manufacturing orders + work centers + BOM routing operations + work orders + subcontracting + MPS (`064`)** | work-center capacity calendar / finite scheduling, per-subcontractor locations |
| product | 995 | products, categories (PK1, cycle-safe) | **`product.supplierinfo`, variants/attributes, parametric parts** |
| hr | 676 | employees | payroll, leave, expenses, appraisal |

### The honest read

The **core transactional spine works and is safe**: quote → sale → invoice → payment →
reconcile, with correct tax and multi-currency, gapless numbering (`ir.sequence`, P4, with
`INV000001` customer-invoice series, `063`), scheduled jobs (`ir.cron`, P5), content-addressed
file storage (`ir.attachment`) and stable external ids (`ir.model.data`) (`063`), auditing,
record rules, and optimistic concurrency. The earlier audits (`037`, `038`, `039`) flagged the
primitives that were missing — tax engine, payment allocation, sequences, cron, attachments,
audit coverage — and **P1–P7 plus `063` closed all of them except SMTP.**

What is genuinely absent is the **second tier of accounting and inventory**: analytic
accounting, bank reconciliation, fiscal positions on the finance side; lots/serials and
landed costs on the inventory side. None of these is a bug — they were never built. Whether
their absence blocks you is entirely workflow-dependent (§4).

> **Update (`064`/`065`, 2026-08-08):** the two biggest gaps this section named — no on-hand
> quantity anywhere, and BOMs that could not be manufactured — are closed, and inventory is now
> **costed**. `stock.quant` (on-hand, reservation, inventory adjustment), full manufacturing
> (MO / work centers / routing / work orders), subcontracting, MPS (`064`), and inventory
> **valuation** — standard/average/FIFO with real-time GL postings (`065`) — all landed, covered
> by 7 new integration suites (full suite: 35 green).

---

## 3. Feature gap vs PartKeepr

`docs/039` specified the PartKeepr port as PK1–PK7. Current state:

| | Feature | State |
|---|---|---|
| PK1 | Category tree + counts | ✅ Done — cycle guard (C-3) and cascade (C-4) **fixed** since `039`; recursive-CTE ancestor check present |
| **PK2** | Footprints | ❌ Not started |
| **PK3** | Part parameters + SI units | ❌ Not started — *the differentiator* |
| **PK4** | Manufacturer part numbers (MPN) | ❌ Not started |
| PK5 | Enhanced supplier info | ❌ Blocked — `product.supplierinfo` doesn't exist |
| PK6 | Min-stock / part status | ◐ **Unblocked** — `stock.quant` on-hand exists (`064`); min-stock can build on `product.qty_available` + the MPS safety-stock field |
| PK7 | Attachments / datasheets | ◐ **Unblocked** — `ir.attachment` now exists (`063`); a datasheet is an `ir.attachment` with `res_model='product.product'`. The catalogue-side UI (list/upload on the product form) is the only piece left, and it is small |

**The gap that matters: PK2–PK4, the parametric parts catalogue.** For an electronics
inventory this is *the* PartKeepr feature — searching parts by resistance/tolerance/package,
matching MPNs to suppliers. It is three-to-five self-contained tables (no dependency on the
missing ERP primitives) and is the single most valuable thing to build next if the parts
catalogue is a real requirement. `039` §4.3 recommends PK3 first, and that still holds.

---

## 4. Deployment readiness

### Safe to deploy — with the scope understood

**Yes**, now that S-49 is closed, *if* your workflows live inside the implemented subset:
selling/renting, invoicing, taking payment, basic stock movements, BOMs. The transactional
core is tested (28 suites, ~400 checks), audited, and free of the obvious injection classes.

**No**, if you need any of: analytic/cost accounting, bank-statement reconciliation, fiscal
positions, real inventory valuation (`stock.quant`), lots/serial tracking, or a parametric
parts catalogue. Those are absent, not broken — but absent.

### Deploy gate — operational (from `037` §4–5, re-scored against the current tree)

`037` framed the *code* as good and the *shipping* of it as the real gate. That still holds,
but half its gate is now cleared. Its Docker/TLS-in-process track (A3) is **superseded** — the
target is a GCP VM behind nginx + Cloudflare, which terminates TLS and owns all inbound.

| `037` | Item | Status | Note |
|---|---|---|---|
| A2 | `secure_cookies` + `SameSite` | ✅ | cfg sets `secure_cookies = True`; `setSameSite(kLax)` in `JsonRpcDispatcher.hpp:391`. HSTS/TLS are at nginx + Cloudflare, not in-process |
| A4 | Test suite (was the last open P0) | ✅ | 28 suites green (P7); `run_tests.sh` is the gate |
| B1 | `ir.sequence` | ✅ | P4; INV customer-invoice series in `063` |
| B2 | Tax engine | ✅ | P3 |
| B5 | `ir.attachment` | ✅ | `063` — content-addressed filestore |
| B7 | `ir.cron` | ✅ | P5 |
| — | `ir.model.data` | ✅ | `063` |
| A1 | Secrets hygiene | ◐ | `config/system.cfg` is untracked (not in git), but `db_password` is still the default `odoo` and there is **no `system.cfg.example` and no `.gitignore` entry**. Rotate the password; add the example + ignore rule so it can't be committed by accident |
| — | `devMode` off in prod | ◐ | no `dev_mode` key in the cfg → defaults **false** (`Container.hpp:583`); it does not leak `ex.what()`. Confirm the *server's* cfg has no `dev_mode = true` |
| A6 | Persistent sessions (PERF-B) | ☐ | still an in-memory `unordered_map` (`SessionManager.hpp:338`). On one VM this means **a restart logs everyone out**; it also blocks a second replica. Survivable for a single-node launch |
| A7 | Dead ACL entries | ☐ | `product.supplierinfo` and `mrp.production` are still allowlisted in `JsonRpcDispatcher.hpp` with no model behind them (404 today, silent grant the day someone adds the model). Strip or implement — 30 min |
| A5 | Backup + one real restore drill | ☐ | `pg_dump` cron, then actually restore it once, before real data exists |
| B4 | `ir.mail_server` (SMTP) → S-31 | ☐ | portal reset still hardcodes `"Welcome1"` (`PortalModule.cpp:1137`); every reset yields the same known password. Blocked on SMTP, deferred by choice |
| B6 | Password policy / 2FA | ☐ | no min length or complexity on any password; no `auth_totp`. Lower priority |
| — | CSV formula-injection prefix | ☐ | §1 — low priority, one line |
| — | DB password file perms | ☐ | `system.cfg` holds it; check it is not world-readable, and that S-44 shipped |
| — | systemd unit installed | ☐ | `server.sh --install` writes the unit; run it on the server so a crash restarts and the process is managed, not a stray |

The ✅ rows are the substance of `037`'s Track A/B; what remains is one small correctness item
(A7), one single-node caveat (A6), routine ops (A1/A5/perms/systemd), and two deferred features
(B4/B6). None blocks a scoped single-node launch; A1 (rotate the password) and A5 (a restore
drill) are the two worth doing before real data lands.

### Recommended next build, in order

1. **A1 + A5 + A7** — rotate the DB password, prove a restore, strip the two dead ACL entries.
   Hours, not days; clears the last of `037`'s Track A that actually gates.
2. **PK3** parametric parts (if electronics inventory is the point) — ~1.5 w, self-contained;
   PK7 datasheets are now cheap on top of it since `ir.attachment` exists.
3. **`ir.mail_server`** (SMTP) — closes S-31, enables invoice/quote emailing and `auth_signup`.
   This is now the single most useful unbuilt piece (see §4.4 / `037` B4).
4. **`product.supplierinfo`** — per-vendor pricelists / lead times; would let reorder pick the
   vendor + price automatically (today the reorder rule names the vendor) and unblocks PK5.
5. **Multi-company** (`057` §3) — ~1 w, mostly ops, when a second entity is needed.

> The **inventory/manufacturing side is now essentially complete**: on-hand + manufacturing
> depth (MO / work centers / routing / work orders / subcontracting / MPS) (`064`), valuation —
> standard/average/FIFO with real-time GL (`065`), lots/serial + landed costs (`066`), and
> warehouse automation — reorder rules, putaway, barcode (`067`). Quantity, value, traceability,
> costing, and replenishment are all covered. What's left is a barcode **scanning UI** (front-end)
> and non-inventory items (SMTP, supplierinfo, multi-company).

---

## 5. What was verified live, not assumed

- S-49 exploit reproduced (`password like` blind extraction), fix confirmed, regression added.
- `ORDER BY` injection attempt — rejected by `validateOrder_`.
- All `/rental/*` routes — 401 without a session (docs/061).
- 27 integration suites + 120 unit assertions (28 total) — green after the core
  `Domain`/`BaseModel` change S-49 required, and again after `063`.
- INV numbering, attachment upload/download round-trip, traversal-safety, dedup, and
  auth — `verify_ir_primitives.sh` (18 checks), added in `063`.

The full suite is `./scripts/run_tests.sh`. The security tests are
`verify_domain_field_allowlist.sh` and `verify_ir_primitives.sh`.
