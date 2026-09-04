# 040 — Master Plan: Consolidated Roadmap, Multi-Company, Rental Module

**Date:** 2026-08-02
**Baseline:** HEAD `0c798e5`
**Consolidates:** `037` (superseded), `038` (security/feature audit), `039` (category audit + PartKeepr)
**Target:** GCP VM, Ubuntu 24.04, nginx terminates TLS; app + PostgreSQL bind loopback
**Status:** Proposed — for review

---

## 0. How to read this

`038` and `039` remain the detailed findings records. This document is the **single sequenced
roadmap** and the **design** for the two new workstreams: multi-company (database-per-tenant)
and the warehouse storage rental module.

Three things are settled before anything else and are not re-argued here:

- Stage 1 security work (`038` §4) ships before the VM is publicly reachable.
- The test suite is the last open P0 and gates everything after it.
- The rental module has hard dependencies on ERP primitives that do not exist yet (§3.7).
  Building it first would mean building those primitives badly, inside a rental module.

---

## 1. Consolidated roadmap

| Stage | Contents | Effort | Gates |
|---|---|---|---|
| **1** | Security pre-deploy: S-39…S-46, S-47, C-1…C-4 | ~2.5 w | Public reachability |
| **2** | Verify on target VM (wkhtmltopdf/noble, binding, rate limits) | 1 d | Stage 3 |
| **3** | Test suite (last open P0) | 1 w | All later stages |
| **4** | Multi-company (database-per-tenant) | 1 w | Rental (tenanted) |
| **5** | Primitives: `ir.sequence`, `ir.cron`, tax engine, payment allocation, `ir.attachment` | 5–6 w | Rental |
| **6** | Rental module | ~7 w | — |
| **7** | Deferred ERP: `stock.quant`, `mrp.production`, PartKeepr PK2–PK7, etc. | — | — |

**Deployable after Stage 3** (~3.5 weeks) with the ERP as it stands. Stages 4–6 then build
the rental business on a tested base.

### 1.1 Stage 1 detail (absorbing `038` §4 and `039` §5)

> **Implementation status: items 1, 2, 3, 8, 9, 11 are DONE.**
> See `041-security-fixes-implementation.md` for approach, call sites and the
> runtime-verification checklist. Build is clean; runtime tests still pending.

| # | Item | Sev | Effort | Status |
|---|---|---|---|---|
| 1 | **S-39** command injection → RCE: shell removed (`fork`+`execvp`), `mkdtemp` temp dirs | HIGH | 3 h | ✅ Done |
| 2 | **S-40** proxy-aware `ClientIpResolver` at all 3 sites + nginx config | HIGH | 4 h | ✅ Done |
| 3 | **S-47** audit in all 8 unaudited custom ViewModels (19 call sites) | HIGH | 1 d | ✅ Done |
| 4 | Config: untrack `system.cfg`, rotate password, `http_interface=127.0.0.1`, `secure_cookies=true` | HIGH | 1 h | ⚠️ cfg values set; **still tracked in git, password not rotated** |
| 5 | **S-42** rotate session ID on authenticate | MED | 2 h | ✅ Done (`043`) |
| 6 | **S-43** `evictExpired()` timer; stop storing anonymous sessions; bounded store | MED | 3 h | ✅ Done (`043`) |
| — | **S-48** `call_kw` ignored the session cookie *(found in verification)* | MED | — | ✅ Done (`043`) |
| — | **S-49** rate limiter never counted a failed login *(found in verification)* | HIGH | — | ✅ Done (`042`) |
| 7 | **S-41** validate domain field names against FieldRegistry | MED | 4 h | Open |
| 8 | **C-3** category cycle prevention (recursive-CTE ancestry check) | MED | 2 h | ✅ Done |
| 9 | **C-4** guard category unlink on children/products | MED | 2 h | ✅ Done |
| 10 | **C-1** recursive `product_count` | MED | 2 h | Open |
| 11 | **S-44** drop `--enable-local-file-access` | LOW–MED | 1 h | ✅ Done |
| 12 | **S-45** rebuild ORDER BY from validated tokens | LOW | 2 h | Open |
| 13 | Strip dead ACL entries (`product.supplierinfo`, `mrp.production`) | LOW | 30 m | Open |

S-35 is partially closed — proto-based ViewModels now set the user context; four raw-SQL
ViewModels still bypass record rules. See `041` §5; the durable fix is §1.2 below.

### 1.2 Close the structural defect, not the instances

