# Test plan — the whole ERP

What the system exposes, what the suite covers, and what is still missing.
Measured, not guessed: the surface comes from `ir_ui_menu` joined to
`ir_act_window`, and coverage from grepping `tests/**/test.sh`.

Regenerate the numbers any time:

```bash
psql -h localhost -U odoo -d odoo -tAc \
  "SELECT DISTINCT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id
    WHERE a.res_model IS NOT NULL ORDER BY 1"
```

---

## 1. The surface

**12 apps, ~90 menu leaves, 87 distinct models reachable by clicking.**

| App | Covers |
|---|---|
| **Accounting** | invoices, bills, credit notes, refunds, payments, journals, assets, budgets, analytic, bank statements & reconciliation, financial reports, chart of accounts, currencies, fiscal positions, incoterms, lock dates |
| **Sales** | quotations → orders → invoicing |
| **Purchase** | purchase orders → receipts → bills |
| **Inventory** | transfers (receipt / delivery / internal), lots & serials, packages, landed costs, putaway, reordering, warehouses, locations, operation types, valuation, on-hand, move history |
| **Manufacturing** | BOMs, manufacturing orders, work orders, work centers, MPS |
| **Products** | products, templates, variants, attributes, categories, pricelists & rules, vendor pricelists, UoM, parts catalogue, parametric search, part lookup, footprints, part units |
| **Project** | projects, tasks, stages, task board, timesheets, timesheet grid |
| **Employees** | employees, departments, jobs, working schedules, expenses, expense sheets |
| **Rental** | units, unit types, contracts, events, expenses, expense categories, dashboard |
| **Contacts** | partners as customers, vendors, and portal users |
| **Settings** | companies, users, groups, portal users, ERP settings, document templates, database tools & backups |
| **Help** | help centre, help articles |

## 2. Coverage today

**88 tests.** Six functional journeys, ~60 integration tests, nine security
tests, three unit suites.

- **10 menu-reachable models had no test at all.** `07` and `08` closed three
  (`account.payment`, `account.analytic.line`, plus `stock.location` /
  `stock.picking.type` / `stock.quant.package` promoted out of "thin").
  **Still uncovered:** `account.report` · `bank.reconcile` · `barcode.scan` ·
  `company.admin` · `db.backups` · `part.search` · `project.task.type` ·
  `res.company`
- **34 more are touched by exactly one test**, several only incidentally
  (`stock.location`, `uom.uom`, `res.groups` and six others appear only in
  `core/new-forms`, which checks that a form *opens* — not that the model
  works).

`account.payment` is the sharpest gap: the journeys pay invoices through
`account.move.action_register_payment`, so **the payment model's own screens
have never been exercised**.

## 3. What a functional journey must do

Not "call every method". A journey is one continuous story, and it earns its
place by asserting things a per-module test structurally cannot:

1. **State after each step**, not one call in isolation.
2. **The cancel path**, not just the happy one — reversal, not deletion, for
   anything posted.
3. **A cross-module invariant at the end.** The books balance; stock ties out;
   what was consumed matches the BOM ratio; the report agrees with the ledger.
4. **Database validation, not just API responses.** Read the rows back. An API
   that returns `{"ok":true}` while writing nothing is the failure mode these
   catch.
5. **Clean up after itself**, with a distinctive prefix and a `trap`.

## 4. The journeys

Built (213 checks) — see docs/109 §3 for what each asserts:

| | Story | |
|---|---|---|
| `01-sell` | quote → deliver → invoice → pay → cancel & reverse | 40 |
| `02-buy` | PO → receive → bill → pay → cancel & reverse | 40 |
| `03-make` | BOM → MO → produce → conservation; cancel returns parts | 28 |
| `04-parts` | CSV BOM → resolve → commit → catalogue → labels | 27 |
| `05-project` | project → tasks → board → timesheets → drop work | 28 |
| `06-close` | post → all seven reports → reverse → exact round trip | 50 |
| `07-money-in-and-out` | invoice → part payment → balance → vendor paid → statement reconciled | 37 |
| `08-warehouse` | sub-location → receipt → transfer → lots → package → delivery | 28 |

