# Phase 17g — Transfer + Product Form Small Fixes

**Status:** ✅ Code complete — ⚠️ Transfer form changes NOT tested in browser
**Date:** 2026-03-20
**Build:** Clean

> **Note:** Product form (image upload, expense_ok checkbox) was verified by user.
> Transfer form changes (Responsible dropdown, Company from DB, editable locations on draft)
> have not been manually tested. Test when possible before relying on them in production.

---

## Changes Made

### 1. Backend — StockModule.hpp

**New column migration:**
```sql
ALTER TABLE stock_picking ADD COLUMN IF NOT EXISTS user_id INTEGER REFERENCES res_users(id);
```

**StockPicking model:**
- Added `int userId = 0;` member
- `registerFields()`: added `user_id` Many2one → `res.users`
- `serializeFields()`: added `j["user_id"]`
- `deserializeFields()`: added `parseM2o(j, "user_id")`

**StockPickingFormView::fields():**
- Added `user_id` (many2one → res.users)
- Added `company_id` (many2one → res.company)

---

### 2. Backend — ProductModule.hpp

**New column migrations:**
```sql
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS expense_ok BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE product_product ADD COLUMN IF NOT EXISTS image_1920 TEXT;
```

**ProductProduct model:**
- Added `bool expenseOk = false;` and `std::string image1920;` members
- `registerFields()`: added `expense_ok` Boolean and `image_1920` Text
- `serializeFields()`: added both fields (image_1920 returns `false` if empty)
- `deserializeFields()`: added both fields

**ProductProductFormView::fields():**
- Added `expense_ok` Boolean
- Added `image_1920` Char

---

### 3. Frontend — TransferFormView (app.js)

**New state fields:**
- `users: []` — list of all active res.users for Responsible dropdown
- `locations: []` — list of all active stock.locations for editable src/dest selects
- `companyName: ''` — resolved from company_id on the picking

**load() changes:**
- Added `company_id` and `user_id` to the read fields list
- Parallel fetch: partners + users + locations all at once
- After record load: fetch company name via `res.company read [companyId]`

**Template changes:**

*Main info grid — Source Location and Destination:*
- When `isDraft`: renders `<select>` with all active locations (allows editing)
- Otherwise: renders read-only `<span>` with resolved location name (unchanged)

*Additional Info tab — Responsible:*
- Replaced `— (not implemented)` stub with a `<select>` populated from `state.users`
- `data-field="user_id"` — handled by existing `onAnyChange` (parseInt)

*Additional Info tab — Company:*
- Replaced hardcoded `"My Company"` with `state.companyName` (live from DB)

**onSave() changes:**
- Always writes `user_id` (from select)
- When `isDraft`: also writes `location_id` and `location_dest_id` (from selects)

---

### 4. Frontend — ProductFormView (app.js)

**New fields in load():**
- Added `expense_ok` and `image_1920` to `read` fields list
- New record defaults include `expense_ok: false, image_1920: false`

**Template — Can be Expensed checkbox:**
- Removed `disabled="1"` and `prd-check-disabled` class
- Now has `data-field="expense_ok"` and reacts to `onFormChange`
- Fully editable and saved

**Template — Product image:**
- If `state.record.image_1920` is set: renders `<img>` with `data:image/*;base64,...` src
- Otherwise: shows the camera icon placeholder
- Clicking the image box triggers a hidden `<input type="file">` via `triggerImageUpload()`
- `onImageChange(e)`: reads file via `FileReader.readAsDataURL()`, strips the prefix,
  stores base64 string in `state.record.image_1920` (reactive, triggers re-render)
- Image is saved via `onSave()` as `image_1920` field

**onSave() changes:**
- Added `expense_ok: !!r.expense_ok` and `image_1920: r.image_1920 || false`

**New methods:**
- `triggerImageUpload()` — finds and clicks the hidden file input via `this.el.querySelector`
- `onImageChange(e)` — FileReader-based base64 image loading
- `onViewMoves()` — calls `this.props.onNavigate('stock.move', [...])` if prop provided
  (ActionView provides a stub navigateTo that shows an informational alert)

---

### 5. Frontend — ActionView (app.js)

- Added `navigateTo(model, domain)` method (stub — shows informational alert)
- Passes `onNavigate.bind="navigateTo"` to `ProductFormView`
  (wiring Product Moves button to a real filtered list is planned for Phase 27+)

---

### 6. CSS — app.css

- `.prd-image-box`: removed `cursor: not-allowed; opacity: .5`, added `cursor: pointer; overflow: hidden`
- `.prd-image-box:hover`: added hover state (border highlight)
- `.prd-img-preview`: new class — `width: 100%; height: 100%; object-fit: contain; border-radius: 6px`

---

## What's still deferred

See `docs/015-transfer-form-deferred.md` and `docs/016-product-form-deferred.md` for full list.
Items remaining after Phase 17g:

| Item | Status | Phase |
|------|--------|-------|
| Transfer — Lot/Serial tracking | stub | Phase 28 |
| Transfer — Package tracking | stub | Phase 28/29 |
| Transfer — Put in Pack button | disabled | Phase 29 |
| Transfer — Chatter (real log) | static stub | Phase 27 |
| Product — Units On Hand (real) | 0 stub | Phase 29 (needs stock_quant) |
| Product — Forecasted | 0 stub | Phase 28 |
| Product — Reordering Rules stat | 0 stub | Phase 29 |
| Product — Bill of Materials stat | 0 stub | Phase 26 |
| Product — Putaway Rules stat | 0 stub | Phase 29 |
| Product — Product Moves navigation | alert stub | Phase 27+ |
| Product — Customer Taxes (M2M) | stub | Phase A1 |
| Product — Sales/Purchase/Inventory/Accounting tabs | disabled | Phase A3 |
| Product — Chatter (real log) | static stub | Phase 27 |

---

## Next: Phase 27 — Chatter / Audit Log

Create `mail_message` table, add `postLog()` to all 5 ViewModels,
implement shared ChatterPanel OWL component.
