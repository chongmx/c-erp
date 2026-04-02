# 034 — Computed Fields & Onchange Handlers

**Date:** 2026-04-01
**Status:** Implemented

---

## Overview

This document covers the implementation of two P0 features:

1. **Computed Fields** — field metadata flag (`compute`, `depends`) that prevents client writes and advertises server-side derivation via `fields_get`.
2. **Onchange Handlers** — server-side `onchange` RPC that returns derived field values when a trigger field changes, replacing client-side product-defaults logic.

---

## 1. Computed Fields

### Backend: `FieldDef` metadata (`modules/base/FieldRegistry.hpp`)

Two new fields added to `FieldDef`:

```cpp
bool        compute = false;  ///< true = server-derived; client must not edit
std::string depends;          ///< comma-separated field names this depends on
```

`toJson()` updated to:
- Force `readonly = true` when `compute = true` (`effectiveReadonly = readonly || compute`)
- Emit `"compute": true` in the JSON when set
- Emit `"depends": "field1,field2"` when `depends` is non-empty

The frontend reads `readonly` from `fields_get` and disables the input. Computed fields therefore become non-editable automatically without any per-form changes.

### Sale module (`modules/sale/SaleModule.cpp`)

Fields marked as computed using C++20 designated initializers:

**`sale.order`:**
| Field | depends |
|---|---|
| `invoice_status` | `order_line` |
| `amount_untaxed` | `order_line` |
| `amount_tax` | `order_line` |
| `amount_total` | `order_line` |

**`sale.order.line`:**
| Field | depends |
|---|---|
| `price_subtotal` | `product_uom_qty,price_unit,discount` |
| `price_tax` | `product_uom_qty,price_unit,discount` |
| `price_total` | `product_uom_qty,price_unit,discount` |
| `qty_invoiced` | _(none — cross-model)_ |
| `qty_delivered` | _(none — cross-model)_ |

### Purchase module (`modules/purchase/PurchaseModule.cpp`)

Same pattern:

**`purchase.order`:** `invoice_status`, `amount_untaxed`, `amount_tax`, `amount_total`
**`purchase.order.line`:** `price_subtotal`, `price_tax`, `price_total`, `qty_invoiced`, `qty_received`

---

## 2. Onchange Handlers

### Protocol

The `onchange` method follows the Odoo JSON-RPC calling convention:

**Request:**
```json
{
  "method": "call",
  "params": {
    "model": "sale.order.line",
    "method": "onchange",
    "args": [ <current_line_values>, "<changed_field>", {} ],
    "kwargs": {}
  }
}
```

**Response:**
```json
{
  "result": {
    "value": { "name": "Product A", "price_unit": 99.95, "product_uom_id": [1, "Units"] },
    "warning": null
  }
}
```

### Backend dispatch

`REGISTER_METHOD("onchange", handleOnchange)` added to both `SaleViewModel` and `PurchaseViewModel` base constructors. The base implementation returns `{ "value": {}, "warning": null }` (no-op). Subclasses override to add logic.

### Sale order line onchange (`modules/sale/SaleModule.cpp`)

Trigger: `product_id` changed.

```
SELECT p.name, p.list_price, p.uom_id, u.name AS uom_name
FROM product_product p
LEFT JOIN uom_uom u ON u.id = p.uom_id
WHERE p.id = $1
```

Returns:
- `name` — product name
- `price_unit` — `list_price`
- `product_uom_id` — `[uom_id, uom_name]` array (Odoo many2one format)

### Purchase order line onchange (`modules/purchase/PurchaseModule.cpp`)

Same trigger. Uses `standard_price` (cost) instead of `list_price`, and `COALESCE(uom_po_id, uom_id)` to prefer the purchase UoM:

```sql
SELECT p.name, p.standard_price,
       COALESCE(p.uom_po_id, p.uom_id) AS uom_id,
       u.name AS uom_name
FROM product_product p
LEFT JOIN uom_uom u ON u.id = COALESCE(p.uom_po_id, p.uom_id)
WHERE p.id = $1
```

### Frontend (`web/static/src/app.js`)

Each order form view previously had a client-side `applyProductDefaults` method that queried `product.product` directly. This has been replaced with a server `onchange` call that centralizes the logic.

**`GenericFormView.triggerLineOnchange(fieldName, key, changedField)`** — generic version for simple relational line views.

**`SaleOrderFormView.triggerLineOnchange(key, changedField)`:**
```javascript
async triggerLineOnchange(key, changedField) {
    const line = this.state.lines.find(l => l._key === key);
    if (!line) return;
    try {
        const res = await RpcService.call(
            'sale.order.line', 'onchange', [line, changedField, {}], {});
        if (res && res.value) {
            Object.assign(line, res.value);
            if (changedField === 'product_id') {
                if (!line.product_uom_qty) line.product_uom_qty = 1;
                this.recalcLine(line);
            }
        }
    } catch (_) {}
}
```

**`PurchaseOrderFormView.triggerLineOnchange(key, changedField)`:** identical pattern targeting `purchase.order.line`.

`updateLine()` in both views now calls `triggerLineOnchange` instead of `applyProductDefaults` when `product_id` changes.

---

## 3. fields_get cache (PERF-D)

`JsonRpcDispatcher` caches `fields_get` results per model with a 300-second TTL (`TtlCache<std::string, nlohmann::json> fieldsGetCache_`). Field metadata is static between server restarts, so this eliminates repeated registry iteration on every form open. Cache is invalidated on server restart.

---

## 4. Files changed

| File | Change |
|---|---|
| `modules/base/FieldRegistry.hpp` | Added `compute`, `depends` to `FieldDef`; updated `toJson()` |
| `modules/sale/SaleModule.cpp` | Computed field markers; `handleOnchange` for order line |
| `modules/purchase/PurchaseModule.cpp` | Computed field markers; `handleOnchange` for order line |
| `core/infrastructure/JsonRpcDispatcher.hpp` | PERF-D: `fieldsGetCache_` with 300s TTL |
| `web/static/src/app.js` | Replaced `applyProductDefaults` with `triggerLineOnchange` in Sale and Purchase form views |

---

## 5. Limitations / future work

- **Stored computed fields**: the current implementation marks fields as `compute=true` in metadata only. The actual column values are computed at write time by SQL triggers or application logic already present in the module. No separate compute scheduling exists yet.
- **Multi-field onchange**: only `product_id` triggers are implemented. Other triggers (e.g., `product_uom_qty` changing after a price rule lookup) are deferred.
- **Onchange for header fields**: partner/payment term changes on the order header are not yet handled server-side.
