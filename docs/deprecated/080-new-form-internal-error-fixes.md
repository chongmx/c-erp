# 080 — "New … → Internal Error" fixes + regression guard

Clicking **New** and then **Create** on most lists (Inventory → Operations →
Receipts/Deliveries/Internal Transfers, Operation Types, Landed Costs, and every
config model) returned a scary **"An internal error occurred"**. Three distinct root
causes, all fixed.

## Root cause 1 — validation surfaced as a 500

`BaseModel::create` threw a plain `std::runtime_error("Validation failed: …")` when a
required field was missing. The dispatcher's generic `catch (std::exception&)` turned
that into a **500 "the reference ERP Server Error"** (Internal Error), gated by devMode. So a missing
"Name" read as a server fault instead of guidance.

**Fix:** throw `odoo::infrastructure::ValidationError` — which the dispatcher already
returns as a **400** with the message passed through (like `AccessDeniedError`). Now
`create({})` returns e.g. *"name is required"*, *"Journal name is required"*, and the
form shows that instead of "Internal Error".

## Root cause 2 — a NOT NULL column with no default (e.g. `stock.landed.cost.date`)

Some creates passed model `validate()` but still hit a raw Postgres
`null value in column "date" … violates not-null constraint` → 500.

**Fix:** `BaseModel::create` now catches `pqxx::sql_error`, and for a not-null (or check)
violation rethrows a `ValidationError` (*"The field 'date' is required."*). The column
name is safe to show; the full SQL text (SEC-28) is never leaked.

## Root cause 3 — `TransferFormView` read a null id on New

Opening **New** on a transfer (stock.picking) called
`stock.picking.read([[recordId]])` with `recordId = null` → 500 "Internal Error" before
the form even rendered. It was also the *only* custom form without an `if (recordId)`
guard (all others already had one), and its reference-data `Promise.all`
(`partner`/`users`/`location` filtered by `active`) rejected hard when `active` wasn't a
filterable column — nuking the form (this affected existing transfers too).

**Fix:**
- Guard the read; New starts a `{state:'draft'}` record and loads the Operation Types.
- Add an **Operation Type** selector (New mode) — it supplies the required source/dest
  locations — plus a **Create** button and an `onCreate` that creates the draft.
- Make the reference-data lookups resilient (`pr.catch(() => [])`) and drop the fragile
  `active` filter, so one failing lookup can't break the whole form.

## Regression guard

`scripts/verify_new_forms.sh` (now in the suite) asserts the contract:
**`create()` with an empty/partial body must never return a 500 "Internal Error"** — only
a real id or a 400 `ValidationError` whose message tells the user what's missing. It
covers 25+ config models, the sale/purchase/invoice documents, and stock.picking (both a
valid create and the bare validation case).

## Result

`./scripts/run_tests.sh` → **48 passed, 0 failed**. Browser-verified: Receipts → New
renders cleanly (Operation Type dropdown, Create), and Operation Types → New → Create
shows *"…is required"* — no "Internal Error" anywhere.