S-35, S-37, S-38 and S-47 are four occurrences of one defect: **features wired into
`GenericViewModel` are silently absent from custom ViewModels.** Record rules, audit, OCC and
the `Domain` compiler have each been retrofitted case by case, and each retrofit missed
ViewModels written before it.

Before Stage 6 adds ~9 new models, fix the pattern (~3 d):

- Move audit + rule-domain merge + OCC into `BaseViewModel` so custom ViewModels inherit them
  by construction, with an explicit opt-out for read-only models.
- Add a boot-time assertion: every registered ViewModel exposing `create`/`write`/`unlink`
  either inherits the audited path or appears on a named allowlist. Fail startup otherwise.

Without this, the rental module becomes occurrence number five.

---

## 2. Multi-company — database-per-tenant

### 2.1 Requirement

Identical code and schema per company; **separate database per company**. A user logs in and
lands in their company; an admin can switch between companies.

### 2.2 What the current code makes hard

Two facts, both verified, decide the design:

```cpp
// core/factories/BaseFactory.hpp:31
using Creator = std::function<std::shared_ptr<TBase>()>;   // ← takes NO arguments
```

Every one of ~40 `registerCreator` calls captures `db` **by value at boot**:

```cpp
viewModels_.registerCreator("sale.order", [db]{ return std::make_shared<SaleOrderViewModel>(db); });
```

```cpp
// core/RuleEngine.hpp:39,93   core/infrastructure/AuditService.hpp:33,62
static void initialize(std::shared_ptr<DbConnection> db);
static std::unique_ptr<RuleEngine> s_instance_;             // ← process-wide, ONE db
```

So resolving a database per request requires changing the `Creator` signature and all ~40
registration sites and all `create()` call sites, converting both singletons into
db-keyed registries, keying the currency/`fields_get` caches by database, and running
`MigrationRunner` + `ensureSchema_()` per database. That is **3–4 weeks touching every
module** — precisely the change you do not want to make with a codebase that has just
acquired its first tests.

### 2.3 Decision: process per database

Run **one `c-erp` process per company**, each with its own `config/<tenant>.cfg` (its own
`db_name` and `http_port`), all on loopback. nginx routes by subdomain.

```
acme.erp.example.com  → 127.0.0.1:8069  (db: erp_acme)
globex.erp.example.com→ 127.0.0.1:8070  (db: erp_globex)
```

**Why this and not the in-process registry:**

| | Process-per-DB | In-process registry |
|---|---|---|
| Effort | ~1 week, no core changes | 3–4 weeks, every module |
| Isolation | Absolute — no shared pool, no shared cache, no shared singleton | Depends on getting ~40 sites right |
| Cross-tenant data leak | Structurally impossible | One missed cache key away |
| Blast radius of a crash | One tenant | All tenants |
| Session isolation | Free (per-process, per-subdomain cookie) | Must be enforced |
| Memory | ~N × baseline | Shared |
| Cross-company reporting in one UI | Not possible | Possible |

Isolation is the actual reason to want separate databases, and process-per-DB delivers it
structurally rather than by discipline. It also sidesteps every singleton listed in §2.2.

**Cost accepted:** memory scales with tenant count (measure the RSS of one instance before
committing to a tenant count — Drogon + pool + OWL static serving is not free), and no single
UI spanning companies. Revisit the in-process registry only if tenant count passes ~10 or
cross-company reporting becomes a real requirement — by then there will be tests to do it
safely.

### 2.4 Design

**Tenant registry** — `config/tenants.json`, read by the supervisor, not by the app:

```json
[ {"slug":"acme",  "db":"erp_acme",  "port":8069, "display":"Acme Storage"},
  {"slug":"globex","db":"erp_globex","port":8070, "display":"Globex Depot"} ]
```

**Per-tenant config** — `config/<slug>.cfg`, identical to today's `system.cfg` but with
`db_name` and `http_port` varying. `main.cpp` already accepts a path
(`AppConfig::fromFileOrEnv("config/system.cfg")`); change it to read `argv[1]` with the
current path as the default. **~5 lines — the only application code change required.**

**Supervision** — one systemd template unit `c-erp@.service`, instantiated per slug
(`systemctl enable c-erp@acme`). Each instance runs its own `MigrationRunner` and
`ensureSchema_()` against its own database at boot, which is exactly the desired behaviour.

**Routing** — nginx `server` block per subdomain, `proxy_pass` to the tenant port, plus
`set_real_ip_from 127.0.0.1; real_ip_header X-Forwarded-For;` (required by S-40).

**Login and switching** — cookies are naturally scoped per subdomain, so sessions are isolated
with no work. A user logs into their company's subdomain. An admin switcher is a menu listing
the companies that admin may access, linking to each subdomain; switching re-authenticates.

