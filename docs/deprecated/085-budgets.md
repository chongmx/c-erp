# 085 — Budgets (planned vs actual)

The fifth Accounting slice: budgetary positions, budgets over a period, and the
planned-vs-actual comparison — the reference ERP's Budgets / Budgetary Positions / Budgets
Analysis, driven off the same posted ledger as the financial statements.

## Models

- **`account.budget.post`** (Budgetary Position) — a named set of GL accounts the
  budget measures (`account_ids_json`). Config list under **Configuration →
  Budgetary Positions**.
- **`account.budget`** — name, `date_from` / `date_to`, `state`
  (draft → confirm → done). List under **Accounting → Budgets**.
- **`account.budget.line`** — one position, its `planned_amount`, and the
  `practical_amount` (actual) read back from the ledger.

## Actuals (`action_compute`)

The actual is the **real ledger figure**, not a stored guess: for each line it sums
the posted `account_move_line` rows whose account is in the position and whose date
falls inside the budget period, using each account's natural sign
(`credit − debit` for income, `debit − credit` otherwise) so an expense budget
compares against spend as a positive number. Recomputed on Save and on demand via
**Refresh actuals**; `action_confirm` / `action_done` / `action_draft` move the state.

## UI

`BudgetFormView` (routed for `account.budget`) matches the Assets/Sales forms:
**Planned** and **Actual** stat tiles, a Draft → Confirmed → Done status bar with
**Refresh actuals**, and a **Budget Lines** board — position picker, editable
planned amount, actual, remaining, and an **achievement meter** (green bar + %,
turning accent-red past 100%).

## Verified

`scripts/verify_budgets.sh` (in the suite) creates a position over an expense
account and a FY2026 budget, posts RM1,200 of expense **inside** the period and
RM999 **outside** it, then asserts the actual equals the ledger for the period, that
the in-period entry is included and the out-of-period one is **excluded**, plus the
state workflow and both menus. Repeatable — it clears its own prior entries.
Browser-verified (0 JS errors): planned 50,000 vs actual 12,835, remaining 37,165,
meter at 26%.

Full suite: **53 passed, 0 failed**.

> Two gotchas worth remembering. **Menu id 31 was already Settings ▸ Users** — the
> `ON CONFLICT DO NOTHING` seed correctly refused to clobber it, so Budgets uses 33;
> always check `ir_ui_menu` for a free id. And in test scripts, `psql -tAc
> "INSERT … RETURNING id"` prints the id *and* the `INSERT 0 1` tag — take
> `| head -1` or the captured id is garbage (this silently broke the first run).

## Next

Build order continues: the **configuration round-out** (Fiscal Positions, Account
Types, Currencies, Incoterms, Journal Groups), then analysis & audit reports.
