# 079 — Sales order line sections/notes + Pro-Forma invoice

Two requested additions to the sales order, plus a money bug the work uncovered.

## 1. Line sections & notes (like the Invoice/the reference ERP)

The invoice form already supported `display_type` section/note rows; the sales order
did not. Added end to end:

- **`sale.order.line.display_type`** — `'' | 'line_section' | 'line_note'`. New model
  field + `VARCHAR NOT NULL DEFAULT ''` column (with `ADD COLUMN IF NOT EXISTS` so
  existing DBs self-heal); (de)serialised; `validate()` no longer requires a description
  for section/note rows.
- **Frontend** (`SaleOrderFormView`): the order-lines editor renders a bold full-width
  **section** row and an italic **note** row, and the footer offers
  **+ Add a line / + Add a section / + Add a note** (mirrors the invoice editor). Lines
  load/save `display_type`; section/note rows are persisted with zeroed product/qty/price.
- **Totals** are unaffected — section/note rows carry 0 subtotal/tax/total, so the order
  total sums only real product lines.
- **Report** (`SALE_ORDER_TEMPLATE`): line rows are now `<tr class="row-{{line_type}}">`,
  so sections render bold and notes italic (the `.row-line_section` / `.row-line_note`
  CSS already existed). An idempotent migration upgrades already-seeded templates that
  lack the class (skips customised ones). The report loop fetches
  `COALESCE(NULLIF(display_type,''),'product') AS line_type` and blanks qty/price for
  annotation rows.
- **Invoicing skips annotations** — `action_create_invoices` selects
  `... AND COALESCE(display_type,'') = ''`, so a section/note never becomes a bogus
  zero-value ledger line. (Delivery already skipped them via its inner product JOIN.)

## 2. Pro-Forma invoice

A pro-forma is a payable document that is **not** a posted accounting entry — for a
customer to pay in advance. Implemented as a report variant, not a real move:

- `renderDoc_(…, bool proforma)` sets the document title to **"Pro-Forma Invoice"** for
  `sale.order` when `?proforma=1`; both `/report/html` and `/report/pdf` read the query
  param. No `account.move` is created, so the ledger is untouched.
- The sales order form gains a **Pro-Forma Invoice** button (next to Print) →
  `/report/pdf/sale.order/<id>?proforma=1` in a new tab.

## 3. Bug found & fixed: invoices failed for totals ≳ $10

`action_create_invoices` read the order's BIGINT **micro-unit** money columns
(`amount_total`, line `price_subtotal`, …) as `double` and re-appended them. pqxx
serialises a large double like `200000000.0` as `"2e+08"`, which the BIGINT invoice
columns reject → `invalid input syntax for type bigint: "2e+08"`. So creating an invoice
from any order above roughly $10 threw an internal error. Fixed by reading/appending those
values as integer micro-units (`std::llround(... .as<double>())`). This was pre-existing
(a P2 money-migration miss), unrelated to sections — surfaced by testing the new flow.

## Verified

- API: sections/notes stored & read; totals exclude them; report renders real
  `row-line_section`/`row-line_note` rows; `?proforma=1` titles "Pro-Forma Invoice";
  invoice creates, **balances (debit=credit)**, has exactly one income line, and
  `amount_total = 200.00`.
- UI (headless Chrome, 0 JS errors): the New order shows **+ Add a line / section / note**
  and renders the section (bold) and note (italic) rows; a saved order shows the
  **Pro-Forma Invoice** button.
- Regression: `./scripts/run_tests.sh` → **47 passed, 0 failed**.
