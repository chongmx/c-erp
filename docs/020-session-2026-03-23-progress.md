# Session 2026-03-23 — Progress Log

Three major feature areas completed. Build clean after all changes.

---

## 1. Product Form Tabs — Sales / Purchase / Accounting (Full)

**Files:** `modules/product/ProductModule.hpp`, `web/static/src/app.js`, `web/static/src/app.css`

### Backend — 7 new columns via ALTER TABLE ADD COLUMN IF NOT EXISTS

```sql
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS invoice_policy       VARCHAR NOT NULL DEFAULT 'order';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS sale_line_warn       VARCHAR NOT NULL DEFAULT 'no-message';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS sale_line_warn_msg   TEXT;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_method      VARCHAR NOT NULL DEFAULT 'purchase';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_lead_time   NUMERIC(8,2) NOT NULL DEFAULT 0;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_line_warn   VARCHAR NOT NULL DEFAULT 'no-message';
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS purchase_line_warn_msg TEXT;
```

All 7 fields added to `ProductProduct::registerFields()`, `serializeFields()`, `deserializeFields()`.

### Frontend — Sales tab

- **Invoicing Policy** select: `order` (Ordered Quantities) | `delivery` (Delivered Quantities)
- **Sales Warning** select: `no-message` | `warning` | `block` — warning message textarea appears conditionally when not `no-message`
- **Sales Description** textarea (existing, moved to right column)

### Frontend — Purchase tab

- **Control Policy** select: `purchase` (Ordered Quantities) | `receive` (Received Quantities)
- **Purchase Lead Time** number input (days)
- **Purchase Warning** select + conditional message textarea
- **Purchase Description** textarea (right column)

### Frontend — Accounting tab (enhanced)

- Left section **Receivables**: Income Account select (full account.account list) + Customer Taxes note
- Right section **Payables**: Expense Account select + Vendor Taxes note
- Section headers (`.prd-tab-section-title` CSS class added)

---

## 2. WorldData + Countries Settings Tab

**Files:** `modules/base/WorldData.hpp` (new), `modules/base/BaseModule.hpp`, `web/static/src/app.js`, `web/static/src/app.css`

### WorldData.hpp

New file — seeds all 250 world countries + ~700 states for 30+ countries.

```cpp
inline void seedWorldData_(pqxx::work& txn) {
    // Reset sequence, insert 250 countries ON CONFLICT (code) DO NOTHING,
    // insert ~700 states via VALUES+JOIN ON CONFLICT (country_id,code) DO NOTHING
}
```

Countries covered with states: MY(16), US(51), CA(13), GB(4), AU(8), DE(16), FR(18), IN(36), JP(47), CN(33), ID(37), BR(27), MX(32), ZA(9), AE(7), SA(13), SG(5), NZ(16), NL(12), BE(11), CH(26), ES(19), IT(20), NG(37), EG(27), PK(7), BD(8), TH(77), VN(63), PH(81).

### BaseModule.hpp changes

```sql
-- Added to CREATE TABLE res_country
active BOOLEAN NOT NULL DEFAULT TRUE

-- Applied as migration for existing installs
ALTER TABLE res_country ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE;

-- Added UNIQUE constraint for state seeding
ALTER TABLE res_country_state ADD CONSTRAINT res_country_state_country_id_code_key
    UNIQUE (country_id, code);  -- idempotent via DO $$ IF NOT EXISTS block
```

- `ResCountry` model: `active` field added to registerFields/serialize/deserialize
- `ResCountryViewModel` replaced with write-capable inline struct (adds `write` handler)
- `seedCountries_()` now calls `seedWorldData_(txn)` instead of inserting 15 hardcoded rows

### ERPSettingsView — Countries tab

New 5th tab button in settings:
- Loads all 250 countries (`res.country.search_read`)
- Live search filter (by name or code)
- Scrollable checkbox list (max-height 480px)
- Each checkbox calls `res.country.write([id], {active})` immediately on toggle
- Optimistic UI update with revert on error
- "Saved!" / error feedback inline

