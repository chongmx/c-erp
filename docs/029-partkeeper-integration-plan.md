# PartKeeper Integration Plan — Parts Management Enhancements
*Planned: 2026-03-24*

## Summary

PartKeeper is a dedicated electronics component inventory manager. This document analyses its
data model and maps each concept either to existing tables in our system or to new phases.

---

## Gap Analysis: What We Have vs What PartKeeper Adds

| PartKeeper Concept | Our System | Status |
|---|---|---|
| Part name / description | `product_product.name / description` | ✅ Covered |
| Internal part number | `product_product.default_code` | ✅ Covered |
| Category (with parent) | `product_category` (has parent_id) | ✅ Schema OK — ⚠️ Frontend flat |
| Unit of measure | `uom_uom` | ✅ Covered |
| Supplier / distributor link | `product_supplierinfo` (Phase A3b) | 🔜 Planned |
| Storage location | `stock_location` (hierarchical) | ✅ Covered |
| BOM / project parts | `mrp_bom` / `mrp_bom_line` | ✅ Covered |
| **Footprints (IC packages)** | — | 🆕 New |
| **Footprint categories** | — | 🆕 New |
| **Part parameters / specs** | — | 🆕 New |
| **SI prefixes + units** | — | 🆕 New |
| **Manufacturer part numbers (MPN)** | — | 🆕 New |
| **Distributor order number / packaging unit** | — | 🆕 Extend A3b |
| **Min stock level per product** | — | 🆕 New column |
| **Part status / condition** | — | 🆕 New column |
| **Datasheets / attachments** | — | 🆕 New |
| Category tree frontend | parent_id exists, UI is flat list | 🔜 Frontend work |
| Meta-parts (generic matching) | — | ⏸ Defer (complex) |
| Storage location images | — | ⏸ Defer |

---

## Phase Overview

```
PK1 — Category Tree UI              (product_category already hierarchical — frontend fix)
PK2 — Footprints                    (2 new tables: part_footprint, part_footprint_category)
PK3 — Part Parameters & SI Units    (3 new tables: part_parameter, part_unit, part_si_prefix)
PK4 — Manufacturer Part Numbers     (1 new table: part_manufacturer_info)
PK5 — Enhanced Supplier Info        (extend A3b: order_number, packaging_unit on supplierinfo)
PK6 — Min Stock & Part Status       (new columns on product_product)
PK7 — Attachments / Datasheets      (1 new table: ir_attachment)
```

Total new tables: **7** (plus 3 new columns on existing tables)

---

## Phase PK1 — Hierarchical Category Browser

**Effort:** Small | **0 new tables** | **Frontend only**

### Problem

`product_category` already has `parent_id` but the frontend renders it as a flat picklist
(alphabetical). When categories are nested (e.g. "Semiconductors > Microcontrollers > ARM"),
the relationship is invisible.

### Backend

- `ProductCategoryViewModel::handleSearchRead()` already returns `parent_id`.
- Add `category_path` computed field: walk the parent chain in SQL using a recursive CTE
  and return it as a string `"Electronics > Semiconductors > ARM"` in serializeFields().
- This lets the frontend display context without a separate tree API call.

```sql
-- Recursive CTE to build path (called per record in serializeFields)
WITH RECURSIVE cat_path AS (
    SELECT id, name, parent_id, name::TEXT AS path
    FROM product_category WHERE id = $1
    UNION ALL
    SELECT p.id, p.name, p.parent_id, p.name || ' > ' || c.path
    FROM product_category p JOIN cat_path c ON p.id = c.parent_id
)
SELECT path FROM cat_path WHERE parent_id IS NULL;
```

### Frontend

- `ProductCategoryListView`: add `category_path` column; rename "Name" → "Full Path".
- Category picklist on product form: show `category_path` in option text so nesting is clear.
- Configuration → Categories form: show parent_id as a Many2one field (allow nesting).

### IR changes

None — menus exist. Only view arch update.

---

## Phase PK2 — Footprints (IC Package Types)

**Effort:** Medium | **2 new tables**

### Concept

