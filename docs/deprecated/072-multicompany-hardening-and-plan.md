# 072 — Multi-company hardening (record-rule + reconcile) + the multi-company plan

**Date:** 2026-08-10
**Status:** §1 fixes ✅ implemented, built, and **proven** (`verify_multicompany_hardening.sh`).
§2–§4 are a **proposal** — the architecture choice is the user's; see §4.
**Context:** closes the two `071` items that gate multi-company — the record-rule bypass
(`071 §1.2`) and the reconcile target-move gap (`071 §1.5`) — and then lays out how to go
multi-company.

---

## 1. The two fixes (done)

### 1.1 Record rules now enforced on custom raw-SQL reads (`071 §1.2`)

**Problem.** BaseModel threads `ir.rule` filtering into every CRUD path, but custom viewmodel
`search_read`s build raw SQL by hand and never called the `RuleEngine` — so record rules were
silently unenforced on them. Under multi-company that is a cross-tenant data-isolation hole.

**Fix.** New reusable helper **`modules/base/RecordRuleSql.hpp`** —
`appendRecordRuleSubquery(sql, params, model, op, ctx, table, idExpr, paramCount)`. It emits the
applicable rule as an **id-membership subquery**:

```sql
AND <idExpr> IN (SELECT id FROM <table> WHERE <ir.rule domain>)
```

The subquery form is deliberate: the custom reads alias and JOIN other tables (the picking read
LEFT JOINs `res_partner`, which *also* has `company_id`), so splicing a bare `company_id = $N`
into the outer WHERE would be **ambiguous and fail**. The subquery's FROM is the single base
table, so its columns resolve unambiguously regardless of the outer query. It is a **no-op** when
the RuleEngine isn't ready, the user is admin, or the model has no active rule.

Applied to the custom reads that carry a company/user dimension:

| Model | Custom read patched |
|---|---|
| stock.picking | ✅ `sp.id IN (SELECT id FROM stock_picking …)` |
| stock.move | ✅ `sm.id IN (…)` |
| product.supplierinfo | ✅ `s.id IN (…)` |
| account.analytic.line | ✅ `l.id IN (…)` |
| account.bank.statement.line | ✅ `l.id IN (…)` |

