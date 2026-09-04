# UOM & Product Module Progress

## Summary
Implemented Phase 7 (Units of Measure) and Phase 8 (Product Catalog). These two modules are the critical unlocking steps — `sale.order.line` and `purchase.order.line` both require `product_id` and `uom_id`. Also introduced `GenericViewModel.hpp`, a shared CRTP ViewModel template that eliminates per-module duplication.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New Files

| File | Purpose |
|------|---------|
| `modules/uom/UomModule.hpp` | UOM model, views, module class |
| `modules/product/ProductModule.hpp` | ProductCategory + ProductProduct models, views, module class |
| `modules/base/GenericViewModel.hpp` | Shared CRTP ViewModel template (replaces per-module duplication) |

---

## Phase 7 — UOM

### Model: `uom.uom` → table `uom_uom`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| category | Char | Unit / Weight / Volume / Time / Length / Area |
| uom_type | Char | reference / bigger / smaller |
| factor | Float | ratio vs. reference unit |
| rounding | Float | display rounding precision |
| active | Boolean | default TRUE |

### Seeds (15 rows)

| id | name | category | uom_type | factor |
|----|------|----------|----------|--------|
| 1 | Units | Unit | reference | 1.0 |
| 2 | Dozens | Unit | bigger | 12.0 |
| 3 | kg | Weight | reference | 1.0 |
| 4 | g | Weight | smaller | 0.001 |
| 5 | Ton | Weight | bigger | 1000.0 |
| 6 | L | Volume | reference | 1.0 |
| 7 | ml | Volume | smaller | 0.001 |
| 8 | m3 | Volume | bigger | 1000.0 |
| 9 | Hours | Time | reference | 1.0 |
| 10 | Days | Time | bigger | 8.0 |
| 11 | Minutes | Time | smaller | 0.01667 |
| 12 | m | Length | reference | 1.0 |
| 13 | cm | Length | smaller | 0.01 |
| 14 | km | Length | bigger | 1000.0 |
| 15 | m2 | Area | reference | 1.0 |

### Views
- `uom.uom.list` — name, category, uom_type, factor, active
- `uom.uom.form` — all fields including rounding

### IR
- `ir_act_window` id=8: Units of Measure → `uom.uom`
- `ir_ui_menu` id=50: Products app tile (parent=NULL)
- `ir_ui_menu` id=52: Configuration section (parent=50)
- `ir_ui_menu` id=53: Units of Measure (parent=52, action=8)

---

## Phase 8 — Product

### Model: `product.category` → table `product_category`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| parent_id | Many2one | → product.category (hierarchical) |
| active | Boolean | |

**Seeds (3 rows):** All (id=1), Goods (id=2, parent=1), Services (id=3, parent=1)

### Model: `product.product` → table `product_product`

Single-model design — no `product.template` / `product.product` variant split. All products are single-variant (can be split later when multi-variant is needed).

| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| default_code | Char | Internal Reference / SKU |
| barcode | Char | |
| description | Text | |
| type | Char | consu / service |
| categ_id | Many2one | → product.category |
| uom_id | Many2one | → uom.uom (NOT NULL DEFAULT 1) |
| uom_po_id | Many2one | → uom.uom (NOT NULL DEFAULT 1) |
| list_price | Monetary | Sales Price |
| standard_price | Monetary | Cost |
| volume | Float | |
| weight | Float | |
| sale_ok | Boolean | |
| purchase_ok | Boolean | |
| company_id | Many2one | → res.company |
| active | Boolean | |

### Views
- `product.category.list` — name, parent_id, active
- `product.category.form` — all fields
- `product.product.list` — default_code, name, type, categ_id, list_price, standard_price
- `product.product.form` — all fields

### IR
- `ir_act_window` id=9: Products → `product.product`
- `ir_act_window` id=10: Product Categories → `product.category`
- `ir_ui_menu` id=51: Products (parent=50, action=9) — direct link
- `ir_ui_menu` id=54: Categories (parent=52, action=10) — under Configuration

---

## GenericViewModel.hpp

Extracted from `AccountModule.hpp`'s internal `AccountViewModel<T>` template. Now lives in `modules/base/GenericViewModel.hpp` (on the include path).

```cpp
template<typename TModel>
class GenericViewModel : public BaseViewModel {
    // search_read, web_search_read, read, web_read,
    // create, write, unlink, fields_get, search_count, search
};
```

Used by: `UomModule`, `ProductModule`. `AccountModule` still uses its own internal template (no change needed).

---

## CMakeLists.txt Changes
Added to `target_include_directories`:
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/modules/uom
${CMAKE_CURRENT_SOURCE_DIR}/modules/product
```

---

## main.cpp Changes
```cpp
#include "modules/uom/UomModule.hpp"
#include "modules/product/ProductModule.hpp"
// ...
g_container->addModule<odoo::modules::uom::UomModule>();
g_container->addModule<odoo::modules::product::ProductModule>();
```

Registered after `AccountModule` (uom has no account dep; but keeping account before product since future sale/purchase will depend on both).

---

## Bug Fixed: Duplicate registerFields

`BaseModel<T>` constructor already calls `TDerived::registerFields()` via CRTP. New model constructors were redundantly calling it a second time, causing:
```
FieldRegistry: duplicate field 'name'
```
**Fix:** Keep derived constructor bodies empty: `explicit Foo(db) : BaseModel(std::move(db)) {}`

---

## Module & Table Inventory (cumulative after Phase 8)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu` |
| account | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |
| uom | `uom_uom` |
| product | `product_category`, `product_product` |

**Total: 18 tables**

---

## Next Steps
- Phase 9: `sale` module — `sale_order` + `sale_order_line`, `action_confirm`, `action_create_invoices`
- Phase 10: `purchase` module — `purchase_order` + `purchase_order_line`, `action_confirm`, `action_create_bills`
- Phase 12: `hr` module — independent of sale/purchase, can be done in parallel