Footprints describe the physical package of an electronic component: DIP-8, SOIC-8, QFP-44,
BGA-256, 0402, etc. They have their own two-level category hierarchy
(e.g. "IC Packages > DIP", "SMD Passives > 0402").
Each footprint can have a diagram image (stored as base64 in the DB, same as product image).

### Schema

```sql
CREATE TABLE IF NOT EXISTS part_footprint_category (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    parent_id   INTEGER REFERENCES part_footprint_category(id) ON DELETE SET NULL,
    description TEXT,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

CREATE TABLE IF NOT EXISTS part_footprint (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL UNIQUE,   -- e.g. "PDIP-8", "SOIC-8N"
    description TEXT,
    category_id INTEGER REFERENCES part_footprint_category(id) ON DELETE SET NULL,
    image       TEXT,                      -- base64 SVG/PNG diagram
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

ALTER TABLE product_product
    ADD COLUMN IF NOT EXISTS footprint_id INTEGER REFERENCES part_footprint(id) ON DELETE SET NULL;
```

### Backend

- `PartFootprintCategory` struct + ODOO_MODEL("part.footprint.category", "part_footprint_category")
- `PartFootprintCategoryViewModel`: GenericViewModel CRUD
- `PartFootprint` struct + ODOO_MODEL("part.footprint", "part_footprint")
- `PartFootprintViewModel`: GenericViewModel CRUD; serializeFields includes `category_id` name
- `ProductProductViewModel`: expose `footprint_id` in serializeFields (Many2one)
- `checkModelAccess_()`: add `part.footprint` and `part.footprint.category` to `kAllowed`

### Frontend

**Footprint list:** name | category | description | image thumbnail

**Footprint form:**
- Name (required), Category (Many2one), Description
- Image upload (same base64 pattern as product image)

**Product form — General tab:** add "Footprint" Many2one field below Category.

### IR menus / actions

```
Inventory → Configuration → Footprints          (new menu id=110, action id=37)
Inventory → Configuration → Footprint Categories (new menu id=111, action id=38)
```

### Groups

Footprint management: `INVENTORY_USER (11)` — same as stock configuration.

---

## Phase PK3 — Part Parameters & SI Units

**Effort:** Large | **3 new tables**

### Concept

Electronic components have electrical/physical specifications: resistance (Ω), capacitance (F),
voltage rating (V), power (W), tolerance (%), operating temperature, etc.

PartKeeper supports SI prefixes (pico/nano/micro/milli/kilo/mega/giga) so that a 10µF
capacitor stores `value=10, si_prefix='µ', unit='F'` and normalizes to `0.00001 F`
for range comparisons.

This enables powerful searching: "find all resistors between 1kΩ and 10kΩ".

### Schema

```sql
CREATE TABLE IF NOT EXISTS part_si_prefix (
    id       SERIAL PRIMARY KEY,
    name     VARCHAR NOT NULL,    -- "micro", "nano", "pico"
    symbol   VARCHAR NOT NULL,    -- "µ", "n", "p"
    exponent INTEGER NOT NULL,    -- -6, -9, -12
    base     INTEGER NOT NULL DEFAULT 10
);

-- Seeds (idempotent, insert by name):
-- pico  p  -12   nano   n  -9   micro  µ  -6   milli  m  -3
-- (none)   0     kilo   k   3   mega   M   6   giga   G   9

CREATE TABLE IF NOT EXISTS part_unit (
    id         SERIAL PRIMARY KEY,
    name       VARCHAR NOT NULL UNIQUE,   -- "Ohm", "Farad", "Volt", "Ampere", "Watt", "Henry"
    symbol     VARCHAR NOT NULL,          -- "Ω", "F", "V", "A", "W", "H"
    prefixes   INTEGER[] NOT NULL DEFAULT '{}'  -- allowed si_prefix ids
);

CREATE TABLE IF NOT EXISTS part_parameter (
    id                  SERIAL PRIMARY KEY,
    product_id          INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
    name                VARCHAR NOT NULL,          -- "Resistance", "Capacitance"
    description         VARCHAR,
    value_type          VARCHAR NOT NULL DEFAULT 'numeric',  -- 'numeric' | 'string'

    -- numeric fields
    value               DOUBLE PRECISION,
    normalized_value    DOUBLE PRECISION,          -- value * 10^exponent
    min_value           DOUBLE PRECISION,
    max_value           DOUBLE PRECISION,
    normalized_min      DOUBLE PRECISION,
    normalized_max      DOUBLE PRECISION,
    si_prefix_id        INTEGER REFERENCES part_si_prefix(id),
    min_si_prefix_id    INTEGER REFERENCES part_si_prefix(id),
    max_si_prefix_id    INTEGER REFERENCES part_si_prefix(id),
    unit_id             INTEGER REFERENCES part_unit(id),

    -- string field
    string_value        VARCHAR,

    create_date         TIMESTAMP DEFAULT now(),
    write_date          TIMESTAMP DEFAULT now()
);
```

