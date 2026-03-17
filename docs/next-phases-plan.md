# Upcoming Implementation Phases — Roadmap

Reference: `zzref/odoo/addons/`
Current state: base, auth, ir, account, uom, product (Phases 1–8) complete and compiling.

---

## Critical Path

```
[DONE] base → auth → ir → account → uom → product
                                               ↓
                                    Phase 9:  sale
                                               ↓
                                    Phase 10: purchase
                                               ↓
                                    Phase 11: stock  (most complex — deferred)

[INDEPENDENT]
                                    Phase 12: hr  (no product dep)
```

`sale` and `purchase` both require `product` and `uom` (now complete).

---

## ✅ Phase 7 — UOM (COMPLETE — see uom-product-progress.md)

**Reference:** `zzref/odoo/addons/uom/models/uom_uom.py`
**File:** `modules/uom/UomModule.hpp`
**Dependency:** base

### Why first
`product.uom_id` is NOT NULL in Odoo. Both sale lines and purchase lines reference `product_uom_id`. Without UOM the product model can't be saved correctly.

### Simplifications vs Odoo
- **Drop:** `relative_uom_id` hierarchy and factor computation (store factor as-is)
- **Drop:** `_compute_factor`, `_compute_rounding`, recursive factor chain
- **Keep:** flat table with `factor` for unit conversion display

### Schema

```sql
CREATE TABLE IF NOT EXISTS uom_uom (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    factor      NUMERIC(12,6) NOT NULL DEFAULT 1.0,   -- ratio vs base unit
    rounding    NUMERIC(12,6) NOT NULL DEFAULT 0.01,
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    uom_type    VARCHAR NOT NULL DEFAULT 'reference',  -- 'bigger'|'reference'|'smaller'
    category    VARCHAR NOT NULL DEFAULT 'Unit',       -- 'Unit'|'Weight'|'Volume'|'Time'|'Length'
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)
```

### Seeds (15 rows)

| name    | category | factor | uom_type  |
|---------|----------|--------|-----------|
| Units   | Unit     | 1.0    | reference |
| Dozens  | Unit     | 12.0   | bigger    |
| kg      | Weight   | 1.0    | reference |
| g       | Weight   | 0.001  | smaller   |
| Ton     | Weight   | 1000.0 | bigger    |
| L       | Volume   | 1.0    | reference |
| ml      | Volume   | 0.001  | smaller   |
| m³      | Volume   | 1000.0 | bigger    |
| Hours   | Time     | 1.0    | reference |
| Days    | Time     | 8.0    | bigger    |
| Minutes | Time     | 0.01667| smaller   |
| m       | Length   | 1.0    | reference |
| cm      | Length   | 0.01   | smaller   |
| km      | Length   | 1000.0 | bigger    |
| m²      | Area     | 1.0    | reference |

### ViewModels / Views
- `AccountViewModel<UomUom>` (reuse generic template)
- `uom.uom.list`: name, category, factor, active
- `uom.uom.form`: name, category, factor, rounding, active

### IR Menu additions
| id | name | res_model | path |
|----|------|-----------|------|
| 8  | Units of Measure | uom.uom | uom |

---

## ✅ Phase 8 — Product (COMPLETE — see uom-product-progress.md)

**Reference:** `zzref/odoo/addons/product/models/`
**Files:** `modules/product/ProductModule.hpp`, `modules/product/ProductViews.hpp`
**Dependency:** uom, account (for `taxes_id` FK — optional, can omit)

### Simplifications vs Odoo

**Drop entirely:**
- `product.attribute` / `product.template.attribute.line` / variant management — complex M2M pivot tables, JS configurator needed
- `product.pricelist` / `product.pricelist.item` — deferred to Phase 9+
- `product.combo` / `product.combo.item`
- `product.document`, `product.tag` (M2M)
- `product.supplierinfo` — add in Phase 10 (purchase)
- `seller_ids`, `variant_seller_ids`
- `combo_ids`, `product_properties`

