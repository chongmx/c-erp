# 037 — Deployment Readiness Plan

> ⚠️ **Superseded by `038-security-feature-audit-pre-deploy.md` (2026-08-02).**
> Deployment target was since fixed as a GCP VM behind nginx (nginx terminates TLS and owns
> all inbound; app and DB bind loopback). Track A's packaging/TLS/Docker items no longer
> apply. §2 (the reference ERP gap analysis) and §3 (PartKeepr status) remain accurate and are carried
> forward into 038 §3. **Read 038 instead.**

**Date:** 2026-08-02
**Baseline:** HEAD `0c798e5` (3 commits past review v10's `39b1377`)
**Inputs:** `review/c-erp-review-v10.docx`, docs `000`–`036`, `zzref2/odoo14`, `zzref3/PartKeepr-master`
**Status:** Planned

---

## 0. Status corrections to review v10

Review v10 was cut at `39b1377`. Three commits have landed since. Both open MEDIUM
findings were fixed in `d087308` ("security patch") — the review's "Recommended Next
Steps" items 1 and 2 are **done**.

| Finding | v10 status | Verified actual status |
|---|---|---|
| S-37 — custom ViewModels don't audit | NEW — OPEN | **FIXED** (`d087308`). 36 `AuditService` call sites across account / sale / stock / purchase / mrp / ir |
| S-38 — CSV import/export bypass record rules | NEW — OPEN | **FIXED** (`d087308`). `getSession` → `UserContext` → `setUserContext()` on both routes (`IrModule.cpp:576`, `:723`) |
| S-31 — portal hardcoded `"Welcome1"` | OPEN (deferred) | Still open — blocked on SMTP (see §3.4) |
| P0-3 — test suite | OPEN | Still open — **zero test files in the repo** |
| PERF-B — in-memory sessions | OPEN | Still open |

**Net: 1 low finding + 1 P0 + 1 PERF item open.** The security posture is genuinely
good. The blockers to deployment are no longer security findings — they are
operational (§4) and functional (§2).

---

## 1. Verified inventory

- 25k LOC C++ / 15 modules / ~35 registered models / 9,227-line single-file OWL frontend
- Build type is `Release` in `build/CMakeCache.txt`; binary present at `build/c-erp`
- Infrastructure complete: DI container, connection pool w/ timeout, JSON-RPC dispatcher,
  session manager, rate limiter, RuleEngine (record rules), AuditService, MigrationRunner,
  CsvParser, TtlCache, WebSocket server

### 1.1 Two allowlisted-but-unimplemented models

`JsonRpcDispatcher.hpp` grants ACL to two models that have **no model, no ViewModel, and
no table**:

| Model | Allowlist site | Reality |
|---|---|---|
| `product.supplierinfo` | `JsonRpcDispatcher.hpp:566` | Not implemented. Phase A3b, marked "🔜 Next" in `plan.md` since 2026-03-24 |
| `mrp.production` | `JsonRpcDispatcher.hpp:607` (group 13) | Not implemented. Only `mrp.bom` + `mrp.bom.line` exist |

Harmless today (`ModelFactory::has()` returns false → 404), but they are dead ACL entries
that will silently grant access the day someone adds the model. Either implement or strip.

---

## 2. Gap analysis vs `zzref2/odoo14`

Scoped to the 15 modules c-erp actually ports. All rows below were verified absent by
source search, not inferred.

### 2.1 Missing framework primitives (`base` / `ir`) — highest leverage

These are single, small pieces of infrastructure that **each unblock several downstream
features**. This is where the next work belongs.

| the reference ERP model | Status in c-erp | What it blocks |
|---|---|---|
| **`ir.sequence`** | Missing. Numbering uses hardcoded raw PG sequences (`sale_order_seq`, `stock_out_seq`) created inline in `ensureSchema_()` | No prefix/padding config, no per-company numbering, no yearly reset, no gapless invoice numbering. **Invoice numbering is a legal requirement in most jurisdictions** |
| **`ir.attachment`** | Missing entirely | Datasheets (PK7), emailing PDFs, document management, any file upload beyond the existing `uploads/` scratch dir |
| **`ir.cron`** | Missing | Reordering rules, recurring invoices, session GC, scheduled reports, any background job |
| **`ir.mail_server`** | Missing (Phase 17f, deprioritised) | Portal password reset (S-31), `auth_signup` completion (Phase 14, documented as blocked on this), invoice/quote emailing, any notification |
| **`ir.model.data`** | Missing. Replaced by the manual ID registry in `docs/026` | Fragile seeding; no safe re-seed on upgrade; ID collisions are caught by a spreadsheet, not the DB |

### 2.2 Missing business logic

| Area | Status | Notes |
|---|---|---|
| **Tax computation** | `account.tax` model + `tax_line_id` field exist; **no engine**. No tax lines are generated on invoices | Invoice totals are untaxed. This is the single largest correctness gap for a real deployment |
| **`stock.quant`** | Missing. **There is no on-hand quantity anywhere in the system** — no `qty_available` on product, no quant table. Stock level is only derivable by ad-hoc `SUM(stock_move.quantity)` | No inventory valuation, no reservation, no availability check, no inventory adjustment, no negative-stock guard |
| `product.template` / `product.attribute` | Missing — `product.product` only | No variants (size/colour). Acceptable for a parts-oriented deployment; a hard blocker for retail |
| `product.pricelist` | Missing | Price is `list_price` only. No customer pricing, no qty breaks, no discounts |
| `product.supplierinfo` | Missing (allowlisted, see §1.1) | No vendor pricelist, no MOQ, no lead time on purchase |
| `account.fiscal.position` | Missing | No tax mapping per customer/region |
| `res.currency.rate` | Missing | `currency_id` is stored but never converted. Multi-currency is nominal only |
| `account.bank.statement` / reconciliation | Missing | Payments cannot be matched to bank lines |
| `account.analytic.account` | Missing | No cost-centre reporting |
| `stock.production.lot` | Missing (Phase 28, deferred) | No lot/serial traceability |
| `stock.rule` / reordering / putaway | Missing (Phase 29, deferred) | No automated replenishment; also needs `ir.cron` |
| `mrp.production` | Missing (allowlisted, see §1.1) | BOMs exist but cannot be manufactured |
| `auth_totp` (2FA) | Missing | P1 in v10 |
| `auth_password_policy` | Missing | No min length or complexity on any password |

### 2.3 Deliberately out of scope — leave deferred

CRM, project, calendar, fleet, payroll, delivery carriers, website/eCommerce, POS,
`base_automation`. All correctly deprioritised in `plan.md`; none belong before a first
deployment.

---

## 3. `zzref3/PartKeepr` status

The two plans (`029`, and the 55 KB detailed `030`) are largely **unimplemented**.
Verified by source search across `modules/`:

| Phase | Plan | Actual |
|---|---|---|
| **PK1** Category tree UI | Frontend-only | ✅ **Implemented** — `ProductCategoryTree` component, `app.js:8427` (collapsible left-panel tree with `child_count` / `product_count`) |
| PK2 Footprints | 2 tables | ❌ No `part_footprint*` anywhere |
| PK3 Part parameters + SI units | 3 tables | ❌ No `part_parameter` / `part_unit` / `part_si_prefix` |
| PK4 Manufacturer part numbers | 1 table | ❌ No `part_manufacturer_info` |
| PK5 Enhanced supplier info | extends A3b | ❌ Base `product.supplierinfo` itself doesn't exist |
| PK6 Min stock + part status | 2 columns | ❌ No `min_stock` column |
| PK7 Attachments / datasheets | `ir_attachment` | ❌ Not implemented — and this is the same `ir.attachment` gap as §2.1 |

So: **PK1 landed; PK2–PK7 did not.** What made the earlier gap analysis read as "mostly
covered" is that the ✅ rows in `docs/029` were pre-existing c-erp features
(`product_category.parent_id`, hierarchical `stock_location`, `mrp_bom`, `uom_uom`) —
not PartKeepr work.

**Recommendation: do not start PK2–PK7 before deploying.** They are a domain
specialisation (electronics parts) layered on top of an ERP core that still lacks tax
computation and on-hand stock. PK5/PK6/PK7 in particular are cheap *after* `product.supplierinfo`
and `ir.attachment` exist, and near-worthless before. Sequence them post-deployment.

---

## 4. Deployment blockers — not covered by any review

Reviews 1–10 audited the *code*. Nothing has audited *shipping it*. These are the real
gate on a first deploy.

### 4.1 Secrets and config

- `config/system.cfg` is **tracked in git** with `db_password = odoo` in plaintext.
  `.gitignore` excludes `zzref*/`, `log/`, `review/` — but not `config/`.
- Fix: commit `config/system.cfg.example`, add `config/system.cfg` to `.gitignore`,
  `git rm --cached config/system.cfg`, and rotate the DB password. `AppConfig::fromFileOrEnv`
  already falls back to env vars, so a container deploy can skip the file entirely.

### 4.2 Transport security

- `secure_cookies` defaults to **`false`** (`Container.hpp:466`) and is **absent from
  `config/system.cfg`** — so the session cookie ships without the `Secure` flag today.
  `HttpOnly` is set (`JsonRpcDispatcher.hpp:346`); `SameSite` is not set anywhere.
- No TLS in-process, no HSTS header. `X-Frame-Options: DENY` and
  `Content-Security-Policy: default-src 'none'` are present (`HttpServer.hpp:352`, `:357`).
- Fix: terminate TLS at nginx/Caddy in front of Drogon; set `secure_cookies = true` in the
  production cfg; add `SameSite=Lax` to the session cookie; add HSTS at the proxy.

### 4.3 Process supervision and packaging

- No Dockerfile, no systemd unit, no reverse-proxy config, no CI pipeline (`.github/` absent).
- `logfile = log/system.log` but `log/` is gitignored — the directory must exist at
  startup or logging silently degrades.
- Fix: a `Dockerfile` (multi-stage: build → slim runtime with `libpq5` + wkhtmltopdf) plus
  a `docker-compose.yml` with Postgres is the shortest path and sidesteps every WSL
  packaging problem below.

### 4.4 Operational gaps

| Gap | Impact |
|---|---|
| Sessions in-memory (PERF-B) | Every restart logs out every user. Blocks running >1 replica. Postgres-backed store, ~3–5 days |
| No backup/restore procedure | Unrecoverable data loss. `pg_dump` cron + a documented restore drill |
| No `/healthz` wiring to a supervisor | Endpoint exists; nothing consumes it |
| Portal reset password `"Welcome1"` (S-31) | Every portal reset yields the same known password. Blocked on `ir.mail_server` (§2.1) |
| No password policy | Users can set `a` as a password |

### 4.5 WSL-specific notes

You develop in WSL Ubuntu (repo is on the native ext4 fs at `/home/user/code/c-erp` — good,
don't move it to `/mnt/c`, the I/O penalty is severe for CMake).

- **WSL is not a deployment target.** No systemd by default (needs `systemd=true` in
  `/etc/wsl.conf`), no boot-time start, the VM shuts down when the last shell exits, and
  the IP changes on every restart.
- If you must expose it from WSL for a demo, `netsh interface portproxy` from Windows is
  required — `0.0.0.0:8069` inside WSL is not reachable from the LAN otherwise.
- Postgres in WSL needs `service postgresql start` manually each session (no systemd).
- Build inside a container matching the deploy distro. A binary linked against WSL
  Ubuntu's glibc/libpq will not necessarily run on the target host.
- `scripts/install_wkhtml.sh` pins `wkhtmltox-0.12.6.1-2.jammy` — the runtime image must be
  Ubuntu 22.04 (jammy) or the PDF reports break at runtime, not build time.

---

## 5. Recommended pipeline

Three tracks. **Track A is the actual gate on deploying** — it is ~2 weeks and contains no
new features. Track B is what makes the deployment *correct*. Track C is everything else.

### Track A — Deploy gate (~2 weeks, do first)

| # | Item | Effort | Why it gates |
|---|---|---|---|
| A1 | Secrets hygiene: `system.cfg.example`, gitignore + `git rm --cached` the real one, rotate DB password | 1 h | Credentials are in git history |
| A2 | `secure_cookies = true` in prod cfg; add `SameSite=Lax`; HSTS at proxy | 2 h | Session hijacking over plain HTTP |
| A3 | Dockerfile (multi-stage, jammy runtime) + docker-compose with Postgres + nginx TLS | 1–2 days | Nothing is deployable today; also fixes every §4.5 WSL issue |
| A4 | **Test suite (P0-3)** — the last open P0. Use the plan already written in `docs/033` §3 (zero-dependency `ERP_TEST` harness, `erp_tests` target). Start with: `Domain::toSql`, `FieldRegistry`, `CsvParser`, `RuleEngine`, OCC conflict, `htmlEscape`, BaseModel CRUD roundtrip | 1 week | No regression net under any of Track B |
| A5 | Backup + documented restore drill (`pg_dump`, actually restore it once) | 4 h | Data loss is unrecoverable |
| A6 | Persistent sessions (PERF-B) — Postgres-backed store | 3–5 days | Restart logs everyone out; blocks >1 replica |
| A7 | Strip or implement the two dead ACL entries (`product.supplierinfo`, `mrp.production`) | 30 min | Dead allowlist entries silently grant access later |

### Track B — Correctness for a real deployment (~4–5 weeks)

Ordered by dependency, not by size. B1 and B2 unblock the most.

| # | Item | Effort | Unblocks |
|---|---|---|---|
| B1 | **`ir.sequence`** — replace hardcoded PG sequences. Prefix/padding/per-company/yearly reset | 3–4 days | Legally-compliant invoice numbering; prerequisite for anything issuing documents |
| B2 | **Tax computation engine** — compute tax lines on `account.move` from `account.tax`; price-included, tax groups, rounding. Extend to sale/purchase order lines | 2 weeks | Invoice totals are currently wrong. Highest-value single item in this table |
| B3 | **`stock.quant` + `qty_available`** — real on-hand quantity, inventory adjustment, availability check on delivery | 1.5–2 weeks | Inventory is presently unreadable; blocks reordering, valuation, PK6 |
| B4 | `ir.mail_server` (SMTP) — Phase 17f | 3–5 days | Unblocks S-31 portal reset, Phase 14 auth_signup, invoice emailing |
| B5 | `ir.attachment` + `/web/content/<id>` — schema is already fully designed in `docs/030` §2.1 | 3–4 days | PK7 datasheets, PDF attachment, document management |
| B6 | Password policy + `auth_totp` (2FA) | 1.5 weeks | v10 P1 items; cheap once done together |
| B7 | `ir.cron` — scheduled job runner | 3–4 days | Reordering rules, session GC, recurring invoices |
| B8 | `product.supplierinfo` (Phase A3b — already "Next" for 4 months) | 3–4 days | Purchase workflow; prerequisite for PK5 |

### Track C — After deployment

In rough order: `mrp.production` → reordering rules (needs B7) → lot/serial (Phase 28) →
`product.pricelist` → PK2–PK7 (needs B5, B8) → multi-currency FX → bank reconciliation →
`account.analytic.account` → `product.template`/variants.

### Suggested cut line for a first deploy

**A1–A7 + B1 + B2.** That gives you a deployable, backed-up, TLS-terminated,
test-covered system that numbers documents legally and computes tax correctly — roughly
5–6 weeks.

Deploying without **B2 (tax)** means every invoice total is wrong.
Deploying without **B3 (stock.quant)** means the inventory module reports nothing —
survivable only if the first deployment is invoicing-only.

---

## 6. Cross-cutting rules for all work above

Unchanged from `docs/033` §Cross-cutting Rules — SEC-28 (gate `ex.what()` behind
`devMode`), SEC-29 (allowlist DB values in shell commands / validate field names against
`FieldRegistry`), S-33 (`PoolExhaustedException` → 503), PERF-E (split `.hpp`/`.cpp`),
PERF-F (1000-row cap).

Add one, now that A4 exists:

| Rule | Requirement |
|---|---|
| **TEST-1** | Every item in Track B ships with at least one test in `tests/` in the same commit. The test suite is worthless if it stops growing the day it's written. |
