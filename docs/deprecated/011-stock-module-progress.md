# Stock Module Progress

## Summary
Phase 15 — Stock MVP. 4 models / 4 tables covering locations, operation types, transfers (pickings), and individual product moves. Depends on product, sale, purchase. `button_validate` closes the procure-to-pay loop by writing back `qty_delivered` to sale lines and `qty_received` to purchase lines.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New File
| File | Purpose |
|------|---------|
| `modules/stock/StockModule.hpp` | All 4 models, viewmodels, and module class |

---

## Models

### `stock.location` → `stock_location`
| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| complete_name | Char | full path e.g. "WH/Stock" |
| location_id | Many2one | → stock.location (parent) |
| usage | Char | view / internal / supplier / customer / inventory / transit |
| company_id | Many2one | → res.company |
| active | Boolean | |

### `stock.picking.type` → `stock_picking_type`
| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| code | Char | incoming / outgoing / internal |
| sequence_prefix | Char | WH/IN/, WH/OUT/, WH/INT/ |
| default_location_src_id | Many2one | → stock.location |
| default_location_dest_id | Many2one | → stock.location |
| company_id | Many2one | → res.company |
| active | Boolean | |

### `stock.picking` → `stock_picking`
| Field | Type | Notes |
|-------|------|-------|
| name | Char | sequence reference (New → WH/IN/2026/0001) |
| picking_type_id | Many2one | → stock.picking.type, required |
| state | Char | draft / confirmed / assigned / done / cancel |
| partner_id | Many2one | → res.partner |
| location_id | Many2one | source, required |
| location_dest_id | Many2one | destination, required |
| scheduled_date | Char | timestamp |
| origin | Char | SO/PO reference |
| company_id | Many2one | → res.company |
| sale_id | Many2one | → sale.order |
| purchase_id | Many2one | → purchase.order |

### `stock.move` → `stock_move`
| Field | Type | Notes |
|-------|------|-------|
| picking_id | Many2one | → stock.picking, CASCADE delete |
| product_id | Many2one | → product.product, required |
| product_uom_id | Many2one | → uom.uom |
| name | Text | description, required |
| product_uom_qty | Float | demand quantity |
| quantity | Float | done quantity |
| state | Char | draft / confirmed / assigned / done / cancel |
| location_id | Many2one | → stock.location |
| location_dest_id | Many2one | → stock.location |
| company_id | Many2one | → res.company |
| origin | Char | |

---

## ViewModels

### `StockPickingViewModel`
Full CRUD + workflow buttons:

| Method | Behaviour |
|--------|-----------|
| `action_confirm` | draft → confirmed; assigns sequence name (WH/IN/YYYY/NNNN, WH/OUT/..., WH/INT/...); confirms child moves |
| `action_assign` | confirmed → assigned (availability stub — marks ready immediately) |
| `button_validate` | Sets all moves quantity = product_uom_qty if 0; sets moves + picking to done; writes qty_delivered on sale_order_line (outgoing) or qty_received on purchase_order_line (incoming); re-evaluates invoice_status / billing_status on parent order |
| `action_cancel` | Sets picking + non-done moves to cancel |

### Others
- `GenericViewModel<StockMove>` — full CRUD on stock.move
- `GenericViewModel<StockLocation>` — full CRUD on stock.location
- `GenericViewModel<StockPickingType>` — full CRUD on stock.picking.type

---

## Sequences
| Sequence | Used for |
|----------|---------|
| `stock_in_seq` | Receipts → WH/IN/YYYY/NNNN |
| `stock_out_seq` | Deliveries → WH/OUT/YYYY/NNNN |
| `stock_int_seq` | Internal transfers → WH/INT/YYYY/NNNN |

---

## Seeds

### stock_location
| id | name | complete_name | usage | parent |
|----|------|--------------|-------|--------|
| 1 | Virtual Locations | Virtual Locations | view | NULL |
| 2 | Physical Locations | Physical Locations | view | NULL |
| 3 | WH | WH | view | 2 |
| 4 | Stock | WH/Stock | internal | 3 |
| 5 | Vendors | Partners/Vendors | supplier | 1 |
| 6 | Customers | Partners/Customers | customer | 1 |
| 7 | Inventory Adjustments | Virtual Locations/Inventory | inventory | 1 |

### stock_picking_type
| id | name | code | src | dest |
|----|------|------|-----|------|
| 1 | Receipts | incoming | Vendors (5) | Stock (4) |
| 2 | Deliveries | outgoing | Stock (4) | Customers (6) |
| 3 | Internal | internal | Stock (4) | Stock (4) |

---

## IR

| Type | id | name | target |
|------|----|------|--------|
| ir_act_window | 17 | Transfers | stock.picking |
| ir_act_window | 18 | Locations | stock.location |
| ir_act_window | 19 | Operation Types | stock.picking.type |
| ir_ui_menu | 90 | Inventory (app tile) | parent=NULL |
| ir_ui_menu | 91 | Transfers | parent=90, action=17 |
| ir_ui_menu | 92 | Configuration | parent=90, no action |
| ir_ui_menu | 93 | Locations | parent=92, action=18 |
| ir_ui_menu | 94 | Operation Types | parent=92, action=19 |

---

## CMakeLists.txt / main.cpp Changes
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/modules/stock
```
```cpp
#include "modules/stock/StockModule.hpp"
g_container->addModule<odoo::modules::stock::StockModule>();
```

---

## Module & Table Inventory (cumulative after Phase 15)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu`, `ir_config_parameter` |
| account | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |
| uom | `uom_uom` |
| product | `product_category`, `product_product` |
| sale | `sale_order`, `sale_order_line` |
| purchase | `purchase_order`, `purchase_order_line` |
| hr | `resource_calendar`, `hr_department`, `hr_job`, `hr_employee` |
| stock | `stock_location`, `stock_picking_type`, `stock_picking`, `stock_move` |

**Total: 31 tables** (+3 sequences: stock_in_seq, stock_out_seq, stock_int_seq)

---

## Next Steps
- Phase 16: Wire stock ↔ sale/purchase (auto-create picking on SO/PO confirm)
- Phase 17: Frontend form views