> Do **not** scope the session cookie to the parent domain to make switching seamless. That
> re-couples the tenants, and combined with S-42 (fixation) it would let a session minted on
> one tenant be presented to another.

**Provisioning** — `scripts/new_tenant.sh <slug> <db>`: `createdb`, render the cfg from a
template, add the nginx server block, `systemctl enable --now c-erp@<slug>`, reload nginx.
Schema is created by the app on first boot.

**Central admin identity** is explicitly out of scope. Each tenant has its own `res.users`.
An admin needing three companies has three accounts. Cross-tenant SSO is a Stage 7 topic and
should not be improvised.

### 2.5 Effort

| Item | Effort |
|---|---|
| `main.cpp` accepts config path argument | 1 h |
| `config/tenants.json` + per-tenant cfg template | 2 h |
| systemd template unit | 3 h |
| nginx per-tenant server blocks + `real_ip` | 3 h |
| `new_tenant.sh` provisioning script | 4 h |
| Company switcher UI (admin menu → subdomain links) | 4 h |
| Per-tenant backup/restore + a real restore drill | 1 d |
| Memory measurement and tenant-count ceiling | 2 h |
| **Total** | **~1 week** |

---

## 3. Rental module (`modules/rental`)

Warehouse storage rental: lockers and rooms let to customers, open-ended until cancellation,
recurring invoicing, expense tracking, dashboard and an event log.

### 3.1 Data model

**`rental.unit.type`** — Small Locker / Large Locker / Room / Pallet Space.
`name`, `code`, `default_rate`, `default_period`, `tax_ids`, `area_sqm`, `volume_m3`, `active`.

**`rental.unit`** — the physical lettable space.
`code` (unique), `name`, `type_id`, `site_id`, `zone`, `floor`, `area_sqm`, `volume_m3`,
`state` ∈ {available, reserved, occupied, maintenance, retired}, `location_id` (optional link
to `stock.location` if the space is also an inventory location), `notes`, `active`.

`state` is **derived** from active contract lines, never hand-edited except for
maintenance/retired. Computing it from the lines avoids the classic double-let bug.

**`rental.contract`** — the customer agreement.
`name` (from `ir.sequence`), `partner_id`, `state` ∈ {draft, active, cancelled, closed},
`date_start`, `date_cancelled`, `billing_period` ∈ {monthly, quarterly, yearly},
`payment_term_id`, `deposit_amount`, `deposit_state` ∈ {none, held, refunded, forfeited},
`currency_id`, `notes`.

**`rental.contract.line`** — one per rented unit. **This is where the per-unit dates live.**
`contract_id`, `unit_id`, `date_start`, `date_end` (NULL = open-ended), `unit_price`,
`discount_pct`, `billing_anchor_day`, `next_invoice_date`, `invoiced_through`,
`proration_policy` ∈ {full_period, prorate_days, start_next_cycle}, `state`.

> You said customers rent multiple units with different start dates and therefore different
> due dates. That is modelled by giving **each line its own `next_invoice_date`**, derived
> from its own `date_start`. The billing run groups lines by `(contract_id, next_invoice_date)`
> — lines that happen to share a due date are combined onto one invoice, lines that don't
> produce separate invoices with separate due dates. No configuration needed; it falls out of
> the data.

**`rental.invoice.link`** — what a generated invoice covers, and the idempotency guard.
`move_id`, `contract_line_id`, `period_start`, `period_end`, `amount`.

```sql
UNIQUE (contract_line_id, period_start)
```

**This constraint is the most important line in the module.** It makes double-billing
impossible even if the cron fires twice, the process restarts mid-run, or someone triggers a
manual run. Every billing bug in systems like this traces back to not having it.

**`rental.expense.category`** — `name`, `account_id`, `is_operating`.

**`rental.expense`** — both one-off and recurring.
`date`, `name`, `category_id`, `amount`, `partner_id` (vendor), `unit_id` (NULL = site-wide),
`contract_id` (NULL = not rechargeable), `account_id`, `state` ∈ {draft, posted, cancelled},
`move_id` (vendor bill, if posted), `attachment_id`,
`is_recurring`, `recurrence_interval` ∈ {monthly, quarterly, yearly}, `recurrence_next_date`,
`recurrence_end_date`, `recurrence_parent_id`.

A recurring expense is a **template row** (`is_recurring = true`) that the cron clones into
dated child rows (`recurrence_parent_id` set). Same idempotency discipline as invoicing:
`UNIQUE (recurrence_parent_id, date)`.

