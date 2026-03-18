# Phase 17a — One2many Order Lines (Sale & Purchase)

## Summary
Sale Orders and Purchase Orders now display and edit their order lines directly
in the form view. Users can add/remove product lines with quantity and price,
and the lines are persisted on save.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## Changed Files
| File | Change |
|------|--------|
| `modules/sale/SaleModule.hpp` | Registered `order_line` One2many field on `SaleOrder` |
| `modules/purchase/PurchaseModule.hpp` | Registered `order_line` One2many field on `PurchaseOrder` |
| `web/static/src/app.js` | Full `FormView` rewrite — O2m sublist editing with event delegation |
| `web/static/src/app.css` | Added `.o2m-section`, `.o2m-table`, `.o2m-input`, `.btn-sm` styles |

---

## Backend Changes

### Field registration (both modules)
```cpp
fieldRegistry_.add({"order_line", FieldType::One2many, "Order Lines",
    false, false, false, false, "sale.order.line", "order_id"});
// (purchase: "purchase.order.line", "order_id")
```

One2many fields are excluded from `storedColumnNames()` so they never appear
in `INSERT`/`UPDATE` on the parent record.

---

## Frontend Architecture

### FormView — scalar vs O2m split
- `scalarFields` — all fields whose type is not `one2many`/`many2many`
- `o2mFields` — only `one2many` fields with a non-empty `relation`
- `o2mColumns(fieldName)` — returns column metadata from `state.o2mMeta[fieldName]`

### loadO2mData()
For each O2m field:
1. `fields_get` on the child model → filter out system/skip columns
2. Load nested Many2one `relOptions` for child model fields
3. `search_read` for existing child records (filtered by parent id)
4. Stores results in `state.o2mLines[fieldName]` and `state.o2mMeta[fieldName]`

### syncO2mLines(parentId)
Called after the parent record is saved/created:
1. `unlink` any IDs in `state.deletedIds[fieldName]`
2. `create` lines that have no `id` yet
3. `write` lines that already have an `id`

### Event delegation pattern
All `t-on-change`, `t-on-input`, `t-on-click` are placed on `div.form-body`
(outside any `t-foreach` loop). Child elements carry data attributes:

| Attribute | Purpose |
|-----------|---------|
| `data-field` | scalar field name |
| `data-o2m` | O2m field name (on line cells) |
| `data-key` | line `_key` identifier |
| `data-add-o2m` | field name → triggers `addO2mLine()` |
| `data-del-o2m` | field name → `data-key` → triggers `removeO2mLine()` |

This pattern is required because OWL 2 (IIFE build) cannot resolve prototype
method names from within `t-foreach` loop scope.

---

## Behaviour

### Adding a line
- Click **+ Add Line** → `addO2mLine(fieldName)` pushes a blank object with a
  unique `_key` into `state.o2mLines[fieldName]`

### Editing a line
- `<select>` for Many2one columns (options loaded from `relOptions`)
- `<input type="number">` for Float/Integer columns
- `<input type="text">` for other types
- Changes flow through `onFormChange` → `updateO2mLine()`

### Removing a line
- Click **✕** → `removeO2mLine()` pops the line from state; if the line had a
  database `id`, that id is pushed to `state.deletedIds[fieldName]` for later
  `unlink`

### Saving
- Parent `save()` calls `syncO2mLines(parentId)` after the parent
  `create`/`write` resolves

---

## What's Next
- Phase 17b: Workflow buttons on Transfers form
  (Confirm, Check Availability, Validate, Cancel)
- Phase 18: CRM module (`crm.lead`, `crm.stage`)

---

## Phase 17c — Sale Order stat buttons (Delivery / Invoices / Preview)

### Implemented
| Button | Behaviour |
|--------|-----------|
| **Delivery** | Shows `stock.picking` count for this sale order. Clicking navigates back to list (Inventory app has full delivery view). |
| **Invoices** | Shows `account.move` (`out_invoice`) count linked via `invoice_origin = order.name`. Count refreshes after "Create Invoice". Clicking navigates back to list (Accounting app has full invoice view). |
| **Preview** | Disabled / greyed out — see deferred section below. |

### Backend changes
- `account_move.invoice_origin VARCHAR` added to `AccountModule` DDL + `ALTER TABLE … ADD COLUMN IF NOT EXISTS` migration.
- `action_create_invoices` in `SaleModule` now writes `invoice_origin = order.name` on each created `account.move`.

---

## Deferred — Customer Preview (PDF)

**Feature:** A "Customer Preview" stat button on the Sales Order form that renders
a PDF version of the quotation/order for the customer to review.

**Why deferred:** PDF generation requires a server-side rendering pipeline.
Options:
1. **wkhtmltopdf** (Odoo's approach) — render an HTML template server-side then
   convert to PDF via wkhtmltopdf binary.
2. **Headless Chromium / puppeteer** — more modern, better CSS support.
3. **Client-side jsPDF** — limited fidelity, no page breaks.

**Current state:** The button is visible in the UI but disabled (greyed out,
`pointer-events: none`). It shows a 📄 icon and "Preview" label to communicate
the planned feature to users.

**Implementation plan (future):**
1. Add a C++ `/report/sale_order/<id>` HTTP endpoint that:
   - Builds an HTML string from the order + lines data
   - Pipes it through `wkhtmltopdf --quiet - -` (stdin → stdout)
   - Returns `Content-Type: application/pdf`
2. Frontend "Preview" button opens `window.open('/report/sale_order/' + id, '_blank')`
3. Enable the button (remove `so-stat-btn-disabled`) once the endpoint exists.