**Scoping note (honest).** `part.parameter` / `part.footprint` are shared **catalogue** data with
no company/user dimension (a resistor's resistance is not company-scoped), so no rule applies and
they were left as-is — the same one-liner extends to them trivially if that ever changes. The
other stock/mrp custom reads (`stock.quant`, valuation layers, orderpoints, `mrp.*`) were **not**
threaded: under the recommended **database-per-company** model (§2) they cannot leak across
tenants — the DB boundary is the isolation, not `company_id`. **If the shared-DB model (Option A)
is chosen instead, every one of those reads must get the same one-liner** — that is the single
biggest reason the choice in §4 matters.

### 1.2 `reconcile` revalidates its target move (`071 §1.5`)

`account.bank.statement.line.reconcile` blindly drove *any* passed `move_id` to `paid` and posted
a bank entry against it. It now revalidates the move first (and throws a pass-through
`ValidationError` the operator can see):

- must be **posted** (not draft),
- must be a **customer/vendor invoice** (`out_invoice`/`in_invoice`/`out_refund`/`in_refund`),
- must be **still open** (`amount_residual > 0`),
- must be the **same company** (when the move names one),
- must be the **same partner** (when both the line and the move name one).

Within-role hardening today; a real cross-company guard once multi-company is on.

### 1.3 Proof — `scripts/verify_multicompany_hardening.sh`

A new integration test proves both, and is self-cleaning (creates a 2nd company + a non-admin
user, activates the dormant "Own Company" rule, restarts for a cold rule cache, then restores
state on exit):

```
§1.2  non-admin (company 1) sees RR-OWN but NOT the company-2 picking RR-FOREIGN
      (control: RR-FOREIGN provably exists in the DB — the filtering is real)
§1.5  reconcile refuses an already-paid invoice; refuses a non-posted invoice;
      the statement line is left unreconciled after both refusals
→ All checks passed.
```

No regression: the rest of the suite is unchanged (same pre-existing non-hermetic failures as
`071 §2.2`, nothing new).

---

## 2. Multi-company — the decision

There are two established shapes. They are **mutually exclusive** and drive very different work.

### Option A — Shared database + `company_id` (the reference ERP's native model)

One database; every company-scoped table has `company_id`; isolation is by `ir.rule`
(`company_id in allowed_company_ids`) + a per-session **active company** + an
`allowed_company_ids` list; users can belong to several companies and switch between them.

- **Pros:** one DB to back up/migrate; native cross-company **consolidated reporting**; shared
  master data (products/partners) with per-company overrides; true the reference ERP semantics.
- **Cons (specific to this codebase):** isolation depends on **every** query being correctly
  company-filtered. This code has ~10+ hand-written SQL reads; §1.1 fixed 5, but Option A means
  **all** of them (quant, valuation, orderpoints, mrp, …) — and every future one — must be
  perfectly company-scoped forever. One miss = a cross-tenant leak. Sessions today carry a single
  `companyId`, not `allowed_company_ids`; a company switcher, per-table `company_id` defaulting,
  and rules on every model would all be new surface.

### Option B — Database-per-company (multi-tenant) — **recommended, and your instinct**

Each company gets its **own** PostgreSQL database, each provisioned by the same
`ensureSchema_` + migrations. The tenant is chosen per request (hostname / the `db` auth param /
a registry). Isolation is **physical** — there is literally no other tenant's data in the DB.

- **Pros (specific to this codebase):**
  1. **It neutralizes the codebase's biggest structural risk.** The custom raw-SQL reads simply
     cannot leak across tenants — different database, different connection. §1.1's fix becomes
     defense-in-depth instead of the wall between tenants.
  2. **The groundwork already exists.** `db` is *already* the auth selector (`login` sends
     `db: 'odoo'`); `070` made a genuinely fresh DB provision cleanly (81 tables, MYR, no demo)
     and shipped `db_preflight.sh`; `ensureSchema_`/migrations are idempotent per DB.
  3. **Operational independence** fits a locker-rental SaaS: per-operator backup, restore, export,
     delete, and blast-radius isolation.
- **Cons:** no out-of-the-box cross-company consolidation (rarely needed for independent
  operators); master data is per-tenant (each seeds its own CoA/products); migrations must run
  across **all** tenant DBs on deploy; needs a connection **router** + a small tenant **registry**.

**Recommendation: Option B.** It matches your expectation, it removes (rather than multiplies) the
audit's central risk, and it builds on the fresh-provision + preflight work already landed in
`070`.

---

## 3. Option B — implementation plan

1. **Tenant registry.** A small source of truth for tenants: `{key, db_name, host, active}`.
   Either a tiny "control" database or a `config/tenants.json`. (Start with config; grow to a
   control DB if a self-serve signup is wanted.)
2. **Tenant resolver.** Per request, derive the tenant from the **Host header** (subdomain, e.g.
   `acme.easylockerspace.com`) and/or the `db` field already in the auth call. One canonical
   resolver, deny-by-default on unknown tenants.
3. **Connection router.** Replace the single global pool with a **pool-per-tenant** cache
   (`map<tenant, DbConnection>`), created lazily from the registry. Every `db_->acquire()` must
   resolve through the request's tenant. (This is the core change — the DB layer is single-DB
   today.)
4. **Provisioning flow.** "Create tenant" = create the database + run `ensureSchema_` + migrations
   + seed reference data — reusing `070`'s path. Expose as an admin action and/or a CLI
   (`tools/provision_tenant.sh`).
5. **Migration runner across tenants.** On deploy, iterate the registry and apply pending
   migrations to each tenant DB, with `db_preflight.sh` per tenant. Fail the deploy if any tenant
   is behind.
6. **Session/auth per tenant.** Key sessions by `(tenant, sid)`; authenticate against the
   tenant's `res_users`. (Sessions are in-memory today — PERF-B — so this is a keying change, and
   is the natural moment to consider a shared session store if a 2nd replica is wanted.)
7. **Ingress.** nginx/Cloudflare: wildcard subdomain → same backend; backend derives the tenant
   from `Host`. Ties into the `071 §4` note that prod is **Debian 13 + Cloudflare** (recheck the
   `X-Forwarded-For` / `CF-Connecting-IP` chain).

**Retained as defense-in-depth:** the §1.1 record-rule fix stays — it costs nothing and protects
against a future in-DB multi-company sub-scoping (e.g. multiple branches within one tenant DB).

---

## 4. Decisions (from the user, 2026-08-10)

