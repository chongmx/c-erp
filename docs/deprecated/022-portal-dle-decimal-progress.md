# Portal & DLE Decimal Precision — Progress Log (2026-03-23 continued)

Build clean after all changes.

---

## 1. Customer Portal — Full Rework

**Files:** `web/static/portal.html`, `web/static/portal.js`, `modules/portal/PortalModule.hpp`

### portal.html — Generic cleanup

- Brand renamed: "Storage Rental Portal" → "Customer Portal"
- Login subtitle → generic text
- Removed "My Account" nav item and section entirely
- Renamed "Storage Plans" tab → "Products"
- Added **Orders** section (`#section-orders`) — 5-column table: Reference, Date, Amount, Status, Actions
- Added **Deliveries** section (`#section-deliveries`) — 5-column table: Reference, Origin, Date, Status, Actions

### portal.js — New sections + public API

- `showSection()` handles `['invoices', 'orders', 'deliveries', 'products']`
- Removed `changePassword()`, `showAlert()`, removed `account` from public API
- Removed "Monthly rental" / "per month" text from product cards
- **New functions:** `loadOrders()`, `renderOrders()`, `orderStateBadge()`, `openOrderDetail()`, `printOrder()`
- **New functions:** `loadDeliveries()`, `renderDeliveries()`, `deliveryStateBadge()`, `openDeliveryDetail()`, `printDelivery()`

### PortalModule.hpp — Same print format as backend + access control

All portal documents now render from the same `ir_report_template` HTML used by the backend admin — identical output, not a separate template.

**Helper functions added (before `PortalSessionManager`):**

```cpp
static std::string portalFmtMoney(double v)           // comma-thousands, 2dp
static std::string portalFmtMoneyF(const pqxx::field&)
static std::string portalFmtPrec(double v, int prec)  // precision-aware
static std::string portalFmtPrecF(const pqxx::field&, int prec)
static std::string portalSafeStr(const pqxx::field&)
static std::string portalYmdToDisplay(const std::string& ymd)
static std::string portalReplaceAll(...)
static std::string portalRenderTemplate(...)
static std::string portalRenderDoc(model, recordId, partnerId, txn)
```

**`portalRenderDoc`:**
- Loads template from `ir_report_template` (same table the backend uses)
- Reads `COALESCE(decimal_qty, 2)`, `decimal_price`, `decimal_subtotal` from template row
- Queries company/partner/bank details from `ir_config_parameter`
- Enforces `AND partner_id = $2` on all queries — customers cannot access other companies' records
- Handles `account.move`, `sale.order`, `stock.picking`

**New routes registered:**

| Route | Purpose |
|-------|---------|
| `GET /portal/api/invoices` | List customer's invoices |
| `GET /portal/api/invoice/{id}/detail` | Invoice detail JSON |
| `GET /portal/api/invoice/{id}/print` | Rendered HTML (from template) |
| `GET /portal/api/orders` | List customer's sales orders |
| `GET /portal/api/order/{id}/detail` | Order detail JSON |
| `GET /portal/api/order/{id}/print` | Rendered HTML |
| `GET /portal/api/deliveries` | List customer's delivery orders (outgoing only) |
| `GET /portal/api/delivery/{id}/detail` | Delivery detail JSON |
| `GET /portal/api/delivery/{id}/print` | Rendered HTML |

All routes enforce `WHERE ... AND partner_id = session->partnerId`.

---

## 2. Decimal Precision per Document Template

**Files:** `modules/report/ReportModule.hpp`, `modules/portal/PortalModule.hpp`, `web/static/src/app.js`

### DB schema — 3 new columns on `ir_report_template`

```sql
ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS decimal_qty      INTEGER NOT NULL DEFAULT 2;
ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS decimal_price    INTEGER NOT NULL DEFAULT 2;
ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS decimal_subtotal INTEGER NOT NULL DEFAULT 2;
```

### ReportModule.hpp

- `ensureSchema_()`: adds the 3 ALTER TABLE statements
- `handleSearchRead` / `handleRead`: both SELECT and serialise the 3 columns as integers
- `handleWrite`: reads and saves all 3 from `vals`; both UPDATE branches include `$5/$6/$7`
- Main render route SELECT: `COALESCE(decimal_qty, 2)` etc. — reads precision and applies to all 4 models
- Added `fmtPrec(double v, int prec)` and `fmtPrecF(pqxx::field, int prec)` helpers
- Removed `loadPrec` lambda (was reading from `ir_config_parameter` — wrong approach)

### PortalModule.hpp

- `portalRenderDoc` render SELECT: includes `COALESCE(decimal_*, 2)` columns
- `portalFmtPrecF` used for all numeric line item fields
- `account.move` SQL: removed `::TEXT` casts — fields must be numeric for `.as<double>()` to work

### app.js — DLE Properties panel

- `docSettings` state: added `decimal_qty: 2`, `decimal_price: 2`, `decimal_subtotal: 2`
- `onDocTypeChange`: `search_read` includes the 3 fields, assigns with `?? 2` fallback
- `onSave`: saves all 3 fields (see pitfall below)
- DLE template: "Precision (decimal digits)" section in Properties panel, visible only when `items_table` block selected, collapsible via `togglePropSect`/`isPropSectOpen`
- `ReportSettingsView`: removed decimal UI section and `CFG_KEYS` entries (no longer uses `ir_config_parameter`)