**Keep: one table per "model"** — skip the `product.template` + `product.product` split. Use a single `ProductProduct` class that IS both template and variant (like Odoo in simple mode). This matches how single-variant products work.

**Rationale:** The template/product split exists purely to support multi-variant products (color=Red, size=L → separate SKUs). For a basic ERP without a POS or configurator, all products are single-variant. We can add the split later.

### Schema

```sql
-- product.category (hierarchical)
CREATE TABLE IF NOT EXISTS product_category (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    parent_id   INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
)

-- product.product (simplified single-model — no template/variant split)
CREATE TABLE IF NOT EXISTS product_product (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL,
    default_code     VARCHAR,            -- Internal Reference / SKU
    barcode          VARCHAR,
    description      TEXT,
    description_sale TEXT,
    description_purchase TEXT,
    type             VARCHAR NOT NULL DEFAULT 'consu',
    -- 'consu'=consumable | 'service'=service | 'combo'=unused
    categ_id         INTEGER REFERENCES product_category(id) ON DELETE SET NULL,
    uom_id           INTEGER NOT NULL REFERENCES uom_uom(id) DEFAULT 1,
    uom_po_id        INTEGER NOT NULL REFERENCES uom_uom(id) DEFAULT 1,
    list_price       NUMERIC(16,4) NOT NULL DEFAULT 0,  -- Sales Price
    standard_price   NUMERIC(16,4) NOT NULL DEFAULT 0,  -- Cost
    volume           NUMERIC(16,4) NOT NULL DEFAULT 0,
    weight           NUMERIC(16,4) NOT NULL DEFAULT 0,
    sale_ok          BOOLEAN NOT NULL DEFAULT TRUE,
    purchase_ok      BOOLEAN NOT NULL DEFAULT TRUE,
    active           BOOLEAN NOT NULL DEFAULT TRUE,
    company_id       INTEGER REFERENCES res_company(id),
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

### Seeds

**product_category (3 rows):**

| id | name     | parent_id |
|----|----------|-----------|
| 1  | All      | NULL      |
| 2  | Goods    | 1         |
| 3  | Services | 1         |

**product_product:** No default seeds — user creates products.

### C++ Models

```
ProductCategory  → ODOO_MODEL("product.category",  "product_category")
ProductProduct   → ODOO_MODEL("product.product",   "product_product")
```

`ProductProduct::name` field: also expose as `product_tmpl_id` self-reference = id for Odoo compat.

### ViewModels / Views
- `AccountViewModel<ProductCategory>` (generic)
- `AccountViewModel<ProductProduct>` (generic)
- List views: product.product: default_code, name, type, categ_id, list_price, standard_price
- Form views: all stored fields

### IR Menu additions (continue numbering from 8)
| id | name     | res_model        | path     |
|----|----------|------------------|----------|
| 9  | Products | product.product  | products |
| 10 | Categories | product.category | product-categories |

---

## Phase 9 — Sale

**Reference:** `zzref/odoo/addons/sale/models/sale_order.py`, `sale_order_line.py`
**Files:** `modules/sale/SaleModule.hpp`, `modules/sale/SaleViews.hpp`
**Dependency:** product (product_id FK in sale lines)

### Simplifications vs Odoo

**Drop:**
- `sale.order`: require_signature, require_payment, prepayment_percent, pricelist_id, fiscal_position_id, utm fields, duplicate detection, locked, signed_by/signed_on
- `sale.order.line`: display_type (section/note), is_downpayment, product configurator fields, analytic_line_ids, combo_item_id, virtual_id
- Invoice creation wizard (down payment, grouped)
- Portal controllers
- `crm.team` dependency — just add `user_id` and skip team_id

**Keep:**
- Core state machine: draft → sale → cancel
- Invoice generation: `action_create_invoices()` creates one `account.move` of type `out_invoice` per order (or update existing draft invoice)
- `invoice_status`: `nothing` | `to invoice` | `invoiced`
- Amounts: amount_untaxed, amount_tax, amount_total computed from lines
- `payment_term_id` FK → account_payment_term

### State Machine

```
draft (Quotation) → [action_confirm()] → sale (Sales Order)
                 ↓                              ↓
             [cancel]                    [action_create_invoices()]
                                                ↓
                                          account.move out_invoice