1. **Architecture:** database-per-company (Option B) — **but keep the reference ERP's interface** (a top-bar
   company switcher, familiar list/form UX). "Any complications?" → answered in §5.
2. **Tenant routing: maximum flexibility.** All of: subdomain (`companyA.com` → company A),
   **email-domain** (`@companyA.com` → company A), and **login-based** (the same person on
   `@gmail.com` uses a *different login name* to enter a different company). One rule-driven
   resolver must support every case.
3. **Master data:** per-tenant and independent, **with an opt-in option to share a product**
   across tenants.
4. **Provisioning:** CLI/script first.

## 5. Refined design — a control plane + per-tenant data planes

To keep an reference-ERP-like interface **and** deliver the routing flexibility in (2), the missing piece is
a small **control plane**: a central directory that answers "who is this person, which companies
can they reach, and which database is each." The per-company ERP databases are the **data plane**.

```
                     ┌──────────────────────────────────────────┐
   request  ───────▶ │  CONTROL PLANE  (one small central DB)    │
   (Host, email,     │  • tenants:  key, db_name, subdomain,     │
    login)           │              email_domains[], active      │
                     │  • identities: person ⇄ (tenant, login)   │
                     │  • routing rules (subdomain / email / …)  │
                     └───────────────┬──────────────────────────┘
                        resolves to  │  selects DB + issues session
                                     ▼
        ┌───────────────┐   ┌───────────────┐   ┌───────────────┐
        │ DATA: companyA │   │ DATA: companyB │   │ DATA: gmailco │   … per-tenant DBs
        │ (full ERP)     │   │ (full ERP)     │   │ (full ERP)     │   (070 provisioning)
        └───────────────┘   └───────────────┘   └───────────────┘
```

**Resolver (satisfies every case in decision 2), in priority order:**
1. **Subdomain** — `Host: companyA.easylockerspace.com` → tenant A (from `tenants.subdomain`).
2. **Email domain** — login `x@companyA.com` → tenant A (from `tenants.email_domains`).
3. **Explicit login / DB** — the `db` field already in the auth call, or a login-page company
   picker when the identity maps to several tenants (the `@gmail.com`, different-login case).
4. **Ambiguous** → after the password check, show a **company chooser** listing the tenants that
   identity can reach.

**Keeping the reference ERP's interface (the answer to "any complications?").** Yes — the app tiles, list/form
views, and a **top-bar company switcher** all work. The switcher lists the person's other tenants
(from the control plane) and switches the **active tenant DB** (re-scoping the connection + issuing
a session in that DB), exactly like the reference ERP's company menu. Two honest complications:

- **(A) No *combined* multi-company view.** the reference ERP (shared DB) can show companies A **and** B merged
  in one list. With separate databases each screen shows **one** tenant at a time; a merged view
  needs a cross-DB **aggregation layer** (a reporting concern). Recommendation: ship the
  *switch-active-company* UX first (covers ~all day-to-day needs for independent operators); add
  consolidated reporting later only if needed.
- **(B) Cross-tenant single sign-on.** For the switcher to jump A→B without re-typing a password,
  the control plane must own the identity link and be trusted to open a data-plane session in the
  target tenant. That is precisely what the control plane is for — but it means the control plane
  is security-critical (it can mint sessions), so it gets the same deny-by-default, audited
  treatment as the rest.

**Optional product sharing (decision 3).** Independent per-tenant catalogues by default; a shared
product lives once in the control plane (or a dedicated shared catalogue DB) and is *referenced*
(read-through) by tenants that opt in. Deferred — built only when wanted; the data-plane stays
fully independent until then.

## 6. Phasing

- **Phase 1 — Foundation (no UX change):** tenant registry (control-plane schema) + resolver +
  **connection router** (pool-per-tenant, replacing the single global pool) + `provision_tenant.sh`
  (create DB → `ensureSchema_` → migrations → seed, reusing `070`) + a **cross-tenant migration
  runner** for deploys. Outcome: two real tenant DBs, each reachable by subdomain and by the `db`
  login field. This is the load-bearing change and is independent of the identity work.
- **Phase 2 — Identity & the reference ERP switcher:** control-plane identities (person ⇄ per-tenant login),
  email-domain routing, the login-page company chooser, and the **top-bar company switcher** with
  cross-tenant SSO. This is where decision-2's flexibility lives.
