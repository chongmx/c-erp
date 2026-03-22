# Phase 19 — DLE Improvements, Security Assessment, Product Form

**Status:** ✅ Code complete — build clean
**Date:** 2026-03-22
**Build:** Clean

---

## Summary of work

Four independent areas of improvement were completed in this session:

1. Document Layout Editor (DLE) — layout, UX, and preview fixes
2. Security Assessment — full review of the frontend + backend, plan written
3. Report Template Mapping — critical bug fix (wrong template saved per model)
4. Product Form — product moves filter fix + Inventory tab implemented

---

## 1. Document Layout Editor (DLE)

### 1a. Shell height — JS-based detection (replaces CSS `height: 100%`)

**Problem:** `.dle-shell` was using `height: 100%` which expanded with accordion content instead of being fixed to the viewport.

**Fix — `app.js`:**
- Added `useRef` and `onWillUnmount` to OWL imports
- Added `this.shellRef = useRef('shell')` to DLE setup
- Added `_updateHeight()` that reads `el.getBoundingClientRect().top` and sets `el.style.height` to the remaining viewport height
- Called `_updateHeight()` on `onMounted` and bound to `window.addEventListener('resize', ...)`
- Cleaned up listener in `onWillUnmount`

**Fix — `app.css`:**
- Removed `height: 100%` from `.dle-shell`; comment notes height is set by JS

---

### 1b. Left panel — reverted to tabs (Blocks / HTML)

**Problem:** Left panel had been changed to an accordion; user requested revert to tabs.

**Fix — `app.js`:**
- Replaced accordion markup with a `div.dle-left-tabs` containing two `<button>` elements: "Blocks" and "HTML"
- State uses `leftTab: 'blocks'` instead of `leftOpen`
- Clicking "Blocks" tab also clears `state.selectedBlock = null` — this returns the right panel to "Document Settings" automatically without a separate back button
- `addBlock()` and `onSave()` updated to use `leftTab` instead of `leftOpen`
- "HTML" tab shows a popout button (↗) inline when active

---

### 1c. Accordion header color

The right-panel section headers (`.dle-acc-hdr`) were given a visually distinct background:

```css
.dle-acc-hdr {
    background: var(--border);   /* was var(--surface) — indistinguishable */
}
.dle-acc-hdr:hover { background: #1a3a6e; }
```

---

### 1d. Dummy data — section/note/product line items in preview

**Problem:** Preview was showing only description column for line items; section and note rows were displaying without data.

**Root cause:** `dleRenderPreview` ran the top-level `{{var}}` replacement *before* the `{{#each lines}}` loop, which blanked all inner template variables before they could be used.

**Fix — `app.js`, `dleRenderPreview`:**

Process `{{#each lines}}...{{/each}}` first, then replace top-level document variables:

```js
// 1. Process #each blocks FIRST
let html = templateHtml.replace(/\{\{#each lines\}\}([\s\S]*?)\{\{\/each\}\}/g, (_, tpl) =>
    (d.lines || []).map(ln => {
        if (ln.line_type === 'line_section')
            return `<tr class="row-line_section"><td colspan="${cols}">${ln.product_name || ''}</td></tr>`;
        if (ln.line_type === 'line_note')
            return `<tr class="row-line_note"><td colspan="${cols}">${ln.product_name || ''}</td></tr>`;
        return tpl.replace(/\{\{(\w+)\}\}/g, (__, k) => ln[k] || '');
    }).join('')
);
// 2. Then replace top-level vars
html = html.replace(/\{\{(\w+)\}\}/g, (_, k) => d[k] !== undefined ? d[k] : '');
```

**Updated `DLE_DUMMY`:** Each doc type now has realistic line items:
- `line_section` rows with section headers
- `product` rows with `qty`, `uom`, `price_unit`, `subtotal`
- `line_note` rows with note text

---

## 2. Security Assessment

**Input:** An external security agent report was reviewed against the actual codebase.

**Output:** `/home/user/code/c-erp/docs/security-assessment-plan.md` — comprehensive plan marking each finding as 🔴 Action required / 🟡 Partially mitigated / ✅ Irrelevant.

### Findings by section