```

### Schema

```sql
CREATE TABLE IF NOT EXISTS sale_order (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL DEFAULT 'New',
    state            VARCHAR NOT NULL DEFAULT 'draft',
    -- draft | sale | cancel
    partner_id       INTEGER NOT NULL REFERENCES res_partner(id),
    partner_invoice_id  INTEGER REFERENCES res_partner(id),
    partner_shipping_id INTEGER REFERENCES res_partner(id),
    date_order       TIMESTAMP NOT NULL DEFAULT now(),
    validity_date    DATE,
    payment_term_id  INTEGER REFERENCES account_payment_term(id),
    note             TEXT,
    currency_id      INTEGER REFERENCES res_currency(id),
    company_id       INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    user_id          INTEGER REFERENCES res_users(id),
    client_order_ref VARCHAR,          -- Customer's PO number
    origin           VARCHAR,
    invoice_status   VARCHAR NOT NULL DEFAULT 'nothing',
    -- nothing | to_invoice | invoiced
    amount_untaxed   NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_tax       NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_total     NUMERIC(16,2) NOT NULL DEFAULT 0,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)

CREATE TABLE IF NOT EXISTS sale_order_line (
    id               SERIAL PRIMARY KEY,
    order_id         INTEGER NOT NULL REFERENCES sale_order(id) ON DELETE CASCADE,
    sequence         INTEGER NOT NULL DEFAULT 10,
    product_id       INTEGER REFERENCES product_product(id),
    name             TEXT NOT NULL,                    -- line description
    product_uom_qty  NUMERIC(16,4) NOT NULL DEFAULT 1,
    product_uom_id   INTEGER REFERENCES uom_uom(id),
    price_unit       NUMERIC(16,4) NOT NULL DEFAULT 0,
    discount         NUMERIC(8,4) NOT NULL DEFAULT 0,  -- percentage
    tax_ids_json     TEXT NOT NULL DEFAULT '[]',       -- store as JSON [tax_id, ...]
    price_subtotal   NUMERIC(16,2) NOT NULL DEFAULT 0, -- qty * unit * (1-disc/100)
    price_tax        NUMERIC(16,2) NOT NULL DEFAULT 0,
    price_total      NUMERIC(16,2) NOT NULL DEFAULT 0,
    qty_invoiced     NUMERIC(16,4) NOT NULL DEFAULT 0,
    qty_delivered    NUMERIC(16,4) NOT NULL DEFAULT 0,
    company_id       INTEGER REFERENCES res_company(id),
    currency_id      INTEGER REFERENCES res_currency(id),
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

**Note:** `tax_ids` is a Many2many to `account_tax`. Simplification: store as JSON array of IDs rather than a junction table (avoids complex M2M joins in MVP).

### C++ Models

```
SaleOrder     → ODOO_MODEL("sale.order",      "sale_order")
SaleOrderLine → ODOO_MODEL("sale.order.line", "sale_order_line")
```

### ViewModels

**`SaleOrderViewModel`** — extends `AccountViewModel<SaleOrder>` with:
- `action_confirm()` — sets state='sale', assigns name from sequence (SO/YYYY/NNNN)
- `action_cancel()` — sets state='cancel' (from draft only)
- `action_create_invoices()` — creates `account.move` (out_invoice) from confirmed order

**`SaleOrderLineViewModel`** — generic `AccountViewModel<SaleOrderLine>` but with computed line amounts on write.

### Amount computation

On `write()` or `create()` for a sale_order_line, recompute:
```
price_subtotal = product_uom_qty * price_unit * (1 - discount/100)
price_tax      = price_subtotal * sum(tax.amount/100 for tax in tax_ids)
price_total    = price_subtotal + price_tax
```
Then UPDATE parent sale_order totals:
```sql
UPDATE sale_order SET
    amount_untaxed = (SELECT SUM(price_subtotal) FROM sale_order_line WHERE order_id = $1),
    amount_tax     = (SELECT SUM(price_tax)      FROM sale_order_line WHERE order_id = $1),
    amount_total   = (SELECT SUM(price_total)    FROM sale_order_line WHERE order_id = $1)
WHERE id = $1
```

### Invoice creation (simplified)

`action_create_invoices()` on a confirmed sale.order:
1. Create `account.move` with `move_type='out_invoice'`, `partner_id`, `payment_term_id`, `journal_id` = first SAL journal
2. For each `sale_order_line`: create `account_move_line` with:
   - `account_id` = first `income` type account (code 4000 Sales Revenue)
   - `name` = line.name
   - `quantity` = line.product_uom_qty
   - `price_unit` = line.price_unit
   - `debit` = 0, `credit` = price_subtotal (credit side — income)
   - Also create the receivable line: `debit` = amount_total, account = 1200 AR
3. Set `sale_order.invoice_status = 'invoiced'`

### IR Menu additions
| id | name        | res_model   | path   |
|----|-------------|-------------|--------|
| 11 | Sales Orders | sale.order | sales  |

---

## Phase 10 — Purchase

**Reference:** `zzref/odoo/addons/purchase/models/purchase_order.py`
**Files:** `modules/purchase/PurchaseModule.hpp`, `modules/purchase/PurchaseViews.hpp`
**Dependency:** product, account

### Simplifications vs Odoo

**Drop:**
- `partner_ref`, `origin`, priority, `dest_address_id` (dropship)
- `acknowledged`, `lock_confirmed_po`, `show_comparison`
- `purchase.bill.line.match` (3-way matching)
- `analytic_account.py` extensions
- Portal controllers
- Receipt tracking (qty_received, qty_received_method) — only track billing

**Keep:**
- State machine: draft → purchase → cancel
- Bill creation: `action_create_bills()` → creates `account.move` of type `in_invoice`
- `invoice_status`: `nothing` | `to bill` | `billed`
- `payment_term_id`, `date_planned` (expected delivery date)

### State Machine

```
draft (RFQ) → [action_confirm()] → purchase (Purchase Order)
                                          ↓
                                   [action_create_bills()]
                                          ↓
                                    account.move in_invoice (Bill)
```

### Schema

```sql
CREATE TABLE IF NOT EXISTS purchase_order (
    id               SERIAL PRIMARY KEY,
    name             VARCHAR NOT NULL DEFAULT 'New',
    state            VARCHAR NOT NULL DEFAULT 'draft',
    -- draft | purchase | cancel
    partner_id       INTEGER NOT NULL REFERENCES res_partner(id),
    date_order       TIMESTAMP NOT NULL DEFAULT now(),
    date_planned     DATE,             -- expected receipt date
    payment_term_id  INTEGER REFERENCES account_payment_term(id),
    note             TEXT,
    currency_id      INTEGER REFERENCES res_currency(id),
    company_id       INTEGER NOT NULL REFERENCES res_company(id) DEFAULT 1,
    user_id          INTEGER REFERENCES res_users(id),
    origin           VARCHAR,
    invoice_status   VARCHAR NOT NULL DEFAULT 'nothing',
    -- nothing | to_bill | billed
    amount_untaxed   NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_tax       NUMERIC(16,2) NOT NULL DEFAULT 0,
    amount_total     NUMERIC(16,2) NOT NULL DEFAULT 0,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)

CREATE TABLE IF NOT EXISTS purchase_order_line (
    id               SERIAL PRIMARY KEY,
    order_id         INTEGER NOT NULL REFERENCES purchase_order(id) ON DELETE CASCADE,
    sequence         INTEGER NOT NULL DEFAULT 10,
    product_id       INTEGER REFERENCES product_product(id),
    name             TEXT NOT NULL,
    product_qty      NUMERIC(16,4) NOT NULL DEFAULT 1,
    product_uom_id   INTEGER REFERENCES uom_uom(id),
    price_unit       NUMERIC(16,4) NOT NULL DEFAULT 0,
    discount         NUMERIC(8,4) NOT NULL DEFAULT 0,
    tax_ids_json     TEXT NOT NULL DEFAULT '[]',
    price_subtotal   NUMERIC(16,2) NOT NULL DEFAULT 0,
    price_tax        NUMERIC(16,2) NOT NULL DEFAULT 0,
    price_total      NUMERIC(16,2) NOT NULL DEFAULT 0,
    date_planned     DATE,
    qty_invoiced     NUMERIC(16,4) NOT NULL DEFAULT 0,
    qty_received     NUMERIC(16,4) NOT NULL DEFAULT 0,
    company_id       INTEGER REFERENCES res_company(id),
    currency_id      INTEGER REFERENCES res_currency(id),
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
)
```

### ViewModels

**`PurchaseOrderViewModel`** — extends `AccountViewModel<PurchaseOrder>` with:
- `action_confirm()` — sets state='purchase', assigns name (PO/YYYY/NNNN)
- `action_cancel()`
- `action_create_bills()` — creates `account.move` (in_invoice)

### Bill creation (simplified mirror of sale invoicing)

`action_create_bills()`:
1. Create `account.move` with `move_type='in_invoice'`, `partner_id`, `journal_id` = first PUR journal
2. For each line: create `account_move_line`:
   - Expense account (code 5000/6000), debit = price_subtotal
   - Payable account (code 2000 AP), credit = amount_total
3. Set `invoice_status = 'billed'`

### IR Menu additions
| id | name            | res_model      | path      |
|----|-----------------|----------------|-----------|
| 12 | Purchase Orders | purchase.order | purchases |

---

## Phase 11 — Stock (Deferred / Optional)

**Reference:** `zzref/odoo/addons/stock/models/` (26 models — most complex)
**Dependency:** product, purchase (for receipts), sale (for deliveries)

### Complexity warning
Full stock requires:
- 26 models including `stock.warehouse`, `stock.location`, `stock.picking`, `stock.move`, `stock.move.line`, `stock.quant`, `stock.lot`, `stock.rule`, `stock.orderpoint`
- Complex routing engine (procurement rules, reorder points)
- Barcode scanning UI
- Multi-step warehouse workflows (receive → quality check → put away)

### Simplified MVP scope (if implemented)
Keep only what's needed to record stock movements:

```
stock_location    — warehouse locations (hierarchical)
stock_picking     — delivery/receipt slip header
stock_move        — individual product movement
```

**Drop:** lots, packages, routes, reorder points, barcodes, quality checks, forecasting.

**State machine (simplified):**
```
draft → [action_confirm()] → confirmed → [action_assign()] →
assigned → [button_validate()] → done
```

This phase is large enough to be a full sub-project. Recommend implementing after sale + purchase are working end-to-end.

---

## Phase 12 — HR (Optional, Independent)

**Reference:** `zzref/odoo/addons/hr/models/`, `zzref/odoo/addons/resource/models/`
**Dependency:** base (independent of sale/purchase)

Can be implemented in parallel with Phase 9/10 since it doesn't depend on product.

### MVP scope

```sql
-- resource.calendar (working schedule)
resource_calendar: id, name, hours_per_day, company_id, active

-- hr.department
hr_department: id, name, parent_id, manager_id, company_id, active

-- hr.job (job positions)
hr_job: id, name, department_id, company_id, active

-- hr.employee
hr_employee: id, name, job_id, department_id, parent_id (manager),
             work_email, work_phone, resource_calendar_id,
             company_id, user_id (linked portal user), active
```

### IR Menu
| id | name        | res_model    | path      |
|----|-------------|--------------|-----------|
| 13 | Employees   | hr.employee  | employees |
| 14 | Departments | hr.department| departments |

---

## Implementation Notes

### Sequence naming
Follow pattern `{CODE}/{YEAR}/{4-digit-seq}` already established in account:
- Sale orders: `SO/2026/0001`
- Purchase orders: `PO/2026/0001`
- Stock pickings (future): `WH/IN/2026/0001`, `WH/OUT/2026/0001`

### `tax_ids_json` approach
For sale/purchase lines, using a JSON column `tax_ids_json TEXT DEFAULT '[]'` instead of a junction table avoids M2M complexity. Frontend sends `[1, 2]` → stored as `"[1,2]"`. On read, parse the JSON and resolve tax amounts. Trade-off: no FK integrity, but acceptable for MVP.

When account.tax amounts are needed, do:
```sql
SELECT SUM(amount) FROM account_tax WHERE id = ANY(ARRAY[1,2])
```

### Sequence counter approach
Simple auto-increment without year partitioning:
```sql
-- Add to sale_order module init:
CREATE SEQUENCE IF NOT EXISTS sale_order_seq START 1;
-- On confirm: SELECT nextval('sale_order_seq')
```
Name = `SO/2026/` + LPAD(nextval, 4, '0').

### `GenericViewModel<T>` — DONE
Extracted to `modules/base/GenericViewModel.hpp`. Include it in new modules with `#include "GenericViewModel.hpp"` (already on include path via `modules/base`).

### IR menu id continuity
The menu system now uses a **3-level hierarchy** (App → Section → Leaf) with non-sequential IDs. Old flat IDs 1-9 were deleted on boot.

**ir_act_window IDs (flat sequence):**
```
1  : res.partner  (Contacts)
2  : res.users    (Users)
3  : res.company  (Companies)
4  : account.account
5  : account.journal
6  : account.move
7  : account.payment
8  : uom.uom
9  : product.product
10 : product.category
11 : sale.order          ← next
12 : purchase.order      ← next
13 : hr.employee         ← next
14 : hr.department       ← next
```

**ir_ui_menu IDs (hierarchical, by tens):**
```
10  Accounting app
  11  Journal Entries, 12  Customers, 13  Vendors, 14  Configuration
  15  Payments, 16  Chart of Accounts, 17  Journals
20  Contacts app
  21  Contacts
30  Settings app
  31  Users, 32  Companies
50  Products app
  51  Products, 52  Configuration
  53  Units of Measure, 54  Categories
60  Sales app          ← next (SaleModule to create)
  61  Sales Orders
70  Purchase app       ← next (PurchaseModule to create)
  71  Purchase Orders
80  HR app             ← next (HrModule to create)
  81  Employees, 82  Departments
```

---

## Out of Scope (long term)

- `analytic` — cost centers, project cost allocation
- `crm` — leads, pipeline, CRM dashboard
- `project` — tasks, milestones, timesheets
- `mrp` — bills of materials, manufacturing orders
- `delivery` — carrier rate calculation, shipping labels
- `payment` — online payment providers (Stripe, PayPal)
- `website` / `point_of_sale` — e-commerce, POS terminal
- `l10n_*` — country-specific accounting localizations
- `mail` / `discuss` — message threads, activity management

---

## Priority Order

| Phase | Module   | Tables | Complexity | Status |
|-------|----------|--------|------------|--------|
| 7     | uom      | 1      | Low        | ✅ DONE |
| 8     | product  | 2      | Low-Medium | ✅ DONE |
| 9     | sale     | 2      | Medium     | **NEXT** |
| 10    | purchase | 2      | Medium     | **NEXT** |
| 12    | hr       | 4      | Low-Medium | NEXT (independent) |
| 11    | stock    | 3-9    | High       | Deferred |
