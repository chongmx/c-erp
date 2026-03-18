# Purchase Module Progress

## Summary
Implemented Phase 10 — Purchase module. Mirrors the Sale module structure: two models with a state machine (draft → purchase → cancel), automatic PO naming via DB sequence, bill generation into `account.move` (in_invoice), and line-level amount recomputation on create/write.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New Files

| File | Purpose |
|------|---------|
| `modules/purchase/PurchaseModule.hpp` | PurchaseOrder + PurchaseOrderLine models, views, viewmodels, module class |

---

## Phase 10 — Purchase

### Model: `purchase.order` → table `purchase_order`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | 'New' → 'PO/YYYY/NNNN' on confirm |
| state | Selection | draft / purchase / cancel |
| partner_id | Many2one | → res.partner (Vendor), required |
| date_order | Date | defaults to now() |
| date_planned | Date | Expected arrival date |
| payment_term_id | Many2one | → account_payment_term |
| note | Text | |
| currency_id | Many2one | → res.currency |
| company_id | Many2one | → res.company, DEFAULT 1 |
| user_id | Many2one | → res.users (Purchase Rep.) |
| origin | Char | Source document |
| invoice_status | Selection | nothing / to_bill / billed |
| amount_untaxed | Monetary | computed from lines |
| amount_tax | Monetary | computed from lines |
| amount_total | Monetary | computed from lines |

### Model: `purchase.order.line` → table `purchase_order_line`

| Field | Type | Notes |
|-------|------|-------|
| order_id | Many2one | → purchase.order ON DELETE CASCADE |
| sequence | Integer | DEFAULT 10 |
| product_id | Many2one | → product.product |
| name | Text | Description, required |
| product_qty | Float | Quantity |
| product_uom_id | Many2one | → uom.uom |
| price_unit | Monetary | Unit price |
| discount | Float | Discount % |
| tax_ids_json | Text | JSON array of account.tax IDs |
| price_subtotal | Monetary | qty * unit * (1 - disc/100) |
| price_tax | Monetary | computed from tax_ids_json |
| price_total | Monetary | subtotal + tax |
| date_planned | Date | Expected arrival per line |
| qty_invoiced | Float | |
| qty_received | Float | |

---

## State Machine

```
draft (RFQ) → [action_confirm()] → purchase (Purchase Order)
            ↓                              ↓
       [action_cancel]          [action_create_bills()]
        (→ cancel)                         ↓
                                   account.move (in_invoice)
```

### `action_confirm()`
- Validates `state == 'draft'`
- Gets `nextval('purchase_order_seq')` → generates `PO/YYYY/NNNN`
- Sets `state = 'purchase'`, updates `name`

### `action_cancel()`
- Sets `state = 'cancel'` for all draft orders in batch

### `action_create_bills()`
1. Finds PUR journal (`type='purchase'`) and expense account (`account_type IN ('expense','expense_direct_cost')`)
2. Finds AP account (`account_type='liability_payable'`)
3. Creates `account.move` with `move_type='in_invoice'`, state=`'draft'`
4. For each purchase_order_line: creates expense debit line + optional tax debit line
5. Creates one AP credit line for `amount_total`
6. Sets `invoice_status = 'billed'` on the order

---

## Amount Recomputation

`PurchaseOrderLineViewModel` overrides `create` and `write`:

```
price_subtotal = product_qty * price_unit * (1 - discount/100)
price_tax      = price_subtotal * sum(tax.amount/100 for percent taxes)
price_total    = price_subtotal + price_tax
```

After any line create/write, parent `purchase_order` totals are recalculated via SQL aggregation.

---

## Views

- `purchase.order.list` — name, vendor, dates, state, invoice_status, amounts
- `purchase.order.form` — all fields
- `purchase.order.line.list` — order, product, name, qty, price, discount, subtotal
- `purchase.order.line.form` — all fields including date_planned, qty_received

---

## IR

| Type | id | name | target |
|------|----|------|--------|
| ir_act_window | 12 | Purchase Orders | purchase.order |
| ir_ui_menu | 70 | Purchase (app tile) | parent=NULL |
| ir_ui_menu | 71 | Orders (section) | parent=70, action=NULL |
| ir_ui_menu | 72 | Purchase Orders (leaf) | parent=71, action_id=12 |

---

## CMakeLists.txt Changes
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/modules/purchase
```

## main.cpp Changes
```cpp
#include "modules/purchase/PurchaseModule.hpp"
// ...
g_container->addModule<odoo::modules::purchase::PurchaseModule>();
```

---

## Module & Table Inventory (cumulative after Phase 10)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu` |
| account | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |
| uom | `uom_uom` |
| product | `product_category`, `product_product` |
| sale | `sale_order`, `sale_order_line` |
| purchase | `purchase_order`, `purchase_order_line` |

**Total: 22 tables** (plus `sale_order_seq`, `purchase_order_seq` sequences)

---

## Next Steps
- Phase 11: Stock module (deferred — complex, 26 models in Odoo)
- Phase 12: HR module — independent of sale/purchase, can be implemented standalone
