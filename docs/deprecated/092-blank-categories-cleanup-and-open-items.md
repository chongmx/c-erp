# 092 — The blank categories, the debris behind them, and the open items

Started from a UI report — *"the categories sidebar has many empty
cat-tree-items"* — which turned out to be the visible end of a leak that had
been running for weeks. Then closed the outstanding follow-ups from docs/090
and docs/091.

Suite: **64 passed, 0 failed**, and a full run now leaves **zero** rows behind.

---

## 1. The blank categories

29 of 109 `product.category` rows had an empty name. Their `create_date`s lined
up one-for-one with test-suite runs.

**Root cause.** `ProductCategoryViewModel::handleCreate` writes raw SQL and so
never reaches `ProductCategory::validate()` — which *does* require a name. So
`create({})` inserted a nameless row. The category's name is its entire
identity in the tree, so each one rendered as an unlabelled, indistinguishable
line in the sidebar.

**Fix, in three layers**, because each catches what the one before it cannot:

- the handler now rejects an empty or whitespace-only name (and trims what it
  accepts) — on `write` too, since renaming to blank is the same defect;
- a startup migration deletes blanks that nothing references and *names* any
  that do own products or children, rather than deleting data underneath them;
- a `CHECK (btrim(name) <> '')` constraint, so no other path — a raw INSERT, an
  import — can put one back.

`verify_new_forms.sh` now reports `product.category — clean validation error
('Category name is required.')` where it used to report a spurious success.

## 2. What was creating them

`verify_new_forms.sh` calls `create({})` on ~30 models to prove none of them
500s. It never deleted what those calls created. For most models create
correctly fails, so nothing accumulated — but `product.category` succeeded
(bug 1) and `stock.picking` succeeds by design, giving **one blank category and
one empty "New" transfer per run**.

Three other scripts were leaking too:

| Script | Left behind, per run |
|---|---|
| `verify_new_forms` | 1 blank category, 1 empty transfer |
| `verify_budgets` | 1 budget + 2 posted journal entries |
| `verify_assets` | 1 asset + its posted depreciation entries |
| `verify_product_inventory`, `verify_lots_packages`, `verify_expenses` | a QA product / tax (stable, but permanently in the user's lists) |

All six now clean up after themselves. The budget and asset cleanups delete the
**journal entries with the records** — posted depreciation left orphaned would
quietly skew the P&L that this suite's own report tests read — and both re-assert
that the ledger still balances afterwards.

Two full suite runs back to back now leave zero rows behind, asserted by
`cleanup_test_data.sh --dry-run` reporting all-zero.

## 3. Cleaning up what had accumulated

`scripts/cleanup_test_data.sh` — a maintenance tool, **not** part of the suite.
Dry-run by default; `--apply` to delete. It removed 29 empty transfers, 21
budgets, 17 assets, 204 orphaned depreciation lines, the QA fixtures, and their
journal entries.

Its rules are the interesting part, and they are in the file:

- only records identifiable as test output; master data is never touched;
- journal entries go **with their lines** — a posted move whose lines are gone
  is worse than the debris;
- one transaction, and it verifies the ledger still balances before it counts as
  success.

That last rule earned its keep on the first run: an FK ordering mistake
(`account_asset_depreciation_line.move_id` references `account_move`, so the
lines must go first — but they are also the only record of which moves to
delete) aborted the transaction and nothing was removed. Fixed with a temp table
holding the ids across both steps.

## 4. Open items from docs/090 and docs/091

### Standalone credit notes — the sign convention (docs/082 follow-up)

`recompute_totals` and `recomputeTaxLines_` assumed the customer-invoice shape
everywhere: product lines on credit, counterparty on debit. A credit note is the
mirror image, so on a hand-entered one:

- "sum the credits" over product lines returned **zero** — the lines are debits;
- `UPDATE … SET debit = total WHERE debit > 0` hit the **revenue** line rather
  than the receivable;
- the generated tax line was posted on the wrong side, so the entry could not
  balance.

Both now branch on move type:

| type | product lines | counterparty |
|---|---|---|
| `out_invoice` | credit | debit |
| `out_refund` | debit | credit |
| `in_invoice` | debit | credit |
| `in_refund` | credit | debit |

`verify_standalone_credit_note.sh` (27 checks) drives all four types and asserts,
for each, that the total equals the lines, the tax lands on the same side as the
lines it came from, the counterparty line is updated, and the document balances —
plus that recomputing repeatedly neither drifts the total nor duplicates tax
lines.

### `rental_expense.attachment_id`

Dropped (migration 814). It was added as a placeholder for receipt upload on the
belief that there was no `ir.attachment` — there was, and it is polymorphic. A
single-valued column would also have capped a receipt at one file. Receipts
attach through `(res_model, res_id)` like everything else.

### Attachments on the remaining documents

`AttachmentPanel` added to Invoice ("Supporting Documents"), Sales Order,
Purchase Order and Transfer ("Documents") — one tag each, as intended.

### Product form inline-create ＋

The generic form has had it since docs/078; the bespoke Product form did not, so
adding a missing category meant abandoning a half-filled product. Category and
Unit of Measure now carry the ＋, with the same modal, and the new record is
selected immediately. It surfaces the server's message on failure — which, for a
category, is now the "Category name is required." from §1.

### Log retention — and why it had never worked

`log/` held **1,807 files (29 MB)** with nothing pruning them. trantor has
`setFileSizeLimit` and `setMaxFiles`; neither was ever called.

Wiring them up was not enough. `setFileName(baseName, extName, path)` takes the
directory and base name **separately**, and we were passing `"log/system"` as the
base name with the default path `"./"`. Writing still worked — the full name is
just `path + base + ext` — but the rotation bookkeeping scans `path` for files
beginning with `base`, so it looked in `./` for names starting with `log/system`
and found nothing. Retention could never have worked at any limit.

Split correctly, with `log_size_limit_mb = 20` and `log_max_files = 30` in
`config/system.cfg`, the next start pruned the directory to **31 files (18 MB)**.

---

## Still open

- **Payment Acquirers** — parked by request.
- **Packages as a quant dimension** — deliberately not built (docs/090 §4).
- The suite's *fixture* data (sale orders, invoices, STJ valuation entries)
  still accumulates. Unlike the debris above it is real, balanced, referenced
  data, and several scripts read what previous runs produced. Making the suite
  hermetic per-run is a larger change than making it stop littering.