| # | Finding | Status | Notes |
|---|---------|--------|-------|
| 1a | DLE block prop CSS injection | 🔴 | `sanitizePropValue()` helper planned |
| 1b | HTML editor / iframe no sandbox | 🔴 | Add `sandbox="allow-same-origin"` to preview iframe |
| 1c | ChatterPanel `t-raw` XSS | ✅ Irrelevant | Already using `t-esc` everywhere |
| 2a | Client-side auth gate | 🟡 | Standard SPA pattern; backend enforces on every request |
| 2b | Session ID in request body | 🟡 | In-memory only (not LocalStorage); console.log removed (see §3) |
| 2c | CSRF | 🟡 | Mitigated by `Content-Type: application/json`; Origin validation planned in backend |
| 3a | IDOR — client-supplied IDs | 🟡 | Backend ACL responsibility |
| 3b | Arbitrary model string | 🟡 | Backend `ir.model.access` responsibility |
| 4a | ALWAYS_SKIP missing sensitive fields | 🔴 | Expand set to include `standard_price`, `cost_price`, `password`, etc. |
| 4b | Hardcoded dummy data | ✅ Irrelevant | Generic placeholder data only |
| 4c | Global `RpcService` scope | 🟡 | Accepted risk; ES module migration deferred |
| 5a | RPC rate limiting | ✅ Irrelevant | Infrastructure/nginx concern |
| 5b | CSS injection via `url()` in style props | 🔴 | Same fix as 1a + CSP meta tag in `DLE_CSS` |
| 6a | SQL injection via domain values | ✅ Safe | All values via `pqxx::params` |
| 6b | SQL injection via field names | ✅ Safe | `sanitizeColumn_()` rejects non-alphanumeric |
| 6c | SQL injection via write field names | ✅ Safe | `fieldRegistry_.has(key)` validates all keys |
| 6d | DDL injection | ✅ Impossible | No DDL paths; `TABLE_NAME` is a compile-time constant |
| 6e | ORDER BY latent injection | 🟡 | Currently hardcoded `"id ASC"` — sanitizer planned before user-supplied order is ever used |
| 6f | Session cookie missing `Secure` flag | 🔴 | `c.setSecure(true)` missing in `JsonRpcDispatcher.hpp` |

### Immediate action taken

- Removed two `console.log` statements in `rpc.js` that printed the session ID to the browser console (security item 2b / 4c)

---

## 3. Report Template Mapping Bug Fix

### Problem

The Document Layout Editor always wrote to template `id=1` regardless of which document type was selected. Opening DLE for Invoice and saving modified the Sales Order template.

### Root cause — `ReportModule.hpp`, `ReportTemplateViewModel::handleSearchRead`

The `handleSearchRead` method was ignoring the incoming domain entirely, always returning the first template:

```cpp
// Before: domain ignored → always returns id=1 (first row)
rows = txn.exec("SELECT ... FROM ir_report_template WHERE active=true ORDER BY id");
```

### Fix

Extract the `model` field from the domain array and apply it as a `WHERE model=$1` filter:

```cpp
nlohmann::json handleSearchRead(const CallKwArgs& call) {
    std::string modelFilter;
    const auto& dom = call.domain();
    if (dom.is_array()) {
        for (const auto& leaf : dom) {
            if (leaf.is_array() && leaf.size() == 3 &&
                leaf[0].get<std::string>() == "model" &&
                leaf[1].get<std::string>() == "=" &&
                leaf[2].is_string()) {
                modelFilter = leaf[2].get<std::string>();
                break;
            }
        }
    }
    if (modelFilter.empty()) {
        rows = txn.exec("SELECT ... FROM ir_report_template WHERE active=true ORDER BY id");
    } else {
        rows = txn.exec(
            "SELECT ... FROM ir_report_template WHERE active=true AND model=$1 ORDER BY id LIMIT 1",
            pqxx::params{modelFilter});
    }
    // ... serialize
}
```

Each document type now loads and saves its own template correctly:
- `account.move` → Invoice template
- `sale.order` → Sales Order template
- `stock.picking` → Delivery Order template

---

## 4. Product Form — Moves Filter Fix + Inventory Tab

### 4a. Product Moves filter bug

**Problem:** Clicking "Product Moves" stat widget from the product form showed ALL stock moves instead of only moves for that product.

**Root causes — two issues found:**

