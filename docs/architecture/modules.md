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
- `res.company` registers the letterhead, bank and `payment_term_days` columns
  that `CompanyIdentity` reads, so Settings edits them with a plain `write`.
  A rename is copied to the company's own contact. Changing `currency_id` is
  refused while a posted entry is in another currency, and rebases the rates —
  see [multi-company.md](multi-company.md#the-home-currency).

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

**`ir.config.parameter.set_params`** saves a whole settings page in one call:
`{key: value, …}`, upserted by key (the column is UNIQUE) in one transaction.
A screen must not write one setting per call — ERP Settings made twenty-six
round trips, which behind a CDN left its Save button on "Saving…" for seconds,
was not atomic, and wrote through row ids that startup could have deleted.
Values may be text, numbers or booleans; they are stored as text.

**AI settings** configure an external model provider for in-app assistance.
Three providers are seeded: `anthropic` (Claude), `xai` (Grok), and `mock`
(no network, for tests). Settings cover the model name, an output-token
ceiling, a daily call cap, a web-search toggle and two **timeouts**
(`reply_timeout_s`, `search_timeout_s`, 5–600 s): how long the model is given
to answer from memory, and how long when it browses.

### Asking is a job, not a held-open request

A model may think for minutes. A request held open that long has to be waited
for by three separate things — the browser, nginx, and any CDN in front of
them — and **the shortest one decides**. It was the browser at a fixed 45 s,
so a slower model produced an answer that arrived to a screen which had
stopped listening, and the person was told it had timed out when nothing had
(CERP-10).

So a part lookup is a row in `ir_ai_job` with a state:

```
ask_async → queued → running → done | failed | cancelled
                ↑                        ↓
            a worker thread          ask_status (polled, ~1.5 s)
```

| Call | |
|---|---|
| `ask_async` | creates the job, returns its id in milliseconds |
| `ask_status` | state, elapsed seconds, and the result once it is there |
| `ask_cancel` | stops waiting; whatever comes back is discarded |
| `ask_latest` | this caller's unfinished question, for a screen that has just loaded |

Every HTTP request is short again, so no proxy setting has to be raised — and
the answer **survives a reload, a closed tab and a dropped connection**,
because it is in the database rather than in flight. A pushed notification
over the existing websocket bus would make it snappier, but it cannot replace
this: a dropped socket still has to ask somewhere what happened, and that
somewhere is `ask_status`.

Rules the design turns on:

- **A job belongs to one person.** Ownership is checked on every status call,
  so an id is not a capability; a job that is not yours is refused in the same
  words as one that does not exist.
- **Bounded work.** Three questions in flight per person, six on the server —
  each one is a paid call and a thread — on top of the daily call cap.
- **Terminal, always.** Every path out of the worker writes a state, and a job
  still marked `running` at boot is failed on startup: its thread died with
  the process, and a screen politely polling it would otherwise wait for ever.
- **The long timeout is only for the background.** A synchronous caller — the
  Test button, the Help assistant, the BOM tidier — is capped at 55 s, below
  the proxy, because outliving the proxy does not produce a slow answer, it
  produces a 504 whose result nobody can read. Anything needing longer belongs
  in a job.
- Jobs older than seven days are pruned when the next one is created, so this
  is not a table that quietly keeps a year of questions and answers.

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

### The life of an invoice

**Every invoice is born a draft.** Whatever raises it — the invoice screen, a
sales order, a rental contract's *Create Invoice*, the rental billing cron —
writes `state='draft'` and `name='/'`. Nothing is numbered and nothing is owed
until someone confirms it.

`action_post` is the only step that changes that. It takes the next number from
`account.move.INV` (customer invoices), `account.move.RINV` (credit notes) or
the journal's own sequence, checks the lock dates, generates the analytic
lines — and **spends any credit the customer already has on account**: a tenant
who paid six months up front has that advance drawn down as each invoice is
posted, because posting is when the invoice first owes anything.
`PaymentAllocation` settles posted moves only, which is why this happens here
and not at creation.

A posted invoice then has four endings, and the form states which in a corner
ribbon rather than leaving "Posted" to mean all of them:

| Ending | How | What the form shows |
|---|---|---|
| unpaid | nothing yet | no ribbon — the status bar says Posted |
| paid / part paid | Register Payment | **Paid** / **Part paid** |
| cancelled | Cancel (`button_cancel`) | **Cancelled**, with *Reset to Draft* |
| reversed | Add Credit Note, then confirm it | **Reversed** |

Reversal is not a state on the invoice: the credit note carries
`reversed_entry_id` pointing back at it, and only a **posted** credit note
counts — a draft one reverses nothing. `tests/functional/account/invoice-states`
drives all four endings through the screen.

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

### The parameter vocabulary

`part_parameter_keyword` is the list of quantities this catalogue measures;
`part_parameter_alias` holds every spelling that resolves to one. Both are
matched on `norm` — lowercase, letters and digits only — so `Rds(on)`,
`RDS_ON` and `rds on` are one name.

It exists because a parameter name is free text by design: an agent reading a
datasheet may send anything. Without a vocabulary, "Resistance", "Ohms" and
"resistance (Ω)" become three parameters, each looking fine on its own product
and none of them finding the others in a parametric search.

**A name the vocabulary does not know is never an error.** `submit` stages the
part and attaches a suggestion — a known spelling is corrected outright, a near
miss names what it is probably meant to be, and a genuinely new name says so.
The fuzzy step is a plain edit distance, with 0.72 as the line between "a
misspelling of this" and "a different quantity sharing some letters".

A person decides on **Products ▸ Configuration ▸ Parameter Keywords** (menu 88,
action 130, `ParamKeywords.js`), which lists every unmatched name in the
catalogue with the two answers next to it:

| | |
|---|---|
| **Add as new** | adopt the name as its own parameter |
| **Merge into** | rename it on every product that used it, and keep the old spelling as an alias |

Merge is the one that writes to products — it is what puts the parts back in
each other's search results. The alias it leaves behind is why the same
question is never asked twice.

The seed is the standard electronics quantities plus their datasheet
spellings, and then **every name already in the catalogue is adopted**: a live
database has a vocabulary whether or not anyone wrote it down, and opening the
screen to a list of false alarms would teach people to ignore it. Two seeded
entries carry `advice` instead of measuring anything — `Package` and
`Operating Temperature` — because those are the two ways a lookup produced a
plausible, completely wrong number: `"0603"` reads as 603, and `"-55 to 125"`
reads as −55.

`tests/integration/product/param-vocabulary` pins the matching, the two
actions and the merge's effect on product rows.

### Units, and the two dead ends on the review desk

The same problem arrives one level down: a new part brings a **unit** nobody
has entered (`Mbps`) or a **value that is not a number** (`MIPS32 M4K`,
`10/100`). Both were errors with nothing to click — the message said "see
describe.units", which is advice to an agent, not an action a person can take.

Both now carry their fix in the issue, and the review desk renders it as a
button:

| Error | Button | What it does |
|---|---|---|
| unknown unit | *Add `Mbps` as a unit…* | `part.unit.create_unit` — asks what it measures and how it converts |
| value is not a number | *Keep it as text* | sets `"text": true`, storing it in `value_text` |

A unit is added through `create_unit` rather than a plain `create` because
`quantity_kind`, `factor` and `is_base` are not registered fields: they are
what makes 4.7 kΩ and 4700 Ω the same number, and a unit without them is a
label that breaks every range search it touches. The **first** unit of a
quantity becomes its base with factor 1; any later one must give the factor
that converts it to that base.

Either fix-up re-runs `update`, which is the same validation `submit` runs, so
the proposal moves itself out of *Needs fixing* rather than waiting to be
re-submitted.

While fixing this, `looksLikeText` gained the **pair** case: `10/100` parsed
as 10 and the second speed was dropped, exactly as `-55 to 125` once became
−55. Pairs and ranges are now kept whole as text and reported at `info` level,
rather than silently becoming a number that reads like a specification.
`tests/functional/product/lookup-fixups` drives both buttons through the
screen.

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
`project.timesheet`, `project.tag` (labels). The task board, the ticket
screen and the timesheet grid are dedicated frontend components.

**The issue tracker.** A task is a ticket — this is what c-erp's own
development is managed in. Migrations 1100–1101 (range 1100–1199):

- **Keys.** Every project has a `task_prefix` (typed, or derived from the name:
  "c-erp" → CERP, "Easy Locker Space" → ELS; unique, 2–10 letters/digits). A
  task takes the project's next number on insert — `CERP-12` — from
  `UPDATE project_project SET task_seq = task_seq + 1 … RETURNING`, which
  row-locks the project, so concurrent creates never share a number. The
  trigger `project_task_key_trg` is the **only** writer of `number`, `key`
  and `display_name` ("CERP-12 Fix the save button"): a client's values are
  dropped by the view model and overwritten by the trigger. Moving a ticket
  to another project gives it that project's next key; renaming a prefix
  re-keys the project's tickets.
- **Type** (`task`/`bug`/`feature`/`chore`) and **priority** (-1 Low, 0 Normal,
  1 High, 2 Urgent — an integer, so `priority DESC` is urgency order; the old
  star was 1) are CHECK-constrained and checked in `write`, which does not run
  `validate()`.
- **Reporter** defaults to whoever created it. **Watchers**
  (`project_task_watcher_rel`): the reporter, the assignee and anyone who
  comments are added automatically. **Labels** (`project_task_tag_rel`) are
  shared across projects; `set_tags` takes ids or names and creates a label
  from a name, matched ignoring case.
- **History.** `create`, `write`, `move_stage` and `set_tags` write a
  `mail_message` with subtype `tracking` — "Status: New → In Progress" — with
  values captured as labels, not ids, so it still reads right after a stage
  is renamed. A write that changes nothing writes nothing.
- **Comments** (`post_comment`, `edit_comment`, `delete_comment`) are authored
  by the session's user — `mail.message create` takes `author_id` from the
  client and is not used for them. Only the author, or an admin, may edit or
  delete one; `write_date` on `mail_message` marks it edited. Screenshots are
  ordinary attachments on the ticket, referenced in text as
  `![name](/web/content/<id>)`.
- `task_detail` and `activity` feed the ticket screen; `board` takes
  `issue_type`, `tag_id` and `q` (key or title, matched literally). Every raw-SQL
  method first reads the ticket through the ORM (`requireTask_`), so record
  rules and the company boundary apply as they do to `read`. Deleting a ticket
  deletes its comments and history.

## api

API keys and the REST API at `/api/v1` — for scripts, CI and AI agents
([../reference/api-v1.md](../reference/api-v1.md)). `res_users_apikey`
(migration 1200; range 1200–1299) holds keys as SHA-256 hashes with scopes,
optional project limits, expiry and revocation; `api.key` (JSON-RPC, session
only) creates, lists and revokes them; Settings → Users & Access → API Keys is
the screen (`ApiKeys.js`). The first area is the issue tracker: its routes
resolve tickets by key and call the `project.task` view model as the key's
owner, so every rule and history line is the screen's. Registered last in
`main.cpp`: its menu hangs under IrModule's Settings tree.

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

### Whole calendar months

`rental_contract.whole_month_billing` (migration 821) changes what a period
**is**:

| | anniversary (default) | whole month |
|---|---|---|
| a contract begun on the 7th | 7 Aug – 6 Sep | **1 – 31 Aug** |
| the invoice line reads | `2026-08-07 to 2026-09-06` | **`August 2026`** |

Most storage businesses charge for *August*; the day someone moved in is a
detail of that month, not the start of a new calendar. The setting is **off by
default** because it changes what an invoice covers, so an existing contract
bills exactly as it did until somebody ticks the box.

**Per line** (`rental_contract_line.billing_span`, migration 822):
`contract` (the default — follow the contract), `month` (bill this line as a
whole calendar month whatever the contract says) or `dates` (bill its exact
period). A locker let by the month and a storeroom let for a fortnight can
share one contract. An invoice is labelled as a month only when **every** line
in it is.

**When the rent is owed** (`rental_contract.due_on_move_in`, migration 822).
The due date is normally the day the period starts, which under whole-month
billing is the 1st. With this on, the invoice falls due on the tenant's
**move-in day inside that period** — they moved in on the 9th, so each month's
rent is due on the 9th. The day comes from the line's `billing_anchor_day`,
else the day its `date_start` falls on. A month without that day (a 31st in
February) falls back to the **last day of that month**, because skipping the
month would be worse. Both settings are on the contract form, and
`tests/integration/rental/contract-invoice` §11 pins all three cases.

The snap happens in SQL, in the same statement as the anchor arithmetic, so the
period that is printed, the period that is stored, and the next due date cannot
drift apart. Only the FIRST period differs between the two modes anyway —
`rental_next_period` anchors to `billing_anchor_day`, which defaults to 1, so
every later period already landed on the 1st.

An end date is optional and always was: a line with `date_end` NULL runs until
it is terminated. *Clearing* one was the part that failed — an emptied date
input sends `""`, `DATE` rejects it, and the operator met "An internal error
occurred". `normalizeForDb_` now maps an empty string to NULL for Date,
Datetime and Many2one — and deliberately not for Char, Text or Selection,
where a blank is either a real value or already sent as null.

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

### What billing produces, and where it shows up

Billing writes a **draft**, like every other producer of an invoice
([The life of an invoice](#the-life-of-an-invoice)): `state='draft'`,
`name='/'`, no sequence consumed. It used to post the invoice itself and apply
the tenant's advance at the same moment; that work now happens once, in
`action_post`, so a rental invoice and a hand-made one behave identically and
the number belongs to a document somebody confirmed.

That moves rent through two panels while it is unconfirmed, and both had to
learn about it:

- **the cashflow forecast** counts drafts as receivable. Billing advances
  `next_period_start`, which takes the month out of the projection; if the
  draft were not counted, a month of rent would vanish from the forecast the
  moment it was billed and reappear only on posting.
- **the dashboard** counts them in *needs attention* as "Invoices still in
  draft". Outstanding receivables and the ageing buckets stay posted-only —
  they are ledger figures, and a draft is in no ledger.

`tests/integration/rental/rental-billing` §10 pins the draft and the advance,
`rental-cashflow` §8 the forecast and `rental-dashboard` §6b the split.

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
