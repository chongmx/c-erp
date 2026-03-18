# Sale Module Progress

## Summary
Implemented Phase 9 — Sales module. Two models with a state machine (draft → sale → cancel), automatic SO naming via a DB sequence, invoice generation into `account.move`, and line-level amount recomputation on create/write.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New Files

| File | Purpose |
|------|---------|
| `modules/sale/SaleModule.hpp` | SaleOrder + SaleOrderLine models, views, viewmodels, module class |

---

## Phase 9 — Sale

### Model: `sale.order` → table `sale_order`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | 'New' → 'SO/YYYY/NNNN' on confirm |
| state | Selection | draft / sale / cancel |
| partner_id | Many2one | → res.partner, required |
| partner_invoice_id | Many2one | → res.partner |
| partner_shipping_id | Many2one | → res.partner |
| date_order | Date | defaults to now() |
| validity_date | Date | |
| payment_term_id | Many2one | → account_payment_term |
| note | Text | |
| currency_id | Many2one | → res.currency |
| company_id | Many2one | → res.company, DEFAULT 1 |
| user_id | Many2one | → res.users (Salesperson) |
| client_order_ref | Char | Customer's PO ref |
| origin | Char | Source document |
| invoice_status | Selection | nothing / to_invoice / invoiced |
| amount_untaxed | Monetary | computed from lines |
| amount_tax | Monetary | computed from lines |
| amount_total | Monetary | computed from lines |

### Model: `sale.order.line` → table `sale_order_line`

| Field | Type | Notes |
|-------|------|-------|
| order_id | Many2one | → sale.order ON DELETE CASCADE |
| sequence | Integer | DEFAULT 10 |
| product_id | Many2one | → product.product |
| name | Text | Description, required |
| product_uom_qty | Float | Quantity |
| product_uom_id | Many2one | → uom.uom |
| price_unit | Monetary | Unit price |
| discount | Float | Discount % |
| tax_ids_json | Text | JSON array of account.tax IDs |
| price_subtotal | Monetary | qty * unit * (1 - disc/100) |
| price_tax | Monetary | computed from tax_ids_json |
| price_total | Monetary | subtotal + tax |
| qty_invoiced | Float | |
| qty_delivered | Float | |

---

## State Machine

```
draft (Quotation) → [action_confirm()] → sale (Sales Order)
                 ↓                              ↓
             [action_cancel]           [action_create_invoices()]
              (→ cancel)                        ↓
                                         account.move (out_invoice)
```

### `action_confirm()`
- Validates `state == 'draft'`
- Gets `nextval('sale_order_seq')` → generates `SO/YYYY/NNNN`
- Sets `state = 'sale'`, updates `name`

### `action_cancel()`
- Sets `state = 'cancel'` for all draft orders in batch

### `action_create_invoices()`
1. Finds SAL journal (`type='sale'`) and income account (`account_type='income'`)
2. Finds AR account (`account_type='asset_receivable'`)
3. Creates `account.move` with `move_type='out_invoice'`, state=`'draft'`
4. For each sale_order_line: creates income credit line + optional tax credit line
5. Creates one AR debit line for `amount_total`
6. Sets `invoice_status = 'invoiced'` on the order

---

## Amount Recomputation

`SaleOrderLineViewModel` overrides `create` and `write`:

```
price_subtotal = product_uom_qty * price_unit * (1 - discount/100)
price_tax      = price_subtotal * sum(tax.amount/100 for percent taxes)
price_total    = price_subtotal + price_tax
```

After any line create/write, the parent `sale_order` totals are recalculated:
```sql
UPDATE sale_order SET
    amount_untaxed = (SELECT SUM(price_subtotal) FROM sale_order_line WHERE order_id = $1),
    amount_tax     = (SELECT SUM(price_tax)      FROM sale_order_line WHERE order_id = $1),
    amount_total   = (SELECT SUM(price_total)    FROM sale_order_line WHERE order_id = $1)
WHERE id = $1
```

---

## Views

- `sale.order.list` — name, partner, date, state, invoice_status, amounts
- `sale.order.form` — all fields
- `sale.order.line.list` — order, product, name, qty, price, discount, subtotal
- `sale.order.line.form` — all fields

---

## IR

| Type | id | name | target |
|------|----|------|--------|
| ir_act_window | 11 | Sales Orders | sale.order |
| ir_ui_menu | 60 | Sales (app tile) | parent=NULL |
| ir_ui_menu | 61 | Orders (section) | parent=60, action=11 |
| ir_ui_menu | 62 | Sales Orders (leaf) | parent=61, action=11 |

---

## CMakeLists.txt Changes
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/modules/sale
```

## main.cpp Changes
```cpp
#include "modules/sale/SaleModule.hpp"
// ...
g_container->addModule<odoo::modules::sale::SaleModule>();
```

---

## Module & Table Inventory (cumulative after Phase 9)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu` |
| account | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |
| uom | `uom_uom` |
| product | `product_category`, `product_product` |
| sale | `sale_order`, `sale_order_line` |

**Total: 20 tables** (plus `sale_order_seq` sequence)

---

## Next Steps
- Phase 10: `purchase` module — `purchase_order` + `purchase_order_line`, `action_confirm`, `action_create_bills`
- Phase 12: `hr` module — employees, leaves, payslips