**Issue 1 — `navigateTo` race condition (`app.js`, `ActionView`):**

`state.mode = 'list'` was set synchronously *before* the `await RpcService.getViews(...)` call. If OWL scheduled a render before the await resolved, `ListView` could mount with `currentAction = null → props.action` (the product.product action), not the stock.move action with the domain.

**Fix:** Moved `state.mode = 'list'` and `state.recordId = null` inside the `try` block, after the await:

```js
async navigateTo(model, domain) {
    this.state.loading = true;
    try {
        const result = await RpcService.getViews(model, [[false,'list'],[false,'form']]);
        this.state.listView       = result.views?.list || null;
        this.state.formView       = result.views?.form || null;
        this.state.overrideAction = { res_model: model, domain: domain || [] };
        this.state.recordId       = null;
        this.state.mode           = 'list';   // ← moved here
    } catch (e) { ... }
    finally { this.state.loading = false; }
}
```

`state.overrideAction` is now always set before `state.mode = 'list'` triggers the ListView mount. OWL's loading spinner (`state.loading = true`) keeps the UI hidden until all state is ready.

**Issue 2 — `ListView` missing `onWillUpdateProps` (`app.js`):**

`ListView.load()` was only called from `onMounted`. If the component stayed mounted between navigations (OWL reusing the instance), prop changes never triggered a reload.

**Fix:** Added `onWillUpdateProps` to `ListView.setup()`:

```js
onWillUpdateProps((np) => {
    const oa = this.props.action;
    const na = np.action;
    if (na?.res_model !== oa?.res_model ||
        JSON.stringify(na?.domain) !== JSON.stringify(oa?.domain)) {
        Promise.resolve().then(() => this.load());
    }
});
```

`Promise.resolve().then(...)` ensures `this.load()` runs after OWL has updated `this.props` to the new values.

Also added `onWillUpdateProps` to OWL imports:
```js
const { Component, useState, xml, mount, onMounted, useRef, onWillUnmount, onWillUpdateProps } = owl;
```

---

### 4b. Inventory tab — weight and volume

Fields `volume` and `weight` were already registered in the `ProductProduct` model but unused in the UI.

**Changes:**

- Added `'volume'` and `'weight'` to the `read` fields list in `ProductFormView.load()`
- Added them to new-record defaults: `volume: 0, weight: 0`
- Added them to `onSave` values dict
- Enabled the Inventory tab (removed `prd-tab-disabled` class, added click handler `setTab('inventory')`)
- Replaced `setTabGeneral()` with generic `setTab(tab)` method
- Added `typeLabel` getter (Consumable / Service / Storable Product)
- Implemented Inventory tab content:

```xml
<t t-if="state.activeTab === 'inventory'">
    <div class="so-info-grid">
        <div class="so-info-col">
            <!-- Weight (kg) — editable number input -->
            <!-- Volume (m³) — editable number input -->
        </div>
        <div class="so-info-col">
            <!-- Product Type — read-only label via typeLabel getter -->
            <!-- Tracking — stub "No Tracking" (lot/serial tracking deferred) -->
        </div>
    </div>
</t>
```

---

## What remains deferred

| Item | Notes |
|------|-------|
| `sanitizePropValue()` in DLE | Prevents CSS/XSS injection via block property values |
| `sandbox` attribute on DLE preview iframe | Prevents script execution in preview |
| CSP meta tag inside `DLE_CSS` | Blocks external `url()` loads even if value slips through |
| Expand `ALWAYS_SKIP` with sensitive field names | `standard_price`, `password`, etc. |
| `c.setSecure(true)` on session cookie | `JsonRpcDispatcher.hpp` line ~192 |
| `ORDER BY` sanitizer in `BaseModel.hpp` | Before any user-supplied order is accepted |
| On Hand / Forecasted quantities | Needs `stock_quant` table or move-aggregation query |
| Update Quantity / Replenish buttons | Needs stock.quant write and reordering rules |
| Sales tab content | No `description_sale` field in backend yet |
| Purchase tab content | No `description_purchase` / vendor pricelist yet |
| Accounting tab content | Income/expense account fields not implemented |
| Customer Taxes on product | `account_tax_ids` relation not implemented |
| Per-user author attribution in chatter | Session uid propagation into ViewModels |
