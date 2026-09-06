# The modules

Twenty modules, registered in `main.cpp` in dependency order. Each owns its
tables, its models, its ViewModels, its views, its menu entries and any HTTP
routes it needs.

Every module follows the mandatory `.hpp` (declaration) / `.cpp`
(implementation) split — see [../development/conventions.md](../development/conventions.md).

| Module | Models | Tables | What it owns |
|---|---:|---:|---|
| [base](#base) | 5 | 5 | partners, countries, currencies, languages; the ORM itself |
| [auth](#auth) | 3 | 6 | users, groups, companies, login, signup, password reset — and it creates `ir_cron`, though the scheduler that drives it lives in `ir` |
| [mail](#mail) | – | 1 | chatter messages on any record |
| [ir](#ir) | 7 | 11 | menus, actions, attachments, rules, cron, audit log, AI settings |
| [account](#account) | 24 | 24 | the whole accounting build |
| [uom](#uom) | 1 | 1 | units of measure |
| [product](#product) | 12 | 16 | products, templates, variants, pricelists, electronic parts |
| [sale](#sale) | 2 | 2 | quotations and sales orders |
| [purchase](#purchase) | 2 | 2 | RFQs and purchase orders |
| [hr](#hr) | 11 | 11 | employees, expenses, attendance, leave, the kiosk |
| [stock](#stock) | 13 | 13 | warehouses, transfers, quants, valuation, lots, landed costs |
| [mrp](#mrp) | 8 | 8 | BOMs, manufacturing orders, work centres, MPS |
| [project](#project) | 4 | 4 | projects, tasks, timesheets |
| [help](#help) | 1 | 1 | the in-app Help Centre |
| [bom](#bom) | – | 1 | the BOM editor and its staged importer |
| [report](#report) | – | 1 | PDF/HTML document rendering and templates |
| [portal](#portal) | – | 3 | the customer portal |
| [rental](#rental) | 7 | 8 | rental units, contracts, billing, expenses, forecast |
| [website](#website) | 5 | 6 | the public CMS, its editor, media and forms |
| *(core)* | – | 4 | `schema_migrations`, `decimal_precision`, `ir_sequence`, `account_partial_reconcile` |
| *(control plane)* | – | 2 | `mc_membership`, `mc_shared_product` — in the **separate** `mc_control` database, not in a tenant |

**128 tables in a tenant database — plus 2 in the control plane — and 105
models in total.** The model count is the names registered with the *model*
factory; ViewModel-only registrations (list, form and dashboard viewmodels —
106 of those) are not counted as models. A dash means the module registers
ViewModels but no models of its own: `mail`, `report` and `portal` define
`BaseModel` classes they drive internally, and `bom` operates on `mrp`'s.

Each table is counted **once**, under a module that has its `CREATE TABLE`. Two
are created in more than one place — `ir_cron` and `ir_sequence` are ensured
both by `auth` and by `core/MoneyMigrations.cpp`, so that a database provisioned
by either route is complete. `ir` reads and schedules from them but does not
create them.

`./tests/tools/audit_schema_doc.sh` checks the per-table detail in
[../reference/database-schema.md](../reference/database-schema.md) against a
live database.

---

## base

`modules/base/` — and the ORM every other module builds on.

`res.partner`, `res.country`, `res.country.state`, `res.currency`, `res.lang`.

The infrastructure here is the important part:

| File | What it is |
|---|---|
| `BaseModel.hpp` | the CRTP ORM base — fields, CRUD, `search_read`, `read_group` |
| `FieldRegistry.hpp` | the per-model column allowlist; the basis of S-49 |
| `Domain.hpp` | reference-ERP domain syntax → parameterised SQL |
| `RecordRuleSql.hpp` | threads `ir.rule` filters into hand-written SQL reads |
| `GenericViewModel.hpp` | CRUD ViewModel any model can use unchanged |
| `BaseView.hpp` / `BaseViewModel.hpp` | view metadata; the `REGISTER_METHOD` dispatch table |
| `PartnerMigrations.cpp` | the partner-hierarchy migrations (companies ↔ contacts) |
| `WorldData.hpp` | seeded country and state reference data |

`res.partner` carries the full contact hierarchy: a company partner and its
child contacts, with the child inheriting the parent's address unless it
overrides it.

### How a contact is labelled

A person is shown with their company, an organisation with itself:

| the row | `display_name` |
|---|---|
| company "Big Carrots" | `Big Carrots` |
| Carol, `parent_id` → Big Carrots | `Carol, Big Carrots` |
| Carol, no company | `Carol` |
| Carol, free-text `company_name` "Big Carrots" | `Carol, Big Carrots` |

The company part is `commercial_company_name` — the commercial parent's name
when that is a company, else the row's own free text — so an imported contact
that never had a company *record* reads the same as a linked one. The suffix is
dropped when it would repeat the name.

`display_name` is a **stored column**, maintained by trigger (migration 15),
not composed at read and not composed by the client:

- a client cannot format what a client never sees consistently — there are
  forty-odd pickers and several choose their model at runtime
  (`<M2OSelect model="f.relation"/>`), so "update every call site" cannot be
  finished;
- a picker has to `ORDER` and `ilike` on it. Typing "Big Carrots" into a
  Customer box finds the people who work there, which a computed value could
  not support.

Renaming a company cascades to every contact beneath it, and re-parenting or
promoting a contact to a company recomputes its own. The field is registered on
the model so `search_read` returns it and a domain may filter on it (S-49), but
it is **not** deserialised: accepting it from a client would let a contact
present itself under a company it does not belong to.
`tests/integration/core/partner-display-name` pins the rule and both cascades;
`tests/functional/base/partner-display-name` drives the pickers on screen.

### Deleting a contact

`PartnerService::unlink` refuses when any **document** refers to the contact,
and names what is in the way. `check_unlink` answers the same question without
deleting anything, so the form can say why before the user commits.

| | |
|---|---|
| **Blocks the delete** | invoices, journal items, payments, unallocated payments, sales orders (customer, invoice and delivery address), purchase orders, transfers, rental contracts / lines / expenses / events, customer rental prices, payment proofs, projects, tasks, employee records, user logins, companies |
| **Cleared, and reported first** | child contacts (`parent_id`), BOM subcontractor, reordering-rule supplier, analytic accounts, bank statement lines |
| **Removed with it** | supplier price-list entries and manufacturer records, which cascade |

The check cannot be left to the schema. **Five tables carry a `partner_id`
with no foreign key at all** — `rental_contract`, `rental_contract_line`,
`rental_event`, `rental_expense` and `account_payment_unallocated` — so
PostgreSQL would delete the contact and leave a rental contract pointing at a
customer that no longer exists. Where an FK does exist it is either `NO ACTION`,
which surfaces a constraint violation the user cannot act on, or `SET NULL`,
which silently strips the customer off a document.

**Archive is the way out.** `active = false` keeps every reference intact while
taking the contact out of lists and pickers: `PartnerService::searchRead` and
`searchCount` add `active = true` unless the caller's domain mentions `active`.
Naming it — either value — turns that off, which is how the list's "Show
archived" button gets back to one.

## auth

`res.users`, `res.groups`, `res.company` — plus `res_company_users_rel` and
`res_groups_users_rel`.

- PBKDF2-SHA512 password hashing (`AuthService.hpp`).
- 128-bit session ids from `RAND_bytes()`, stored in `SessionManager`.
- Sixteen built-in groups with fixed ids — see
  [../reference/id-registry.md](../reference/id-registry.md).
- `AuthSignupModule` registers `/web/signup` and `/web/reset_password`.
  Account creation is admin-only; resets are admin-issued.

## mail

`mail.message` — one polymorphic table (`res_model`, `res_id`) behind the
chatter panel on every document form. Messages are `note` or `comment`;
`AuditService` writes the automatic ones.

## ir

The technical model registry.

`ir.ui.menu`, `ir.actions.act_window`, `ir.attachment`, `ir.config.parameter`,
`ir.model.data`, `audit.log`, `decimal.precision` — plus the tables
`ir_rule`, `ir_rule_group_rel`, `ir_cron`, `ir_sequence`, and the three AI
tables `ir_ai_provider`, `ir_ai_prompt`, `ir_ai_settings`.

Menu and action ids are hardcoded and must never be reused; see
[../reference/id-registry.md](../reference/id-registry.md).

**AI settings** configure an external model provider for in-app assistance.
Three providers are seeded: `anthropic` (Claude), `xai` (Grok), and `mock`
(no network, for tests). Settings cover the model name, an output-token
ceiling, a daily call cap and a web-search toggle.

## account

The largest module: 24 tables of its own, plus `account_partial_reconcile` from
`core/MoneyMigrations.cpp`.

| Area | Tables |
|---|---|
| Chart of accounts | `account_account`, `account_account_type` |
| Journals | `account_journal`, `account_journal_group` |
| Entries | `account_move`, `account_move_line`, `account_partial_reconcile` |
| Payments | `account_payment`, `account_payment_term` |
| Tax | `account_tax`, `account_fiscal_position`, `account_fiscal_position_tax` |
| Banking | `account_bank_account`, `account_bank_account_line`, `account_bank_statement`, `account_bank_statement_line` |
| Assets | `account_asset`, `account_asset_type`, `account_asset_depreciation_line` |
| Budgets | `account_budget`, `account_budget_line`, `account_budget_post` |
| Analytic | `account_analytic_account`, `account_analytic_line` |
| Trade terms | `account_incoterms` |

Customer invoices, vendor bills, credit notes and vendor refunds are all
`account.move` with a different type. Financial statements, the SST-02 tax
report, lock dates and the accounting dashboard are served from here.

## uom

`uom.uom`, seeded with the standard categories and a factor to each category's
reference unit. Every quantity in the system is stored in micro-units
(int64, scale 6).

## product

Two layers stacked in one module.

**The general catalogue** — `product.template` and `product.product` (the
variant), `product.category`, `product.attribute` / `product.attribute.value`,
`product.pricelist` / `product.pricelist.item`, `product.supplierinfo` (vendor
pricelists), and the variant join tables.

**The electronics catalogue** — `part.footprint`, `part.parameter`,
`part.unit`, `part.manufacturer.info`, plus `part_lookup_result`, the staging
table behind the part-lookup agent API. Parametric search over typed
parameters with real units (kΩ, µF, V) is what the faceted parts catalogue
runs on.

`part.lookup` is a ViewModel, not a table-backed model: it exposes
`describe` / `submit` / `apply`. See
[../reference/part-lookup-api.md](../reference/part-lookup-api.md).

## sale

`sale.order`, `sale.order.line`. Confirming an order creates the delivery
picking; invoicing creates the `account.move`. Lines can be sections and
notes as well as products, and a pro-forma invoice can be printed before
confirmation.

## purchase

`purchase.order`, `purchase.order.line`. Mirror of sale: confirming creates a
receipt picking, billing creates the vendor `account.move`.

## hr

Employees and everything attached to them.

- `hr.employee`, `hr.department`, `hr.job`, `resource.calendar`
- `hr.expense`, `hr.expense.sheet`
- `hr.attendance` (`HrAttendance.cpp`) — check-in / check-out
- `hr.leave`, `hr.leave.type`, `hr.leave.allocation`, `hr.public.holiday`
  (`HrLeave.cpp`)
- `HrKiosk.cpp` — a standalone `/kiosk` page and `POST /kiosk/api/punch`, for
  a shared tablet at the door

## stock

The warehouse.

`stock.warehouse`, `stock.location`, `stock.picking`, `stock.picking.type`,
`stock.move`, `stock.quant`, `stock.quant.package`, `stock.production.lot`,
`stock.putaway.rule`, `stock.warehouse.orderpoint`, `stock.valuation.layer`,
`stock.landed.cost`, `stock.landed.cost.line`.

On-hand quantity and reservation are **not** computed here — every path goes
through `core/StockQuant`, so there is one answer. Costing supports three
methods and posts real-time GL entries through valuation layers.

## mrp

`mrp.bom`, `mrp.bom.line`, `mrp.production` (manufacturing orders),
`mrp.workorder`, `mrp.workcenter`, `mrp.routing.workcenter`,
`mrp.production.schedule` and `mrp.forecast` (the master production schedule).
Subcontracting backflushes through `StockQuant` like any other move.

## project

`project.project`, `project.task`, `project.task.type` (kanban stages),
`project.timesheet`. The task board and the timesheet grid are dedicated
frontend components.

## help

`help.article` — one row per article or section, with a stable slug, title,
keywords and a markdown body. Stored in the database rather than as static
pages so it stays searchable and citable; every module ships help content
(`HelpContent.hpp`, `HelpContentB.hpp`).

## bom

Sits in front of `mrp.bom` rather than replacing it.

- `bom.editor` — one BOM's lines with per-line status
- `bom.import` — a staged pipeline (parse → resolve → review → commit) backed
  by `mrp_bom_import_line`, so a CSV of part numbers is reviewed before
  anything is written

## report

`ir_report_template` plus the rendering pipeline. Documents render as HTML for
the browser's own print path, and as PDF server-side via `wkhtmltopdf`.
Layout is edited in the Document Layout Editor — see
[../guides/document-templates.md](../guides/document-templates.md) for the
user's view and
[../development/document-layout-editor.md](../development/document-layout-editor.md)
for the developer's.

## portal

The customer-facing site at `/portal`, with its **own** session store
(cookie `portal_sid`, 8-hour TTL) and its own per-IP login rate limiter.
Customers see their invoices, orders, deliveries, statement and rental units;
they can download PDFs, request a quote and upload a payment proof
(`payment_proof`, files under `data/payment_proofs/`).

`portal_access_token` grants document access by link;
`partner_rental_price` holds per-customer rental pricing.

Portal routes are registered directly with `drogon::app().registerHandler()`
and therefore do **not** inherit the `HttpServer` helper behaviour — every
portal lambda applies security headers, the `devMode` error gate and the
`Secure` cookie flag itself.

## rental

The storage-rental business built on top of the ERP.

`rental.unit`, `rental.unit.type`, `rental.contract`, `rental.contract.line`,
`rental.event`, `rental.expense`, `rental.expense.category`, plus
`rental_invoice_link`.

| File | Responsibility |
|---|---|
| `RentalUnits.cpp` | unit state derived from contracts and events, and the grid |
| `RentalBilling.cpp` | recurring invoicing, driven by `IrCron` |
| `RentalExpenses.cpp` | recurring expenses |
| `RentalForecast.cpp` | the cashflow forecast |
| `RentalDashboard.cpp` | the dashboard aggregates |
| `RentalCalendar.cpp` | day-level occupancy, and booking a unit from the calendar |
| `RentalEvents.cpp` | the unit event log |
| `RentalMigrations.cpp` | all rental DDL |
| `RentalDemo.cpp` | demo data, behind `/rental/demo/{seed,clear,status}` |

### The booking calendar

**Rental → Booking** (menu 315, action 127) is a day-level view of what is let:
a sidebar of types and units, a strip of day-boxes per unit, and a month grid
for one unit with the tenant on each day.

**Occupancy is derived, never stored.** A unit is occupied on day *D* when a
live line exists with `date_start <= D` and (`date_end IS NULL` or
`D <= date_end`) — the same dates `RentalBilling` reads, so the calendar shows
exactly what will be invoiced. Storing it as well would create a second source
of truth that drifts the first time someone edits a line. `date_end` is
**inclusive**: it is the last day of the let, which is how billing already
treats it, so the next booking starts the day after.

Booking from the calendar creates an ordinary `rental.contract.line`, opening a
contract when none is given. A dated booking defaults to `oneoff` billing and an
open-ended one to `recurring` — the other way round, a three-day cabin hire
would be invoiced every month for ever.

| Route | |
|---|---|
| `GET /rental/calendar?month=YYYY-MM[&type_id=N]` | the month, by unit and by type |
| `POST /rental/booking/create` | let a unit for a period |

#### The guard that had to change

Migration 803 enforced the double-let rule with a partial UNIQUE index on
`rental_contract_line(unit_id) WHERE state IN ('pending','active')` — **at most
one live line per unit**. That also made a booking calendar impossible: Alice
10–14 December and Bob 20–23 December could not both exist, so a unit could be
let exactly once.

Migration 820 replaces it with an **overlap exclusion**, which is strictly
sharper. The rule was never "one line per unit", it was *never two tenants in
one unit at the same time*:

```sql
EXCLUDE USING gist (unit_id WITH =,
                    daterange(date_start, COALESCE(date_end,'infinity'), '[]') WITH &&)
  WHERE (state IN ('pending','active') AND unit_id IS NOT NULL)
```

Two guards, deliberately. The constraint is race-proof and holds against
hand-written SQL, but needs `btree_gist` and its message is unreadable; a
`BEFORE` trigger names the dates that clash, and still enforces the rule where
the extension cannot be installed. Existing data cannot violate it — one live
line per unit is trivially non-overlapping.

`tests/integration/rental/booking-occupancy` pins the day arithmetic (clamping
at both month edges, open-ended lets, unions, retired units);
`tests/functional/rental/booking-calendar` books from the screen and proves
both halves of the guard — sequential lets allowed, overlapping lets refused.

### Invoicing one contract

`rental.contract.action_create_invoice` is the **Create Invoice** button on the
contract form, the shape `sale.order.action_create_invoices` has. It runs
`RentalBilling::run(db, "", contractId)` — the cron's code path, scoped — not a
second implementation, because a manual path that drifts from the scheduled one
is how double-billing is discovered in production.

Asking for one contract relaxes exactly two filters, and nothing else may:

| | |
|---|---|
| a **one-off / on-demand contract** | skipped by the cron by design. "On demand" means nothing happens until somebody demands it; this is the demand. |
| a **line with `billing_mode` one-off** | the Booking calendar writes dated bookings that way so the recurring engine leaves them alone — and nothing else billed them, so a booking could not be invoiced at all. |

The lead-day period gate and idempotency are **not** relaxed. Pressing twice is
safe, and the reply says which of the two honest outcomes applies: a recurring
line has advanced `next_period_start`, so the answer is "nothing is due yet"; a
one-off line's period start does not move, so `UNIQUE (contract_line_id,
period_start)` rejects the repeat and the answer is "already invoiced".

`tests/integration/rental/contract-invoice` pins the scope, the gate and the
idempotency; `tests/functional/rental/contract-invoice` presses the button.

### The billing period

A contract's cadence is one **preset** plus a derived `(interval, unit)` pair:

| `billing_period` | interval | unit |
|---|---|---|
| `daily` | 1 | day |
| `weekly` | 1 | week |
| `monthly` | 1 | month |
| `quarterly` | 3 | month |
| `biannual` | 6 | month |
| `yearly` | 1 | year |
| `custom` | **the user's X** | day / week / month / year |
| `oneoff`, `ondemand` | NULL | NULL |

`custom` is what makes "every X days/weeks/months/years" reachable for any X
from 1 to 366; the preset names the shape and the pair carries the number. A
trigger (`rental_contract_derive_period`) fills the pair in on every insert and
update, so the form, an import and a hand-written `INSERT` cannot disagree about
what "quarterly" means. `oneoff` and `ondemand` store NULL because they have no
interval at all, and the billing query excludes them outright — a COALESCE
falling back to monthly would invoice them silently, forever.

A line carries the same `billing_mode` vocabulary — `manual`, `recurring`,
`oneoff`, `ondemand` — so a single unit can be billed on demand under an
otherwise monthly contract.

A **line** may override its contract. `rental_contract_line.billing_interval`
and `billing_unit` are nullable, and NULL means *inherit*; `RentalBilling.cpp`
resolves `COALESCE(line, contract, 1/'month')`. They were `NOT NULL DEFAULT
1/'month'` when introduced, which made every line an implicit monthly override
and meant the contract's own period could never take effect.

`rental_next_period(from, anchor, interval, unit)` does the arithmetic; the
older three-argument form still means months. Month and year keep anchor-day
behaviour — a 31st anchor bills on 28 February and returns to the 31st in
March.

## website

The public CMS at `/site`, adapted from the reference ERP's `website` addon
with two deliberate departures:

- pages are served under `/site/...` because the ERP already owns `/`;
- page content is **typed blocks rendered by the server**, not author markup,
  so the ordinary blocks have no XSS surface at all.

`website.page` and `website.menu` (plus `website_page_revision`, which has no
model of its own), and `website.form` / `website.form.field` /
`website.form.submission`, registered from `WebsiteForm.cpp`.

Twenty-one block types render server-side (`WebsiteRender.cpp`): `heading`,
`text`, `image`, `button`, `divider`, `columns`, `hero`, `pricing`, `steps`,
`faq`, `references`, `map`, `video`, `gallery`, `quote`, `stats`, `cta`,
`table`, `spacer`, `form`, `html`.

`WebsitePalette.cpp` holds the site palette, `WebsiteMedia.cpp` the media
library, and `web/static/website-editor.js` the in-place editor.
`robots.txt` and `sitemap.xml` are generated from the published page set.
