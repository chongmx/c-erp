# 090 — Clearing the outstanding list

Five slices, in the order they were recommended: template reset, the stale
"coming soon" buttons, employee expenses, lots and packages, and the id
allocation guard. The last one immediately found two live bugs nobody had
noticed, which is the best argument for it.

Suite: **61 passed, 0 failed** (was 56/0; five new scripts, one flake repaired).

---

## 1. Document templates: a shipped baseline

**The problem.** `ir_report_template.template_html` was seeded once
(`ON CONFLICT DO NOTHING`) and then owned by the database. Improving a template
in code therefore never reached an existing install — the sale-order
section/note fix needed a hand-written `regexp_replace` migration (docs/079),
and every future template change would need another one.

**The fix.** A second column, `default_html`, holds the template as the source
tree ships it and is refreshed on every start. The pair supports three things a
single column cannot:

| | what happens |
|---|---|
| **backfill** | a row predating the column gets a baseline; `template_html` is left alone, because we cannot know whether it was edited |
| **upgrade** | a template nobody has edited (`template_html = default_html`) follows the source tree automatically — no migration |
| **reset** | a customised template can be diffed against, and restored to, the shipped one |

`is_customized` (`template_html IS DISTINCT FROM default_html`) rides along on
`read` and `search_read`.

**UI.** Settings ▸ Technical ▸ Document Templates gains a **Customized/Default**
badge, a **Compare** panel (line diff, LCS, with unchanged runs collapsed), and
**Reset to default** behind a confirmation. Reset also drops the saved block
layout for that model — the editor regenerates `template_html` from those blocks
on its next Save and would otherwise put the customisation straight back.

The `regexp_replace` migration stays: it repairs *customised* sale-order
templates, which no baseline can do. It is the last of its kind — untouched
templates never reach it now.

`scripts/verify_template_reset.sh` asserts both directions of the rule, and does
the destructive half on a throwaway row so a failure cannot leave the real
Invoice template broken.

## 2. The stale "coming soon" buttons

The Product form disabled six widgets citing models that had since been built.
`stock.quant`, `stock.warehouse.orderpoint` and `stock.putaway.rule` all existed
— empty, but real. So:

- **On Hand / Forecasted** now read the ledger via a new
  `stock.quant.product_summary`, which returns on-hand, reserved, pending
  incoming/outgoing, `virtual_available`, and the product's rule counts in one
  call. Forecast = on hand + incoming − outgoing, computed, never stored.
- **Update Quantity** opens a per-location count sheet and writes through
  `set_on_hand`, which books the difference against Inventory Adjustments. Only
  changed rows are sent — an unchanged line would book a zero move.
- **Replenish** runs the reordering scheduler scoped to that product
  (`run_scheduler({product_id})`, new).
- **Reordering Rules / Putaway Rules / On Hand** became real navigation.
- The Sales Order **Preview** button opens `/report/html/sale.order/<id>`.

One behavioural fix fell out of writing the test: the scheduler skipped a rule
with no vendor *silently*, so Replenish would report "nothing to replenish" when
the truth was "nobody to buy from". It now returns `skipped_no_vendor` and the
button says so.

An adjustment leaves a **stock_valuation_layer**, not a `stock_move` — in this
schema a move belongs to a picking and an adjustment has none. The test asserts
the layer, and asserts the *difference* is booked, not the new total.

## 3. Employee expenses — `hr.expense` / `hr.expense.sheet`

Genuinely missing; `rental.expense` is a property/vendor cost, a different
thing. Two models, because the approval and the journal entry belong to the
report, not to each receipt.

    Draft → Submitted → Approved → Posted → Paid

- **Posting** debits each line's expense account and credits either the employee
  payable (`own_account`) or the journal's cash account (`company_account`).
- **Register Reimbursement** clears the payable against bank/cash; it is refused
  on a company-paid report, where there is nothing to reimburse.
- Every transition is guarded server-side, so a stale browser tab cannot approve
  twice or post something nobody approved.
- Totals are derived on every write (`quantity × unit price`, tax on top) — the
  client's number is never trusted, because it is what an approval rests on.

**Malaysian tax treatment.** SST is a single-stage sales/service tax, not a VAT:
a business cannot reclaim SST paid on an expense. `tax_amount` is recorded for
the record and the vendor's paper trail, but the **full tax-inclusive amount is
debited to the expense account**. Booking it to a recoverable input-tax account —
the GST habit — would overstate assets and understate cost. The test asserts
there is no third line on the entry.

