# Product Form (product.product) — Deferred / Stub Items

Items visible in the Product detail page that are currently hardcoded,
stubbed, or not yet implemented. All are candidates for future phases.

---

## 1. Stat widget — Go to Website

**Location:** Stat widget row, first button
**Current state:** Disabled stub (`prd-stat-disabled`), icon only
**What it should be:** Opens the published product page in the website/eCommerce module.

**Depends on:** Phase N/A — eCommerce/website module is out of scope for this ERP.

---

## 2. Stat widget — Units On Hand

**Location:** Stat widget row, second button
**Current state:** Disabled stub, always shows `0`
**What it should be:** Real count from `stock.quant` — sum of `quantity` where
`product_id = this.recordId AND location_id.usage = 'internal'`.

**Work needed:**
- Query `stock_quant` with location join in `ProductFormView.load()`
- Show real qty; clicking navigates to stock quants filtered by product

**Depends on:** `stock_quant` table (already exists if inventory module is active).

---

## 3. Stat widget — Forecasted

**Location:** Stat widget row, third button
**Current state:** Disabled stub, always shows `0`
**What it should be:** Forecasted quantity = On Hand + incoming - outgoing
(from `stock.forecasted_product_product` or computed from moves).

**Work needed:** Requires computing virtual stock from `stock_move` lines
in states `assigned`/`confirmed`. Non-trivial query.

**Suggest as part of Phase 28 (Lot/Serial) or a dedicated inventory
forecasting phase.**

---

## 4. Stat widget — Reordering Rules

**Location:** Stat widget row, fourth button
**Current state:** Disabled stub, always shows `0`
**What it should be:** Count of `stock.warehouse.orderpoint` records for
this product.

**Work needed:**
- `stock_warehouse_orderpoint` table: `id, product_id, product_min_qty,
  product_max_qty, qty_multiple, location_id`
- Count in `load()`, navigate to filtered list on click

**Suggest Phase 29 or alongside Replenishment feature.**

---

## 5. Stat widget — Bill of Materials

**Location:** Stat widget row, fifth button
**Current state:** Disabled stub, always shows `0`
**What it should be:** Count of `mrp.bom` records where `product_id` matches.

**Depends on:** Phase 26 (MRP module).

---

## 6. Stat widget — Putaway Rules

**Location:** Stat widget row, sixth button
**Current state:** Disabled stub, always shows `0`
**What it should be:** Count of `stock.putaway.rule` records for this product.

**Work needed:**
- `stock_putaway_rule` table: `id, product_id, location_in_id, location_out_id`
- Count in `load()`, navigate to filtered list on click

**Suggest Phase 28 or alongside inventory configuration.**

---

## 7. Stat widget — Product Moves (active but stub navigation)

**Location:** Stat widget row, seventh button (the only active one)
**Current state:** Shows real `search_count` for `stock.move` filtered by
product, but clicking shows an `alert()` stub instead of navigating.
**What it should be:** Navigate to Moves History list filtered by product.

**Work needed:**
- In `onViewMoves()`: dispatch an action to open `stock.move` list view
  with domain `[['product_id','=',this.recordId]]`
- Wire through `ActionView` using existing action dispatch pattern

**Small effort — can be done in Phase 17g or standalone.**

---

## 8. Product image — upload / display

**Location:** Identity section, right side
**Current state:** Gray placeholder box with icon and "No image" label
**What it should be:** Display `product.product.image_1920` (base64 stored
in DB) and allow upload via file input.

**Work needed:**
- Add `image_1920 TEXT` column to `product_product` (or `product_template`)
- Expose in `registerFields()` and `serializeFields()`
- In form: `<img>` tag with `data:image/*;base64,{image}` src when present
- Upload: `<input type="file" accept="image/*">` → FileReader → base64 string → write

**Suggest Phase 17g or standalone (medium effort).**

---

## 9. Can be Expensed checkbox

**Location:** Identity section, below the product name
**Current state:** Checkbox rendered but disabled (`prd-check-disabled`)
**What it should be:** Maps to `product.product.can_be_expensed`
(`expense_ok BOOLEAN`). Should be writable.

**Work needed:**
- Add `expense_ok BOOLEAN DEFAULT false` to `product_product` (or template)
- Expose in fields and form; remove disabled styling

**Depends on:** HR Expense module (Phase N/A or future). The field itself
can be added without the full hr.expense module — small standalone fix.

---

## 10. Customer Taxes field

**Location:** General Info tab, right column, below Sales Price
**Current state:** Text `— (taxes not implemented)` stub
**What it should be:** Many2many → `account.tax` showing applicable customer
taxes for this product.

**Work needed:**
- `account_tax` table and `product_taxes_rel` M2M join table
- Load taxes in `load()`, render as multi-select or tag list
- Include in `onSave()` write call

**Depends on:** Phase 22 (Analytic/Accounting) or a dedicated taxes phase.

---

## 11. Sales tab

**Location:** Tab bar, second tab
**Current state:** Disabled stub — clicking has no effect
**What it should be:** Sales-specific product settings: sales description,
invoicing policy, optional products, etc.

**Depends on:** Sales module (Phase 17a already done for SO, but product
sales tab fields need separate work).

---

## 12. Purchase tab

**Location:** Tab bar, third tab
**Current state:** Disabled stub
**What it should be:** Vendor pricelists (`product.supplierinfo`),
purchase description, control policy, etc.

**Depends on:** Purchase module (Phase 17c already done for PO, but
product purchase tab needs `product_supplierinfo` table).

---

## 13. Inventory tab

**Location:** Tab bar, fourth tab
**Current state:** Disabled stub
**What it should be:** Routes (`stock.route` M2M), tracking (lot/serial/none),
packaging (`product.packaging`), responsible user, weight/volume.

**Depends on:** Phase 27 (Lot/Serial), Phase 29 (Package tracking).

---

## 14. Accounting tab

**Location:** Tab bar, fifth tab
**Current state:** Disabled stub
**What it should be:** Income/expense accounts (`account.account` M2o),
customer/vendor tax defaults.

**Depends on:** Accounting module (future phase).

---

## 15. Chatter panel

**Location:** Below the form card
**Current state:** Static stub — always shows `System: "Product created"`
**What it should be:** Real audit log of field changes and user messages.

**Depends on:** Phase 27 (mail.message / chatter infrastructure).

---

## Summary Table

| Item | Effort | Suggested Phase |
|------|--------|----------------|
| Go to Website | N/A | Out of scope (eCommerce) |
| Units On Hand | Small | Phase 17g or 29 |
| Forecasted qty | Medium | Phase 28 |
| Reordering Rules stat | Small | Phase 29 |
| Bill of Materials stat | Small | Phase 26 (MRP) |
| Putaway Rules stat | Small | Phase 28 |
| Product Moves navigation | Tiny | Phase 17g (standalone) |
| Product image upload | Medium | Phase 17g or standalone |
| Can be Expensed checkbox | Tiny | Standalone (field only) |
| Customer Taxes (M2M) | Medium | Phase 22 |
| Sales tab | Medium | Standalone or Phase 18 |
| Purchase tab | Medium | Standalone |
| Inventory tab | Large | Phase 27/28 |
| Accounting tab | Large | Future accounting phase |
| Chatter | Medium | Phase 27 |
