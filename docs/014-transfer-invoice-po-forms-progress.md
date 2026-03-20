# Phase 17b + 17d — Transfer Form & Invoice/PO Custom Forms

## Summary

**Phase 17b** — `TransferFormView` (stock.picking) — Odoo 14-style form with full workflow buttons,
operations tab (editable Done quantities), and a chatter/log panel.

**Phase 17d** — `InvoiceFormView` (account.move) and `PurchaseOrderFormView` (purchase.order)
completed with Odoo 14-style breadcrumbs, stepper, two-column info grid, and editable line tables.

---

## Build Status

- Build: ✅ clean (`[100%] Built target c-erp`)

---

## Phase 17b — TransferFormView

### Frontend — `web/static/src/app.js`

New `TransferFormView` component added and registered in `ActionView` routing
(`stock.picking` → `TransferFormView`).

#### State machine

| State | Buttons shown |
|-------|---------------|
| `draft` | Confirm |
| `confirmed` | Check Availability, Cancel |
| `assigned` | Validate, Unreserve, Cancel |
| `done` | (none — read-only) |
| `cancel` | Reset to Draft |

#### Stepper
`Draft → Waiting → Ready → Done`

Each step highlighted with `.so-step-done` / `.so-step-active` / `.so-step-pending` classes.

#### Operations tab
- Table of `stock.move` lines: Product, From, To, Demand (read-only), Done (editable when
  state=assigned), UoM
- Input on Done column uses `data-move-key` + `data-move-field` attributes with event
  delegation on the parent `div.so-shell`
- Write-back: `stock.move` `write` called for each move before `button_validate`

#### Additional Info tab
- Shows Responsible, Company, Lot/Serial tracking, Package tracking
  (currently stubbed with "not implemented" notes)

### Backend — `modules/stock/StockModule.hpp`

All workflow methods registered in `StockPickingViewModel`:

| Method | Effect |
|--------|--------|
| `action_confirm` | `draft → confirmed`; assigns sequence name (`WH/IN/…` or `WH/OUT/…`) |
| `action_assign` | `confirmed → assigned` (availability check stub; sets state) |
| `button_validate` | `assigned → done`; copies `product_uom_qty → quantity` on moves; writes `qty_delivered` on `sale_order_line` or `qty_received` on `purchase_order_line`; sets `invoice_status/billing_status` to `to_invoice` |
| `action_cancel` | any → `cancel`; sets all moves to `cancel` |
| `button_unreserve` | `assigned → confirmed`; resets move quantities to 0 |
| `button_reset_to_draft` | `cancel → draft`; resets moves to `draft` |
| `default_get` | Returns sensible defaults for new picking creation |

---

## Phase 17d — InvoiceFormView & PurchaseOrderFormView

### InvoiceFormView (account.move)

- Breadcrumb navigation back to Accounting list
- Stepper: Draft → Posted → Cancelled
- Two-column info grid: Customer/Vendor, Invoice Date, Due Date, Payment Term, Source Document
- Editable lines table:
  - Normal lines: Description, Qty, Price Unit, Subtotal (live-computed)
  - Section lines (display_type='line_section'): bold full-width title row
  - Note lines (display_type='line_note'): italic full-width text row
  - Add a Line / Add a Section / Add a Note buttons
- Save syncs lines (create/write/unlink), then calls `recompute_totals` on server
- Unit price inference: for old invoices without `price_unit`, infers `credit / qty`
- Workflow buttons:
  - Draft: Confirm (→ Posted)
  - Posted: Cancel (→ Cancelled)
  - Cancelled: Reset to Draft (→ Draft)

### PurchaseOrderFormView (purchase.order)

- Mirrors SaleOrderFormView with vendor/date_planned/product_qty fields
- Stepper: RFQ → Purchase Order → Done
- Stat button: Receipts count (navigates to Inventory)
- Editable order lines with product auto-fill

---

## ActionView Routing

```js
'sale.order'    → SaleOrderFormView
'purchase.order'→ PurchaseOrderFormView
'account.move'  → InvoiceFormView
'stock.picking' → TransferFormView
*               → FormView (generic)
```

Component declaration order in app.js (required — avoids TDZ):
```
InvoiceFormView → SaleOrderFormView → PurchaseOrderFormView →
TransferFormView → ActionView
```

---

## What's Next

- Phase 18: CRM module (`crm.lead`, `crm.stage`)
- Phase 19: Project module (`project.project`, `project.task`)
- Phase 20: HR Leave (`hr.leave`, `hr.leave.type`)
- Phase 21: Payroll (`hr.payslip`)
- Phase 22: Analytic Accounts
- Phase 17e (deferred): PDF report generation via wkhtmltopdf