---

## 3. DLE Decimal — Bug Fixes

### Bug 1: `TypeError: vN is not a function` (OWL inline assignment)

**Root cause:** OWL 2.x compiles `t-on-change="ev => this.state.x = ..."` as an assignment expression, not a callable function.

**Fix:** Extract to named methods on the component class:

```js
setDecimalQty(ev)      { this.state.docSettings.decimal_qty      = parseInt(ev.target.value); this.rebuildHtml(); }
setDecimalPrice(ev)    { this.state.docSettings.decimal_price    = parseInt(ev.target.value); this.rebuildHtml(); }
setDecimalSubtotal(ev) { this.state.docSettings.decimal_subtotal = parseInt(ev.target.value); this.rebuildHtml(); }
```

Then in template: `t-on-change="setDecimalQty"` (not inline).

**Rule:** If the handler body is an assignment, extract to a named method. Simple method calls (no assignment) are fine inline.

### Bug 2: Precision section not collapsible

**Fix:** Use the same `togglePropSect` / `isPropSectOpen` mechanism as all other block property groups:

```xml
<div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
     t-on-click="()=>this.togglePropSect('Precision')">
    <span>Precision (decimal digits)</span>
    <span class="dle-acc-icon" t-esc="isPropSectOpen('Precision') ? '\u25BE' : '\u25B8'"/>
</div>
<t t-if="isPropSectOpen('Precision')">
    <!-- prop rows -->
</t>
```

### Bug 3: Preview not updating on change

**Fix:** `setDecimalXxx` methods call `this.rebuildHtml()`. `rebuildHtml` passes `this.state.docSettings` to `dleRenderPreview`. Added `dleFormatPrec()` helper and updated `dleRenderPreview` to format dummy line values with the correct decimal places:

```js
function dleFormatPrec(val, prec) {
    const n = parseFloat(String(val).replace(/,/g, ''));
    if (isNaN(n)) return val;
    return n.toLocaleString('en-US', { minimumFractionDigits: prec, maximumFractionDigits: prec });
}

function dleRenderPreview(templateHtml, model, settings) {
    const qtyPrec = settings?.decimal_qty      ?? 2;
    const prcPrec = settings?.decimal_price    ?? 2;
    const subPrec = settings?.decimal_subtotal ?? 2;
    // ... format ln.qty, ln.price_unit, ln.subtotal with respective prec
}
```

All `dleRenderPreview` call sites updated to pass `this.state.docSettings`.

### Bug 4: Setting value to 0 reverted to 2 on save

**Root cause:** `parseInt(0) || 2` evaluates to `2` because `0` is falsy.

**Fix:**
```js
// Before (wrong — treats 0 as falsy)
decimal_qty: parseInt(this.state.docSettings.decimal_qty) || 2,

// After (correct)
decimal_qty: Number.isInteger(this.state.docSettings.decimal_qty) ? this.state.docSettings.decimal_qty : 2,
```

---

## 4. Portal `/me` 401 Console Error Eliminated

**Files:** `web/static/portal.js`, `modules/portal/PortalModule.hpp`

### Problem

On every page load, `portal.js` called `GET /portal/api/me` to detect an existing session. When no session existed the server correctly returned 401 — but the browser always logs 4xx fetch responses to the console, creating noise after every login.

### Fix — sessionStorage replaces `/me` call

**`portal.js` changes:**

1. On successful login → `sessionStorage.setItem('portal_user', JSON.stringify(_user))`
2. On logout → `sessionStorage.removeItem('portal_user')`
3. `DOMContentLoaded`: reads from `sessionStorage` instead of calling `/me`:

```js
const stored = sessionStorage.getItem('portal_user');
if (stored) {
    try { _user = JSON.parse(stored); showApp(); return; } catch (_) {}
}
document.getElementById('login-screen').style.display = 'flex';
```

4. `api()` helper: if any authenticated data call returns 401 (session cookie expired), auto-clears storage and redirects to login screen:

```js
if (res.status === 401 && path !== '/login') {
    sessionStorage.removeItem('portal_user');
    _user = null;
    document.getElementById('portal-app').style.display = 'none';
    document.getElementById('login-screen').style.display = 'flex';
}
```

**`PortalModule.hpp`:** Removed the `GET /portal/api/me` route entirely (dead code).

### Behaviour

| Scenario | Before | After |
|----------|--------|-------|
| First visit (no session) | `GET /me` → 401 in console | Show login immediately, no server call |
| Page refresh (valid session) | `GET /me` → 200, show portal | Read `sessionStorage`, show portal instantly |
| Page refresh (expired cookie) | `GET /me` → 401, show login | First data call 401s → auto-redirect to login |
| Sign out | `/logout` + show login | `/logout` + clear `sessionStorage` + show login |

`sessionStorage` is tab-scoped and cleared when the browser tab closes, matching expected portal session lifetime.
