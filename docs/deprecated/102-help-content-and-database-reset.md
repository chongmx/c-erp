# 102 — Help for every module, and a database reset

Status: **done**. `./scripts/run_tests.sh` → 74 passed, 0 failed
(`verify_help.sh` now 57 checks).

---

## 1. All fourteen books now have content

50 articles across 26 sections. Every tab in the Help Centre opens something.

| Book | Articles | Book | Articles |
|---|---|---|---|
| Project | 10 | Manufacturing | 2 |
| Accounting | 9 | Reporting | 2 |
| Parts | 6 | Using Help | 2 |
| Inventory | 6 | Sales | 1 |
| Products | 4 | Purchase | 1 |
| Settings | 4 | Rental | 1 |
| | | Employees | 1 |
| | | Contacts | 1 |

Content lives in `modules/help/HelpContent.hpp` (Project, Using Help) and
`HelpContentB.hpp` (everything else), split only for size.

**Every path quoted in an article was taken from `ir_ui_menu`, not from memory.**
Help that names a screen which does not exist is worse than no help: it costs the
reader a search before they conclude the documentation is unreliable. Dumping the
real menu tree first is also what surfaced the bug below.

The seeder now walks **both** files in two passes — all sections first, then all
articles — so an article can resolve its parent by slug regardless of which file
it lives in. A parent slug that resolves to nothing is logged as an error rather
than silently producing an unreachable article.

## 2. A bug the menu dump caught

`Settings → Projects` opened **Database Backups**, and `Settings → Task Board`
opened **Company Admin**.

I caused it in docs/100. My first ProjectModule seed claimed menu ids 131 and
132, which belong to ReportModule, and renamed them. `verify_menu_ids.sh` caught
the id collision and I moved my menus to 137–142 — but my cleanup deleted only
rows pointing at *my* actions, and these rows pointed at ReportModule's. They
kept my labels.

They could never heal, because ReportModule's upsert was
`ON CONFLICT DO UPDATE SET parent_id=…, sequence=…, action_id=…` — deliberately
not `name`. Any module that briefly squatted on those ids left its label there
permanently.

Fixed by making those two seeds restore their own names. **A seed that owns a
row should own its name**; leaving it out only preserves a foreign module's
mistake.

## 3. Two findings from `verify_help.sh`

- **`related` was empty for every single-article book.** The fallback chain went
  keywords → section siblings → same book, and a book with one article has none
  of those. Added a final fallback to other books. The test walks *every* article
  and fails if any has an empty panel, which is what caught it.
- **A stale assertion of my own.** "At least one book is empty" passed only while
  books were undocumented and failed the moment they were all filled in. It now
  asserts what actually matters: the tab bar is driven by the configured module
  list, and every configured slug has a tab.

## 4. Reset the database to a clean state

**Settings → Database Tools → Reset.**

### "Empty" cannot mean empty

With no companies, chart of accounts, units or menus the application does not
start. So a reset clears the **data people enter** and keeps the
**configuration the system needs**, in two scopes:

| Scope | Clears | Keeps |
|---|---|---|
| **Transactions** | Invoices, payments, orders, stock moves, manufacturing, tasks, timesheets, rental activity, lookup proposals | Products, contacts, projects, all configuration |
| **Transactions + master data** | The above, plus products, part parameters, BoMs, pricelist rules, rental units | Contacts and all configuration |

### The guards

- Administrator only.
- **A preview is mandatory.** The destructive button does not exist in the page
  until a dry run has been fetched and its per-table row counts shown.
- `confirm` must be exactly `RESET`. Typing `reset` leaves the button disabled —
  asserted by the render probe.
- Changing scope discards both the preview and the typed confirmation, so a
  confirmation always belongs to the preview the user actually read.
- It runs in its **own write transaction**. The SQL console's read-only
  transaction is its safety property and stays absolute.

### The table list computes its own closure

`TRUNCATE` refuses to clear a table while anything outside the statement
references it. Maintaining that list by hand is a losing game — every new module
adds a child table somebody forgets.

So the handler starts from a scope list and **expands it through the foreign-key
catalogue** until closed. It found `stock_landed_cost_line`,
`rental_contract_line`, `rental_invoice_link`, and for the master scope
`mrp_bom`, `mrp_bom_line`, `mrp_routing_workcenter` — none of which I had listed.

`TRUNCATE` is issued **without CASCADE** on purpose: if the closure is somehow
still incomplete the database refuses the whole statement rather than quietly
cascading into something meant to survive.

### It refuses when the scope is wrong

If the closure reaches a table in the protected set — companies, users, chart of
accounts, journals, units, menus, task stages, help articles — the reset is
**refused with the reason**, not widened. That check earned its keep twice:

- Clearing `res_partner` would require deleting `account_analytic_account`.
  Contacts are therefore not in any scope.
- Clearing `project_project` would require deleting `project_task_type`, and
  `TRUNCATE` cannot tell per-project stages from the shared defaults every board
  needs. Projects are therefore not in any scope; the transactions scope already
  empties them of tasks and timesheets.

Both are real constraints of the data model, surfaced rather than worked around.

### Verifying it without destroying anything

The table lists were proved complete by running the `TRUNCATE` inside a
transaction and rolling it back. That is how the first missing table was found —
before any button existed, and without deleting a row.

**I have not run a real reset on this database.** Building the tool is what was
asked; wiping the data in it was not, so that is yours to trigger.

## 5. On using reset instead of fixing the test leak

Reset makes the accumulated-invoice problem *survivable*, and is genuinely useful
between test runs. It does not make it *fixed*: something in
`verify_fx_settlement`, `verify_money_roundtrip`, `verify_multicompany_*` or
`verify_payment_allocation` still creates bare `account_move` rows and abandons
them. A suite that needs a database wipe to stay green is hiding a defect rather
than lacking a button.

## 6. Not done

- Help has no images or screenshots.
- Search is `ILIKE` with title/keyword/body ranking, not a full-text index.
- The reset cannot clear contacts or projects, for the reasons above. Delete
  those individually if you mean to.
- Sequences restart with `TRUNCATE`, so ids begin again at 1 after a reset.
  Anything holding an old id externally will not match.