CSS classes added: `.erp-countries-list`, `.erp-country-item`, `.erp-country-code`

---

## 3. Contact Form — Company Name Styled Select

**File:** `web/static/src/app.js`

Replaced `<datalist>`+`<input>` combination with styled `<select>` matching Country/State dropdowns:

- Normal mode: `<select>` with company list + "— New Company —" option
- "New Company" selected → switches to free-text `<input>` with ✕ Cancel link
- `newCompanyMode: false` added to state
- `onCompanyNameSelect(e)` handler dispatches between modes
- `toggleCompany()` resets `newCompanyMode = false` when toggling company type

---

## 4. Stock: Warehouse + Location Form Views + Internal Transfers

**Files:** `modules/stock/StockModule.hpp`, `web/static/src/app.js`

### Backend — StockWarehouse model

```sql
CREATE TABLE IF NOT EXISTS stock_warehouse (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL,
    code             VARCHAR(5) NOT NULL UNIQUE,
    company_id       INTEGER REFERENCES res_company(id)        ON DELETE SET NULL,
    lot_stock_id     INTEGER REFERENCES stock_location(id)     ON DELETE SET NULL,
    view_location_id INTEGER REFERENCES stock_location(id)     ON DELETE SET NULL,
    in_type_id       INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
    out_type_id      INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
    int_type_id      INTEGER REFERENCES stock_picking_type(id) ON DELETE SET NULL,
    active           BOOLEAN NOT NULL DEFAULT TRUE,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
);
```

**Seed:**
```sql
INSERT INTO stock_warehouse (id, name, code, company_id, lot_stock_id, view_location_id, in_type_id, out_type_id, int_type_id)
VALUES (1, 'Main Warehouse', 'WH', 1, 4, 3, 1, 2, 3) ON CONFLICT (id) DO NOTHING;
```

- `StockWarehouse` model: full registerFields/serialize/deserialize
- `GenericViewModel<StockWarehouse>` registered as `stock.warehouse`

### Backend — New ir_act_window entries

| ID | Name | Model | Domain |
|----|------|-------|--------|
| 22 | Receipts | stock.picking | `[["picking_type_id","=",1]]` |
| 23 | Deliveries | stock.picking | `[["picking_type_id","=",2]]` |
| 24 | Internal Transfers | stock.picking | `[["picking_type_id","=",3]]` |
| 25 | Warehouses | stock.warehouse | `[]` |

> Note: action 17 renamed to "All Transfers". Previous plan reserved 22-24 for lots/reorder/putaway — those are now shifted (see updated plan.md).

### Backend — New ir_ui_menu entries

| ID | Name | Parent | Action |
|----|------|--------|--------|
| 200 | Receipts | 91 (Operations) | 22 |
| 201 | Deliveries | 91 (Operations) | 23 |
| 202 | Internal Transfers | 91 (Operations) | 24 |
| 95 | All Transfers | 91 (Operations) | 17 |
| 203 | Warehouses | 92 (Configuration) | 25 |

### Frontend — LocationFormView

New OWL component for `stock.location`:
- Breadcrumbs + Save/Discard
- Fields: Location Name, Parent Location (select from all locations), Usage (internal/view/supplier/customer/inventory/transit), Full Path (read-only), Active checkbox
- Full CRUD (create + write)

### Frontend — WarehouseFormView

New OWL component for `stock.warehouse`:
- Breadcrumbs + Save/Discard
- Left: Warehouse Name, Short Name (auto-uppercased on save)
- Right: Main Stock Location select (filtered to internal+view), Receipts/Deliveries/Internal Transfers picking type selects
- Full CRUD

### ActionView routing additions

```js
get isStockLocationModel()  { return this.currentAction.res_model === 'stock.location'; }
get isStockWarehouseModel() { return this.currentAction.res_model === 'stock.warehouse'; }
```

Both components registered in `static components = { ... }`.

---

## IR ID Reservation Update

Action IDs 22-24 are now used for filtered stock.picking views. Lot/Serial (Phase 28) and Reordering/Putaway (Phase 29) now use IDs 26+ (see updated plan.md).