### `07-money-in-and-out` ✅ built — 37 checks
`account.payment` · `account.bank.statement` · `account.analytic.line`

Invoice → **partial** payment → the balance → vendor bill paid → bank statement
reconciled → analytic items. Asserts the payment record itself (amount,
`payment_type`, `partner_type`), that a part-payment leaves the invoice open,
that a paid invoice refuses another payment, that no reconciled line points at
a missing move, and that every payment is backed by a journal entry.

### `08-warehouse` ✅ built — 28 checks
`stock.location` · `stock.picking.type` · `stock.quant.package` ·
`stock.production.lot` · `stock.valuation.layer` · `stock.move`

Sub-location → receipt → internal transfer → lots → package → delivery.
Asserts on-hand per location sums to the product's own figure, that an internal
move does not change the total, that no completed move is empty or goes
nowhere, and that stock is **conserved** — the sum across every location,
including vendor and customer, is zero.

> **Found a real bug on its first run.** `stock.move search_read` with any
> domain returned a masked "internal error": `column reference "state" is
> ambiguous`. The query joins `stock_picking`, which also has `state`, `name`
> and `company_id`, and the domain compiler emitted unqualified columns. So
> **filtering Inventory → Reporting → Moves History was broken**, while the
> unfiltered list worked — which is why no smoke test ever saw it.
> `Domain::toSql` now takes an optional table alias; `stock.move` passes
> `"sm"` and `stock.picking` passes `"sp"` (it had the same latent fault).
> **Any search_read whose SELECT joins another table needs an alias** — worth
> auditing the remaining custom readers.

---

## 4b. Proposal for audit — one test per page

Derived from [menu-coverage.md](menu-coverage.md): **106 menu options open a
page.** 8 have no test; 35 have exactly one, several only incidental.

**This table is the thing to review before any of it is written.** Say which
rows are wrong, which are not worth it, and which are missing.

### The 8 with no test at all

| Page | Model | Proposed test | Would assert |
|---|---|---|---|
| Financial Reports | `account.report` | extend `06-close` | already drives the seven reports via `/web/account/report`; add the **screen's own model** — saved report selection, date range, drilldown |
| Bank Reconciliation | `bank.reconcile` | extend `07-money-in-and-out` | the reconcile *screen*: suggested matches, partial match, match refused when amounts differ |
| Barcode | `barcode.scan` | `13-shop-floor` | scan a product, a lot, a location; an unknown barcode is refused, not guessed |
| Companies & Access | `company.admin` | `12-admin-and-recovery` | create a company, assign a user, verify the other company's rows are unreadable |
| Database & Backups | `db.backups` | `12-admin-and-recovery` | backup → change data → restore → the change is gone (docs/109 §9: the restore silently was not restoring) |
| ~~Parametric Search~~ | ~~`part.search`~~ | **removed** | Deleted, not tested — it was a strict subset of Parts Catalogue: same results, same SI shorthand, 83 lines against 470. Its distinctive case (`4k7` embedded-multiplier notation) moved into `part-catalog`'s test **first**, so the capability kept its coverage after the screen went. Closing an audit row by deleting the page is a legitimate outcome; losing the coverage with it is not. |
| Task Stages | `project.task.type` | extend `05-project` | create a stage, reorder it, mark it closing, delete one **that has tasks in it** |
| Companies | `res.company` | `12-admin-and-recovery` | second company, currency, its own journals; a company with entries cannot be deleted |

### The thin ones worth promoting

Grouped by the journey that should absorb them, rather than one test each.