**`rental.event`** — the domain event log.
`occurred_at`, `event_type`, `contract_id`, `line_id`, `unit_id`, `partner_id`, `user_id`,
`summary`, `detail` (JSONB), `ref_model`, `ref_id`.

> Keep this **separate from `audit_log`**. `audit_log` is CRUD forensics (who wrote what row);
> `rental.event` is business narrative (contract activated, unit released, invoice generated,
> payment applied, contract cancelled, rate changed, maintenance opened). Conflating them
> produces a log that is bad at both. The dashboard's activity feed reads `rental.event`.

Event types: `contract_created`, `contract_activated`, `line_added`, `line_started`,
`unit_assigned`, `unit_released`, `rate_changed`, `invoice_generated`, `payment_received`,
`payment_applied`, `invoice_overdue`, `contract_cancelled`, `deposit_held`,
`deposit_refunded`, `maintenance_opened`, `maintenance_closed`.

### 3.2 Billing engine

A daily `ir.cron` job:

1. Select active lines where `next_invoice_date <= today`.
2. Group by `(contract_id, next_invoice_date)`.
3. Per group, compute `period_start` / `period_end` from `billing_period`.
4. Apply `proration_policy` to the **first** period only.
5. Create `account.move` (`out_invoice`) with one line per contract line; taxes from
   `unit_type.tax_ids`; due date from `payment_term_id`.
6. Insert `rental.invoice.link` rows — the UNIQUE constraint aborts a duplicate run.
7. Advance `next_invoice_date`, set `invoiced_through = period_end`.
8. Emit `rental.event: invoice_generated`.

Whole run in **one transaction per contract group**, so a failure on one customer cannot
half-bill another. Log and continue on per-group failure; never abort the run.

A manual "Generate invoices now" action runs the same code path with a date parameter —
never a second implementation.

**Open-ended by default:** `date_end` NULL means billing continues until cancellation.
Cancelling sets `date_cancelled`, closes the line at the period end (or prorates the refund,
per policy), releases the unit, and emits `contract_cancelled`.

### 3.3 Payments in advance and in arrears

This is the requirement with the most hidden depth, and it needs one piece the ERP lacks:
**partial payment allocation**.

- **Advance payment** — an `account.payment` larger than, or earlier than, any open invoice
  leaves an unallocated credit on the customer. The next generated invoice auto-applies it,
  oldest-open-first (configurable), and emits `payment_applied`.
- **Late payment** — an invoice past `date_due` with residual > 0 is overdue. Aging buckets
  (0–30 / 31–60 / 61–90 / 90+) are computed in the dashboard query, not stored.
- **Deposit** — held as a liability, not revenue. `deposit_state` tracks
  held → refunded/forfeited on cancellation. Never auto-applied to rent; that is a decision, not
  a default.

**Required:** a minimal `account.partial.reconcile` — `(payment_id, move_id, amount, date)` —
plus residual computation on `account.move`. Build it in the **account** module, not the
rental module: it is general ERP functionality and putting it in rental guarantees it gets
rebuilt later. ~1 week (Stage 5).

Late fees are deliberately **out of scope for v1**. They interact with tax, credit notes and
jurisdiction-specific rules; add them once the base billing loop has run for a few cycles.

### 3.4 Dashboard

One endpoint, `GET /rental/dashboard`, returning a single JSON payload from a handful of
aggregate queries, cached in `TtlCache` for 60 s.

> Do **not** assemble this from N `search_read` calls in the frontend. It is the fastest way
> to a dashboard that takes 4 s to paint and hammers the pool. One endpoint, one cache entry.

| Panel | Content |
|---|---|
| Occupancy | occupied / available / reserved / maintenance; vacancy %; by unit type |
| Revenue | MRR, ARPU, revenue MTD vs prior month, annualised run-rate |
| Receivables | total outstanding; aging 0–30 / 31–60 / 61–90 / 90+; top 10 debtors |
| Upcoming | invoices due in next 7 / 30 days; contracts starting; contracts cancelling |
| Expenses | MTD split recurring vs one-off; by category; **NOI = revenue − expenses** |
| Churn | cancellations last 90 d; average tenancy length; units vacant > 30 d |
| Attention | overdue > 60 d; units in maintenance; contracts in draft > 7 d; unallocated payments |
| Activity | last 20 `rental.event` rows |

Per-unit profitability (revenue − allocated expenses) is the report that justifies the
`unit_id` column on `rental.expense`; include it as a drill-down, not on the main dashboard.

### 3.5 Portal