- **Phase 3 — Optional extras:** shared-product opt-in; consolidated cross-tenant reporting (only
  if the combined-view need in (A) materialises).

## 7. Implementation status

### Phase 1 — Foundation: ✅ code complete, single-tenant regression green

- **Multi-tenant connection router** — `core/infrastructure/DbConnection.hpp` is now a per-tenant
  pool router keyed by database name, selected by a thread-local set per request. Every existing
  `db_->acquire()` routes to the request's tenant with **no change at the call site**. Unknown/empty
  tenant → the default database (so background work and single-tenant deploys are unaffected).
- **Tenant registry** — `config/tenants.json` (sibling of `system.cfg`); each entry inherits the
  primary db's host/port/user/password unless it overrides them. Parsed in `AppConfig::fromFile`.
- **Resolver** — in `JsonRpcDispatcher`: `authenticate` → the `db` arg, else Host subdomain, else
  default; every other call → the authenticated session's `db`. A `TenantScope` RAII sets/clears the
  thread-local around each request; the session is bound to the **resolved** tenant.
- **Cross-tenant migration runner** — `Container::boot()` loops every registered tenant applying
  `ensureSchema_` + migrations (guarded singletons make re-entry safe). `main --provision` runs that
  and exits without serving.
- **Provisioning CLI** — `tools/provision_tenant.sh <db> [subdomain] [emails]` (createdb → register
  in `tenants.json` → provision) and `tools/migrate_all_tenants.sh` (deploy migrations).
- **Test** — `scripts/verify_multitenant.sh` proves routing + **isolation** across two real
  databases (a record in tenant B is invisible in tenant A and physically only in B; `db`-param and
  Host-subdomain routing both work). It is **env-gated**: it SKIPs cleanly when a tenant DB can't be
  created.
- **Regression:** the full suite is **39 passed / 6 failed** — identical to the pre-refactor
  baseline (the same 6 pre-existing non-hermetic failures, nothing new). The single-tenant path is
  unaffected.

### ⚠ Blocker for real multi-tenant testing: the DB role needs `CREATEDB`

Provisioning a tenant database requires a role that can create databases. In this environment the
`odoo` role has neither `CREATEDB` nor superuser, and there is no superuser access — so the
isolation test (and Phases 2–3, which all need ≥2 tenant DBs) cannot run end-to-end until granted:

```sql
-- run once, as a PostgreSQL superuser:
ALTER ROLE odoo CREATEDB;
```

This is also the privilege the operator needs to run `provision_tenant.sh` in production.

**Test workaround used here:** `tools/test_multitenant_dbpercompany.sh` and
`tools/test_multitenant_switcher.sh` spin up their **own throwaway PostgreSQL** (via `initdb`/
`pg_ctl`, where we are superuser) on ports 5433/5434, create real tenant databases, run the full
end-to-end tests, and tear everything down — so Phases 1–3 are proven against genuinely separate
databases without touching the system PG or needing the grant.

### Phase 2 — Identity, routing, and the company switcher: ✅ code complete + tested

- **Email-domain routing** — the login resolver now tries, in order: explicit `db` → Host subdomain
  → **login email-domain** (`@companyA.com` → companyA, from `tenants.json`).
- **Control plane** — `core/ControlPlane.{hpp,cpp}`: a singleton with its own pool to a small
  control database (`control_db` in `system.cfg`), holding `mc_membership(identity, tenant_db,
  local_login)`. Not a company tenant; never gets the ERP schema.
- **Identity on the session** — at login the dispatcher reverse-looks-up the control plane
  (`identityFor(tenant, login)`) and records the global `identity` on the session.
- **Company chooser / switcher endpoints** — `POST /web/session/companies` (the companies this
  identity can reach) and `POST /web/session/switch_company` (**cross-tenant SSO**: verifies
  membership, then mints a session in the target tenant with no password — the control plane
  vouches). Switching to a non-member company is refused.
- **Frontend** — `UserMenu.js` gains a top-bar company-switcher dropdown (hidden unless the identity
  has ≥2 companies), backed by `RpcService.listCompanies()/switchCompany()`; `partkeepr.css` styles
  it. On a single-company server it stays hidden — the bar is unchanged (verified: home renders with
  **0 JS errors**).
- **Tested** (`tools/test_multitenant_switcher.sh`, all green): identity recorded at login,
  `/companies` lists both companies, SSO switch A→B with no password writes into B (not A), and a
  non-member switch is refused.

