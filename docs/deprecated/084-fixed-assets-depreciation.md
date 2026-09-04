# 084 — Fixed Assets & Depreciation

The fourth Accounting slice: a fixed-asset register with a straight-line
depreciation board and the journal entries that post it — the reference ERP's Assets, Asset
Types and "Generate Assets Entries" in one workflow.

## Models

- **`account.asset.type`** — a template: number of depreciations, months per
  period, and the three accounts a depreciation entry touches (asset, accumulated
  depreciation, expense) + the journal. Config list under **Configuration → Asset
  Types** (generic form).
- **`account.asset`** — the depreciable asset: name, gross `value`, book value
  `value_residual`, acquisition date, schedule shape, accounts, `state`
  (draft → open → close). List under **Accounting → Assets**.
- **`account.asset.depreciation.line`** — one row of the schedule (date, amount,
  cumulative, book value, the posted `move_id`).

## Workflow (`AccountAssetViewModel`)

- **`action_confirm`** — builds the straight-line schedule: `number` lines of
  `value / number` (the last absorbs rounding), dated `acquisition_date +
  sequence × period_months`, and inherits any missing accounts from the asset type.
  State → `open`.
- **`action_depreciate`** (= *Generate Assets Entries*, optionally to a `date`) —
  for every not-yet-posted line due on/before the date, posts a balanced journal
  entry **Dr depreciation expense / Cr accumulated depreciation**, links it to the
  line, and refreshes the book value; the asset auto-closes once fully depreciated.
  Idempotent — re-running to the same date posts nothing new.
- **`action_close`** — retire early.

## UI

`AssetFormView` (routed for `account.asset`): the asset's fields, a **Book Value**
smart figure, a Draft → Running → Closed status bar with **Confirm — build
schedule** and **Post depreciation**, and a **Depreciation Board** table (#, date,
depreciation, cumulative, book value, Posted/Draft badge).

## Verified

`scripts/verify_assets.sh` (in the suite) drives the whole flow on a RM12,000 / 12
asset: confirm builds 12 lines summing to the gross value; depreciating to a cutoff
posts one **balanced** entry per due line (Dr expense == Cr accumulated); the book
value drops by exactly the depreciation posted; the **ledger stays balanced**; and a
re-run posts no duplicates. Browser-verified (0 JS errors): the depreciation board
shows 5 Posted / 7 Draft with book value falling 12,000 → 7,000.

Full suite: **52 passed, 0 failed**.

## Next

Build order continues: **Budgets** (Budgets, Budgets Analysis, Budgetary
Positions), then the configuration round-out.