| Journey | Absorbs | Why grouped |
|---|---|---|
| `09-people-and-expenses` | `hr.employee` `hr.department` `hr.job` `resource.calendar` `hr.expense` `hr.expense.sheet` | one story: hire → file → approve → post → pay |
| `10-rental-lifecycle` | `rental.contract` `rental.unit` `rental.event` `rental.expense` `rental.dashboard` | unit → contract → billing → event → expense → end |
| `11-configuration` | `uom.uom` `res.currency` `product.attribute` `account.account` `account.journal` `res.groups` `part.unit` `stock.picking.type` | CRUD **and consequence**: create, use in a transaction, then prove it cannot be deleted while in use |
| `14-accounting-config` | `account.account.type` `account.fiscal.position` `account.incoterms` `account.journal.group` `account.budget.post` `account.settings` | configuration that changes how documents post — each set, then a document posted through it |
| extend `03-make` | `mrp.workorder` `mrp.bom` | work orders are part of making something, not a separate story |
| extend `06-close` | `account.asset` `account.asset.type` `account.budget` `account.dashboard` | all are period-end views over the same ledger |
| extend `08-warehouse` | `stock.valuation.layer` | already asserted there; the thin mark is stale |
| render checks only | `project.board` `project.timegrid` `help.center` `help.article` `db.studio` `rental.demo.data` | these are screens, not logic — what matters is that they draw |

### Screens needing a render check

12 have none: Bank Reconciliation · Barcode · Parametric Search · Task Board ·
Timesheet Grid · Help Centre · BOM Editor · Financial Reports · Accounting
Dashboard · Rental Dashboard · Database & Backups · Companies & Access.

Each is one line once the journey exists:
`node tests/lib/render.mjs <menu path…> <selector>`.

### Suggested order

1. `11-configuration` — cheapest, widest, and hunts destructive deletes
2. `12-admin-and-recovery` — covers 3 of the 8 uncovered pages, and re-proves the restore
3. `09-people-and-expenses` and `10-rental-lifecycle` — two whole apps with almost no coverage
4. `14-accounting-config`, then the extensions to `03`/`04`/`05`/`06`/`07`
5. `13-shop-floor` (barcode) last — smallest surface
6. The 12 render checks, as each journey lands

---

**Planned**, in priority order. Each closes uncovered models and follows §3.

### `09-people-and-expenses` — the HR gap
`hr.employee` · `hr.department` · `hr.job` · `resource.calendar` ·
`hr.expense` · `hr.expense.sheet`

Hire an employee into a department and job. They file expenses; a sheet is
submitted, approved, posted, paid.
**Invariants:** the sheet's total equals its lines; posting creates balanced
entries against the employee's partner; an approved sheet cannot be edited.

### `10-rental-lifecycle` — the rental gap
`rental.contract` · `rental.unit` · `rental.event` · `rental.expense`

Unit created → contract signed → recurring billing runs → an event is logged →
an expense is booked → contract ends and the unit returns to available.
**Invariants:** a unit is never double-booked; billed periods match the
contract term; the dashboard's figures equal the underlying rows.

### `11-configuration` — the config-model gap
`uom.uom` · `res.currency` · `product.attribute` · `project.task.type` ·
`account.account` · `account.journal` · `res.company` · `res.groups`

Not a story but a **CRUD-and-consequence sweep**: create each configuration
record, use it in a transaction, then prove it cannot be deleted out from
under that transaction. Reference data that can be deleted while in use is the
bug this hunts.

### `12-admin-and-recovery` — the settings gap
`company.admin` · `db.backups` · `res.company` · `res.users` ·
`portal.partner`

A second company, a user restricted to it, a portal user. A backup is taken,
data changed, the backup restored, and the change is gone.
**Invariants:** the restore is complete (docs/109 §9 — it silently was not);
a company-restricted user cannot read the other company's rows.

## 5. Rendering

Every journey that has a screen should end with a render check —
`node tests/lib/render.mjs <menu path…> <selector>` — because an OWL template
error is invisible server-side. See [browser-render-checks.md](browser-render-checks.md).

Screens with no render check yet: Bank Reconciliation, Barcode, Parametric
Search, Task Board, Timesheet Grid, Help Centre, BOM Editor, Financial Reports,
Accounting Dashboard, Rental Dashboard, Database & Backups, Companies & Access.

## 6. Order of work

1. `07-money-in-and-out` — the largest correctness risk, and the biggest gap
2. `08-warehouse` — second largest; stock errors are silent and expensive
3. `11-configuration` — cheap, wide, and catches destructive deletes
4. `09-people-and-expenses`, `10-rental-lifecycle`
5. `12-admin-and-recovery`
6. Render checks for the twelve screens above

Add each as `tests/functional/NN-name/` with a `meta`, and update the table in
§4 and docs/109 §3 when it lands.