Customers see their own units, contracts, invoices, payment history and outstanding balance.
This runs through the existing portal module and **requires `ir.rule` record rules scoped by
`partner_id`** — the portal is the public surface, and `038` S-41/S-47 are both reminders of
what happens when a model joins the system outside the framework.

### 3.6 Module structure

Per `CLAUDE.md` PERF-E, and per §1.2 — the rental module must **inherit** the audited,
rule-enforcing, OCC-guarded path rather than hand-rolling ViewModels:

```
modules/rental/
  RentalModule.hpp          declarations only
  RentalModule.cpp          models, ViewModels, routes, migrations
  RentalBilling.hpp/.cpp    billing engine (cron entry + manual action)
  RentalDashboard.hpp/.cpp  aggregate queries + cache
```

Use `GenericViewModel<T>` wherever the model is plain CRUD (`rental.unit.type`,
`rental.unit`, `rental.expense.category`). Only `rental.contract`, `rental.contract.line` and
the dashboard need custom ViewModels — and those must audit, merge the rule domain, and honour
`__expected_write_date`.

Migration range: **800–899** (extends the table in `docs/036` §3, which currently stops at 799).

### 3.7 Dependencies — read before scheduling

| Needs | Why | Status |
|---|---|---|
| **`ir.cron`** | Recurring invoicing and recurring expenses. There is no scheduler at all | Missing (Stage 5) |
| **`ir.sequence`** | Contract and invoice numbering; invoice numbering is a legal requirement | Missing (Stage 5) |
| **Tax engine** | Rent is taxable. Without it every rental invoice total is wrong | Missing (Stage 5) |
| **Payment allocation** | The advance/late payment requirement *is* allocation | Missing (Stage 5) |
| `ir.attachment` | Signed contracts, expense receipts | Missing (Stage 5) |
| `ir.mail_server` | Invoice delivery, overdue reminders | Missing — v2 |
| `stock.quant` | — | Not needed |

The first four are **hard blockers**. A rental module built before them would ship its own
scheduler, its own numbering and its own tax arithmetic — three pieces of core ERP
infrastructure, built inside a vertical module, that would then have to be torn out. The
rental requirement is in fact the clearest justification the ERP has for building them
properly, and Stage 5 is scheduled on that basis.

### 3.8 Effort

| Item | Effort |
|---|---|
| Models, schema, migrations, seed data | 1.5 w |
| Contract lifecycle (activate, add/remove line, cancel, unit state derivation) | 1 w |
| Billing engine + idempotency + proration + manual run | 1.5 w |
| Payment allocation wiring, deposits, overdue detection | 1 w |
| Expenses: one-off, recurring templates, cron generation | 1 w |
| Dashboard: queries, caching, OWL views | 1 w |
| Event log + activity feed | 3 d |
| Portal views + record rules | 1 w |
| Tests (TEST-1) | folded into each item |
| **Total** | **~7 weeks** |

---

## 4. Timeline

```
Stage 1  Security pre-deploy          2.5 w   ██████
Stage 2  VM verification              0.2 w   ▌
Stage 3  Test suite                   1.0 w   ██
         ── deployable here (~3.7 w) ──
Stage 1b Fix the ViewModel pattern    0.6 w   █▌
Stage 4  Multi-company                1.0 w   ██
Stage 5  Primitives                   5.5 w   █████████████
           ir.sequence      0.8 w
           ir.cron          0.8 w
           tax engine       2.0 w
           payment alloc    1.0 w
           ir.attachment    0.8 w
Stage 6  Rental module               7.0 w   █████████████████
                                     ─────
                                     ~18 weeks (~4.5 months)
```

Stages 4 and 5 are independent and can run in parallel with two people. Stage 5's tax engine
is on the critical path to Stage 6 and is the item most likely to overrun — it is the piece I
would prototype first.

---

## 5. Cross-cutting rules for all new code

From `CLAUDE.md`: SEC-28, SEC-29, S-33, PERF-E, PERF-F. From `038` §5: SEC-30, SEC-31,
SEC-32, TEST-1. Three more earned by this plan:

| Rule | Requirement |
|---|---|
| **ARCH-1** | New models default to `GenericViewModel<T>`. A custom ViewModel must audit, merge the rule domain, and honour `__expected_write_date` — enforced by the boot-time assertion in §1.2. |
| **ARCH-2** | Every generated document (invoice, recurring expense) carries a UNIQUE constraint on its natural key `(source_id, period)`. Generation must be safe to re-run. |
| **ARCH-3** | Tenant configuration lives outside the application. No code path reads a tenant list, resolves a database by name, or branches on tenant identity. |