Menus: **Employees ▸ Employee Expenses** (the claimant's view) and
**Accounting ▸ Expense Reports** (the accountant's), distinct actions.

## 4. Lots/serials and packages

**Lots were already finished** in the engine — enforced at validation, keyed
into `stock_quant`, traceable. The only thing missing was a UI, which is why the
transfer form said "not implemented" about working code. The Operations table
now shows a Lot/Serial column for tracked lines, with an inline ＋ to create one,
written straight through so what Validate checks is what is on screen. Two
defects surfaced:

- `lot_id` was never registered on `StockMove`, so `BaseModel::write` skipped it
  **silently and returned true** — the lot looked saved and was not;
- the "a lot is required" error was a plain `runtime_error`, which SEC-28 masks
  to "An internal error occurred" in production. The whole point of that check is
  that the message reaches the operator, so it is a `ValidationError` now, and
  names the product.

**Packages are new, and deliberately narrow.** A package here is a *logistics
label*, not a third dimension of the quant ledger. Put in Pack groups a
transfer's operations under a sequenced parcel (`PACK00001`), the parcel follows
the goods to their destination on validation, and it cannot be unpacked once
shipped. Quants stay keyed by `(product, location, lot)` — adding package to that
key would touch costing, reservation and valuation, which is a far larger change
than the packing slip it would buy. New menu: **Inventory ▸ Products ▸ Packages**.

One bug worth recording: joining `stock_quant_package` into the `stock.move`
search broke *every* `search_read` on that model with "column reference
picking_id is ambiguous", because the package table also has a `picking_id` and
the domain compiler emits an unqualified column. It is a scalar subquery now.

## 5. The id-allocation guard — and what it found

Every previous menu-collision fix was a repair after the fact. The standing rule
was "grep before you pick an id", and it kept getting skipped, eight times.

`scripts/verify_menu_ids.sh` is a **static** check over the source: it extracts
every hardcoded `ir_ui_menu` / `ir_act_window` id from every module, fails on any
id claimed by two modules or any app root with more than one owner, and prints
the next free id in each space — the number the next person actually needs.

It needs no server, so it fails at authoring time rather than after a menu
breaks. Two canary assertions guard the parser itself: a check that silently
matches nothing would make every other check pass.

**It immediately found two live collisions:**

| id | winner | loser | effect |
|---|---|---|---|
| `ir_act_window` 35 | Mrp "Manufacturing Orders" | Portal "Portal Users" | the action was clobbered |
| `ir_ui_menu` 120 | Mrp "Master Production Schedule" | Portal "Portal Users" | **Settings ▸ Portal Users had vanished** |

Portal moved to action **100** / menu **73**, both `DO UPDATE` so existing
databases repair themselves on the next start. `verify_new_forms.sh` now pins
Portal Users, Master Production Schedule, Manufacturing Orders, Packages and both
expense menus by name.

Two parser bugs found while testing the checker against a deliberate collision —
worth recording because both made it report success:

- `sort -n -u` judges uniqueness on the numeric key alone, so
  `10 IrModule.cpp` and `10 UomModule.cpp` collapsed into one row and the
  collision disappeared. `sort -u`.
- the awk `mode` leaked across statements, so an `UPDATE … WHERE id IN (101,103)`
  was counted as an action id and the "next free" hint drifted.

## Also: one flaky test repaired, in three passes

`verify_ir_cron` greps the server log, and took three attempts to make honest —
each fix exposing the next layer:

1. Drogon **rotates** the log: on start `system.log` becomes
   `system.<timestamp>.log` and is recreated on the first write. Grepping a fixed
   path failed with "no line in the log" whenever the script ran in the seconds
   after a restart. → search the recent files, wait for the current one.
2. Searching several files then counted warnings **from previous runs**, so
   "warned once" became "warned 3 times" on the third run of the suite. → assert
   the delta across the probe, not the absolute count.
3. The scheduler warns once **per code for the life of the process** — which is
   the anti-spam property under test — so a fixed probe code produced *no*
   warning on a second run against the same server, failing for the opposite
   reason. → a unique probe code per run.

None of the three failures had anything to do with the scheduler. Verified by
running the suite twice back to back: green both times, no probe rows left
behind.

## And a smoke test for the new screens

`scripts/verify_new_views_smoke.sh` drives the calls each new screen makes on
mount — `get_views`, the opening `search_read`, the dropdown lookups. A model can
behave perfectly over RPC and still show "Internal Error" because `get_views` has
no arch to return, which is the class of bug docs/077 chased; it is cheap enough
to guard directly.

---

## Outstanding after this round

- **Payment Acquirers** — parked by request.
- **Packages as a quant dimension** — deliberately not built; see §4.
- ~~**`ir.attachment`** — still no attachment model, so expenses cannot carry a
  scanned receipt.~~ **Wrong — see docs/091.** `ir.attachment` was fully built:
  table, model, both HTTP routes, a content-addressed filestore with dedup, and
  18 passing checks in `verify_ir_primitives.sh`. What was missing was any UI to
  reach it, and cleanup on delete. Both are done now.
- **Product form inline-create ＋** (docs/078) — the generic form has it; the
  bespoke Product form still does not.
- **Standalone credit notes** (docs/082) — a hand-entered `out_refund` still uses
  the invoice-sign convention in `recompute_totals`; the reversal path is the
  tested one.
- **Log housekeeping** — the `log/` directory holds ~1,750 rotated files. Nothing
  prunes them.