### Backend

- `PartSiPrefix`, `PartUnit`, `PartParameter` structs + models
- `PartSiPrefixViewModel`: read-only (seeded); `PartUnitViewModel`: CRUD
- `PartParameterViewModel`: CRUD; normalized_value = value × 10^(si_prefix.exponent)
- `ProductProductViewModel::serializeFields()`: join part_parameter and return as `parameters` array
- `handleWrite()`: handle parameters One2many commands (create/write/unlink)
- Search: `PartParameterViewModel::handleSearchRead()` supports `normalized_value >= X AND normalized_value <= Y`
- `checkModelAccess_()`: add `part.parameter`, `part.unit`, `part.si.prefix` to `kAllowed`

### Frontend

**Product form → new "Parameters" tab:**

```
[ + Add Parameter ]
┌─────────────────┬────────────┬───────┬──────────────┬────────────┬────────────┐
│ Name            │ Value Type │ Value │ SI Prefix    │ Unit       │ String Val │
├─────────────────┼────────────┼───────┼──────────────┼────────────┼────────────┤
│ Resistance      │ Numeric    │ 10    │ k (kilo)     │ Ω (Ohm)    │            │
│ Tolerance       │ String     │       │              │            │ ±5%        │
│ Power           │ Numeric    │ 250   │ m (milli)    │ W (Watt)   │            │
└─────────────────┴────────────┴───────┴──────────────┴────────────┴────────────┘
```

- Inline editable rows (same pattern as BOM lines)
- When value_type = 'string': hide numeric columns; show string_value
- When value_type = 'numeric': show value + si_prefix + unit
- Min/max range (optional, collapsed by default)

**IR menus:**
```
Inventory → Configuration → Part Units   (new menu id=112, action id=39)
```

SI prefixes are seeded and not user-editable (no UI needed).

### Groups

`INVENTORY_USER (11)` for read; `INVENTORY_MANAGER` for create/delete units.

---

## Phase PK4 — Manufacturer Part Numbers (MPN)

**Effort:** Small | **1 new table**

### Concept

A product (e.g. "ATmega328P") can be sold by multiple manufacturers under different part numbers.
PartKeeper links each part to a manufacturer (`res.partner`) with the manufacturer's own
part number (MPN). This is separate from the vendor/supplier pricelist.

### Schema

```sql
CREATE TABLE IF NOT EXISTS part_manufacturer_info (
    id            SERIAL PRIMARY KEY,
    product_id    INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
    partner_id    INTEGER NOT NULL REFERENCES res_partner(id),    -- manufacturer
    part_number   VARCHAR,     -- manufacturer's part number / MPN
    sequence      INTEGER NOT NULL DEFAULT 10,
    create_date   TIMESTAMP DEFAULT now(),
    write_date    TIMESTAMP DEFAULT now()
);
```

### Backend

- `PartManufacturerInfo` struct + ODOO_MODEL("part.manufacturer.info", "part_manufacturer_info")
- `PartManufacturerInfoViewModel`: CRUD
- `ProductProductViewModel::serializeFields()`: return `manufacturer_ids` array with partner name + part_number
- `handleWrite()`: handle manufacturer_ids One2many commands
- `checkModelAccess_()`: add to `kAllowed`

### Frontend

**Product form → General tab** (below "Customer" and "Vendor" sections, or new sub-section):

