# Phase 16 — Stock ↔ Sale/Purchase Auto-Picking

## Summary
When a Sale Order or Purchase Order is confirmed, a `stock.picking` and its
`stock.move` lines are now created automatically in the same DB transaction.
This closes the procure-to-pay loop without any manual warehouse step.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## Changed Files
| File | Change |
|------|--------|
| `modules/sale/SaleModule.hpp` | `handleActionConfirm` — creates delivery picking + moves |
| `modules/purchase/PurchaseModule.hpp` | `handleActionConfirm` — creates receipt picking + moves |

No new tables. No schema changes.

---

## Behaviour

### Sale Order → Delivery (WH/OUT)
Triggered inside `SaleOrderViewModel::handleActionConfirm`, same transaction as
the SO state update.

1. Query `stock_picking_type WHERE code='outgoing'` → get Deliveries type (id=2,
   src=Stock/4, dest=Customers/6).
2. Advance `stock_out_seq` → generate name `WH/OUT/YYYY/NNNN`.
3. `INSERT INTO stock_picking` with `state='confirmed'`, `sale_id=<so_id>`,
   `origin=<SO/YYYY/NNNN>`, `partner_id` and `company_id` from the order.
4. `INSERT INTO stock_move … SELECT` — one move per `sale_order_line`, joining
   `product_product` for the move description; `quantity=0`, `product_uom_qty`
   from the line.

### Purchase Order → Receipt (WH/IN)
Triggered inside `PurchaseOrderViewModel::handleActionConfirm`, same transaction.

1. Query `stock_picking_type WHERE code='incoming'` → get Receipts type (id=1,
   src=Vendors/5, dest=Stock/4).
2. Advance `stock_in_seq` → generate name `WH/IN/YYYY/NNNN`.
3. `INSERT INTO stock_picking` with `state='confirmed'`, `purchase_id=<po_id>`,
   `origin=<PO/YYYY/NNNN>`.
4. `INSERT INTO stock_move … SELECT` — one move per `purchase_order_line`.

### NULL safety
Optional FK columns (`partner_id`, `company_id`) use `NULLIF($n, 0)` so integer
0 becomes SQL NULL without extra application logic.

### Graceful degradation
Both blocks are guarded by `if (!ptRow.empty())` — if the stock module is absent
or picking types are missing, order confirmation still succeeds; only picking
creation is skipped.

---

## Flow Diagram
```
Confirm SO  →  sale_order.state = 'sale'
           →  stock_picking  (WH/OUT/…, outgoing, confirmed, sale_id=SO)
           →  stock_move × N  (one per SOL, state=confirmed)

Confirm PO  →  purchase_order.state = 'purchase'
           →  stock_picking  (WH/IN/…,  incoming, confirmed, purchase_id=PO)
           →  stock_move × N  (one per POL, state=confirmed)

Later:
  Inventory → Transfers → open picking
    → [Check Availability]  action_assign  → state=assigned
    → [Validate]            button_validate → state=done
         ↳ writes qty_delivered on SOL / qty_received on POL
         ↳ flips invoice_status / billing_status
```

---

## What's Next
- Phase 17: Full frontend form views (workflow buttons on picking — Confirm,
  Check Availability, Validate, Cancel — visible in the Transfers form).
- Phase 18: CRM module (`crm.lead`, `crm.stage`).