### Phase 3 — Shared products + consolidated reporting: ✅ code complete + tested

- **Shared-product opt-in** — the control plane holds a shared catalogue
  (`mc_shared_product(code, name, list_price)`); `POST /web/session/import_shared_products` pulls it
  into the **current** tenant's `product_product`, deduped by `default_code` (idempotent). Tenants
  stay independent — sharing is a one-time import, not a live link.
- **Consolidated reporting** — `POST /web/session/consolidated` loops the identity's companies (each
  queried in its own database) and returns per-company figures (partners, invoiced).
- **Tested** (same harness, all green): both companies import their own independent copy, re-import
  is a no-op, and the consolidated report carries per-company figures for both.

### Summary of what shipped

| Piece | File(s) | Tested by |
|---|---|---|
| Multi-tenant connection router | `core/infrastructure/DbConnection.hpp` | both harnesses + regression |
| Tenant registry + resolver + SSO endpoints | `core/infrastructure/JsonRpcDispatcher.hpp`, `core/Container.hpp` | both harnesses |
| Control plane (identity + shared catalogue) | `core/ControlPlane.{hpp,cpp}` | switcher harness |
| `--config` / `--provision` CLI | `main.cpp` | harnesses |
| Provisioning + migration runner | `tools/provision_tenant.sh`, `tools/migrate_all_tenants.sh` | — |
| Frontend switcher | `web/static/src/components/UserMenu.js`, `services/rpc.js`, `components/partkeepr.css` | puppeteer (0 JS errors) |
| End-to-end DB-per-company tests | `tools/test_multitenant_dbpercompany.sh`, `tools/test_multitenant_switcher.sh`, `scripts/verify_multitenant.sh` | self-run |

All on branch **`feature/multi-company`**. Single-tenant regression stays at the **39/6 baseline**
(same pre-existing non-hermetic failures; nothing new).

## 8. Completing the four caveats (all done + tested)

The four items flagged in §7 are now closed and tested against real throwaway databases:

1. **Per-tenant crons.** `IrCron::tick_()` now iterates **every tenant database** (`tickTenant_`),
   pinning the router to each and running that tenant's due jobs; the overlap guard is keyed
   per-`(tenant, code)` so company A's slow billing can't block company B's. Proven by
   `tools/test_multitenant_ops.sh`: a cron in the **non-default** tenant fires (its `last_run`
   advances and `next_run` is rescheduled) — which the old single-tenant tick could never do.
2. **Provisioning CLI.** `tools/provision_tenant.sh` now honours `ERP_CONFIG` (and passes
   `--config` through), so it can target any config. Proven end-to-end by the same harness:
   `createdb` → register in `tenants.json` → provision (fresh tenant comes up with `res_users`
   seeded). Production still needs a `CREATEDB`-capable role (documented above).
3. **Browser click-test of the switcher.** `tools/test_multitenant_browser.sh` stands up a real
   two-company server and drives headless Chrome (`tools/mt_ui.js`): the **login chooser** lists
   both companies, sign-in lands in Company A, the **top-bar switcher** shows the current company,
   and clicking Company B performs the **cross-tenant SSO** reload into B — all with **0 JS
   errors**. (Fix found here: the chooser trigger moved from `t-on-blur`, which OWL's delegation
   doesn't catch, to a debounced `t-on-input`.)
4. **Pre-login chooser + control-plane admin UI.**
   - `POST /web/session/lookup_companies {login}` powers the login-page company dropdown
     (`LoginPage.js`). Tested (API + browser); returns nothing for an unknown email.
   - `POST /web/control/admin` (admin-gated) manages identity **memberships** and the **shared
     catalogue**; `CompanyAdmin.js` is a Settings screen ("Companies & Access", menu 131) over it.
     Tested (`test_multitenant_switcher.sh`): admin can list/add/remove memberships and shared
     products; unauthenticated is refused.

New test harnesses (all self-contained, spin up their own PostgreSQL, tear down cleanly):
`tools/test_multitenant_ops.sh` (crons + provisioning) and `tools/test_multitenant_browser.sh`
(+ `tools/mt_ui.js`), alongside the existing `test_multitenant_dbpercompany.sh` and
`test_multitenant_switcher.sh` (now also covering the lookup + admin endpoints).