```
Manufacturers
┌────────────────────────────┬─────────────────────────┐
│ Manufacturer               │ Part Number (MPN)        │
├────────────────────────────┼─────────────────────────┤
│ Microchip Technology       │ ATMEGA328P-PU            │
│ [+ Add a line]             │                          │
└────────────────────────────┴─────────────────────────┘
```

- Manufacturer is a Many2one to `res.partner` (filtered by supplier=true or any partner)
- Part number is a free text field

---

## Phase PK5 — Enhanced Distributor / Supplier Info

**Effort:** Small | **0 new tables** — extend Phase A3b

### Concept

PartKeeper's `PartDistributor` adds fields that our planned `product_supplierinfo` does not cover:
- `order_number` — distributor's own SKU/order code (e.g. Digikey: "ATMEGA328P-PU-ND")
- `packaging_unit` — minimum order quantity (e.g. sold in reels of 3000)
- `currency` — per-supplier currency (we already have `currency_id` in schema plan)

### Schema extension (add to Phase A3b DDL)

```sql
ALTER TABLE product_supplierinfo
    ADD COLUMN IF NOT EXISTS order_number    VARCHAR,
    ADD COLUMN IF NOT EXISTS packaging_unit  INTEGER NOT NULL DEFAULT 1;
```

### Frontend extension

Phase A3b already adds the vendor pricelist table. Extend with two extra columns:
- "Order Ref." (order_number) — distributor's SKU
- "Pack Qty" (packaging_unit) — minimum buy quantity

---

## Phase PK6 — Min Stock Level & Part Status

**Effort:** Small | **0 new tables** — columns on product_product

### Concept

PartKeeper tracks two simple per-product fields that complement our full reordering rules:
- `min_stock_qty` — below this level the product is "low stock"
- `part_status` — condition of the part: 'new', 'used', 'broken', 'unknown'

These are simpler than Phase 29 (full reordering rules with location granularity).

### Schema

```sql
ALTER TABLE product_product
    ADD COLUMN IF NOT EXISTS min_stock_qty   NUMERIC(16,4) NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS part_status     VARCHAR NOT NULL DEFAULT 'new',
    ADD COLUMN IF NOT EXISTS needs_review    BOOLEAN NOT NULL DEFAULT FALSE;
```

### Backend

- `ProductProductViewModel::serializeFields()`: expose `min_stock_qty`, `part_status`, `needs_review`
- `handleWrite()`: accept updates to these fields
- `ProductProductViewModel::handleSearchRead()`: support filter `[['part_status','=','used']]`

### Frontend

**Product form → Inventory tab:**

Add section "Stock Control":
- Min Stock Qty: numeric field
- Part Status: selection (`new` | `used` | `broken` | `unknown`)
- Needs Review: checkbox

**Product list view:** optional low-stock indicator (highlight row when
`qty_on_hand < min_stock_qty`)

---

## Phase PK7 — Attachments / Datasheets

**Effort:** Medium | **1 new table**

### Concept

PartKeeper allows attaching datasheets (PDF), pinout images, and reference documents to parts.
This maps to Odoo's `ir.attachment` pattern: a generic attachment table polymorphically linked
to any record by `(res_model, res_id)`.

### Schema

```sql
CREATE TABLE IF NOT EXISTS ir_attachment (
    id           SERIAL PRIMARY KEY,
    name         VARCHAR NOT NULL,          -- display name / filename
    res_model    VARCHAR NOT NULL,          -- 'product.product', 'part.footprint', ...
    res_id       INTEGER NOT NULL,          -- PK of the linked record
    file_name    VARCHAR,                   -- original file name
    mimetype     VARCHAR,                   -- 'application/pdf', 'image/png', ...
    file_size    INTEGER,                   -- bytes
    db_datas     TEXT,                      -- base64-encoded file content
    description  TEXT,
    is_public    BOOLEAN NOT NULL DEFAULT FALSE,
    create_uid   INTEGER REFERENCES res_users(id),
    create_date  TIMESTAMP DEFAULT now(),
    write_date   TIMESTAMP DEFAULT now()
);

CREATE INDEX IF NOT EXISTS ir_attachment_res_idx
    ON ir_attachment(res_model, res_id);
```

### Backend

- `IrAttachment` struct + ODOO_MODEL("ir.attachment", "ir_attachment")
- `IrAttachmentViewModel`: CRUD
  - `handleCreate()`: set res_model, res_id, db_datas (base64), compute file_size
  - `handleSearchRead()`: filter by `[['res_model','=','product.product'],['res_id','=',42]]`
  - `handleUnlink()`: require ownership or admin
- `checkModelAccess_()`: add `ir.attachment` to `kAllowed` (any authenticated user can read public ones; write restricted by res_model perms)
- `ProductProductViewModel::serializeFields()`: join ir_attachment for this product, return as `attachment_ids` array

### Frontend

**Product form → new "Documents" tab:**

```
[ Upload Datasheet ]  [ Upload Image ]
┌────────────────────────────┬──────────────┬──────────┬───────────────┐
│ File Name                  │ Type         │ Size     │ Actions       │
├────────────────────────────┼──────────────┼──────────┼───────────────┤
│ ATmega328P-datasheet.pdf   │ PDF          │ 2.3 MB   │ [View] [Del]  │
│ ATmega328P-pinout.png      │ Image        │ 145 KB   │ [View] [Del]  │
└────────────────────────────┴──────────────┴──────────┴───────────────┘
```

- Upload: multipart/form-data → POST /web/binary/upload (new HTTP route)
- View: GET /web/content/ir_attachment/<id> — stream file from db_datas
- Delete: calls ir.attachment unlink
- Images: shown inline as thumbnails

### New HTTP routes (non-JSON-RPC)

```
POST /web/binary/upload          — multipart upload → creates ir.attachment
GET  /web/content/<id>           — streams file (checks session auth)
GET  /web/content/<id>/<filename> — same, with suggested filename
```

These are HTTP routes registered on the Drogon app directly, not JSON-RPC.

---

## Implementation Order

Prioritised by usefulness and dependencies:

```
PK6  (min stock + status)           — zero dependencies, pure column additions
PK1  (category tree UI)             — zero dependencies, frontend fix
PK4  (manufacturer MPN)             — depends only on res_partner existing
PK5  (enhanced supplier info)       — fold into Phase A3b (already planned next)
PK2  (footprints)                   — independent new entity
PK7  (attachments)                  — depends on HTTP binary routes
PK3  (parameters + SI units)        — most complex; depends on PK2 being done first
```

Recommended sequence within overall roadmap:

```
A3b (vendor pricelist + PK5 extension) → PK6 → PK1 → PK4 → PK2 → PK7 → PK3
```

---

## Updated IR ID Continuity (Parts Management)

```
150  Parts Management app (new app icon — optional, or under Inventory)
  or add under Inventory → Configuration:

Inventory → Configuration:
  93    Locations ✅
  94    Operation Types ✅
  203   Warehouses ✅
  110   Footprints               (PK2)
  111   Footprint Categories     (PK2)
  112   Part Units               (PK3)

ir_act_window:
  37   Footprints                (PK2)
  38   Footprint Categories      (PK2)
  39   Part Units                (PK3)
```

---

## Table Count Projection (Parts Management)

| After Phase | New Tables | Running Total |
|-------------|-----------|---------------|
| Current | — | ~45 |
| A3b + PK5 | 1 | 46 |
| PK6 | 0 (3 columns) | 46 |
| PK1 | 0 (frontend) | 46 |
| PK4 | 1 | 47 |
| PK2 | 2 | 49 |
| PK7 | 1 | 50 |
| PK3 | 3 | 53 |

---

## Out of Scope (Deferred)

| PartKeeper Feature | Reason Deferred |
|---|---|
| Meta-parts (generic matching by parameter criteria) | Complex query engine, low priority |
| Storage location images | Minor UX, db_datas pattern same as PK7 — easy to add later |
| Project runs (production batch tracking) | Covered by MRP production orders |
| Stock audit log | Covered by chatter/audit log on transfers |
| Distributor SKU URL template | Cosmetic feature |
