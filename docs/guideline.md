# C++ ERP Programming Guidelines

Last updated: 2026-03-23
This is the single authoritative reference for patterns, rules, and anti-patterns in this codebase.
It captures every bug and mistake encountered so they are not repeated.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [C++ Model Layer](#2-c-model-layer)
3. [ViewModel Layer](#3-viewmodel-layer)
4. [Authorization Rules](#4-authorization-rules)
5. [Security Checklist](#5-security-checklist)
6. [Frontend (OWL 2 IIFE)](#6-frontend-owl-2-iife)
7. [Database / SQL](#7-database--sql)
8. [Session Management](#8-session-management)
9. [Performance Rules](#9-performance-rules)
10. [Naming and ID Conventions](#10-naming-and-id-conventions)

---

## 1. Architecture Overview

```
HTTP → JsonRpcDispatcher → ViewModelFactory → ViewModel → BaseModel → DbConnection
                         ↘ ViewFactory (get_views)
```

- **IModule**: registers models, services, views, viewmodels, and routes
- **BaseModel<TDerived>**: CRTP ORM base; auto-registers fields via call_once
- **BaseViewModel**: dispatch table (REGISTER_METHOD macro)
- **JsonRpcDispatcher**: auth check → model access check → VM dispatch
- **SessionManager**: shared_mutex + two-phase get() (PERF-01)
- **ServiceFactory**: config bus — carries db, devMode, secureCookies, sessions

All state between requests lives in SessionManager. ViewModels are Transient (one per request).

---

## 2. C++ Model Layer

### CRTP Constructor — NEVER call registerFields() in derived constructor

`BaseModel<TDerived>` calls `registerFields()` automatically via `std::call_once`.
The derived constructor body must be EMPTY.

```cpp
// CORRECT
explicit StockMove(std::shared_ptr<DbConnection> db)
    : BaseModel<StockMove>(std::move(db)) {}

// WRONG — throws "FieldRegistry: duplicate field" at runtime
explicit StockMove(std::shared_ptr<DbConnection> db)
    : BaseModel<StockMove>(std::move(db)) {
    registerFields();  // ← NEVER DO THIS
}
```

### FieldRegistry is static per model type

`inline static FieldRegistry fieldRegistry_{}` + `std::call_once` — registered once per
template specialisation, not once per request. Do not add instance-level field registrations.

### normalizeForDb_ handles false → NULL and [id,"Name"] → id

`BaseModel::write()` and `create()` call `normalizeForDb_()` automatically.
Use `m2oToId_()` helper in `deserializeFields()` for Many2one fields:

```cpp
void deserializeFields(const nlohmann::json& j) override {
    if (const int v = m2oToId_(j, "partner_id")) partnerId = v;
}
```

### New columns on existing tables need ALTER TABLE migrations

`CREATE TABLE IF NOT EXISTS` only runs once. Adding a column later requires:
```cpp
txn.exec("ALTER TABLE my_table ADD COLUMN IF NOT EXISTS new_col INTEGER");
```
Always add this in `ensureSchema_()` alongside the column definition.

### product_template does not exist in this codebase

Product names live directly on `product_product.name`. Never JOIN to `product_template`.
Always use `pp.name AS product_name` from `product_product pp`.

---

## 3. ViewModel Layer

### REGISTER_METHOD macro — method dispatch

```cpp
REGISTER_METHOD("search_read", handleSearchRead)
REGISTER_METHOD("create",      handleCreate)
```

Both `search_read` and `web_search_read` should map to the same handler.
Both `read` and `web_read` should map to the same handler.

### handlers return plain records, NOT view-wrapped

`search_read` and `read` handlers return `[{id, field, ...}]` arrays directly.
NEVER wrap in `{arch, fields, record}` — that is for get_views, not data queries.

### views_.fields() must include all fields the frontend needs

`BaseView` subclasses return a hardcoded JSON from `fields()`. One2many fields (e.g.
`order_line`) must be explicitly included or the frontend never sees them. `fields()` is
NOT auto-generated from the model registry.

### List views require a registered View class

Every model shown in the sidebar needs both a list AND form view registered in
`registerViews()`:
```cpp
views_.registerView<MyModelListView>("my.model.list");
views_.registerView<MyModelFormView>("my.model.form");
```
Without a registered list view, `get_views` returns `fields: {}` and the ListView renders
zero columns.

### ViewModel authorization — resolve session from ServiceFactory

Any ViewModel that checks authorization needs the shared SessionManager from ServiceFactory:

```cpp
// In AuthModule::registerViewModels():
viewModels_.registerCreator("res.users", [&sf, db] {
    auto sessions = sf.sessions();  // ← shared store, NOT make_shared<SessionManager>()
    return std::make_shared<AuthViewModel>(auth, sessions, db, db->config().name);
});
```

**NEVER** do `auto sessions = std::make_shared<infrastructure::SessionManager>()` inside a
lambda — this creates an empty store per request; all session lookups return nullopt.

Resolve the caller's session in the handler:
```cpp
infrastructure::Session callerSession_(const core::CallKwArgs& call) const {
    const std::string sid = call.kwargs.contains("context")
        ? call.kwargs["context"].value("session_id", std::string{})
        : std::string{};
    if (!sid.empty()) {
        auto s = sessions_->get(sid);
        if (s) return *s;
    }
    return infrastructure::Session{};
}
```

---

## 4. Authorization Rules

### Model-level access — checkModelAccess_() in JsonRpcDispatcher

When adding a new model with sensitive data, add it to the static map in
`JsonRpcDispatcher::checkModelAccess_()`:

```cpp
static const std::unordered_map<std::string, int> kRequired = {
    {"account.move",   5},  // ACCOUNT_BILLING
    {"hr.employee",   15},  // HR_EMPLOYEE
    // add new model here
    {"my.model",      11},  // INVENTORY_USER (example)
};
```

Group ID constants are in `modules/auth/Groups.hpp`.
**Admins bypass all checks.** Non-members get std::runtime_error → HTTP 200 with JSON error.

### ViewModel write handlers — three rules

1. **create / unlink**: require `isAdmin || hasGroup(SETTINGS_CONFIGURATION)` for sensitive models
2. **write**: non-admin can only write their own record (`ids == [session.uid]`)
3. **password**: NEVER accept `password` in a generic `write()`; require a dedicated
   `change_password` handler that validates the old password first

### Self-deletion guard

In any `handleUnlink` for user records:
```cpp
for (int id : call.ids())
    if (id == session.uid)
        throw std::runtime_error("Cannot delete your own user account");
```

### Portal routes bypass HttpServer helpers

`PortalModule` registers routes directly with `drogon::app().registerHandler()`.
Every portal route lambda MUST:
1. Capture `devMode` and `secureCookies` from `devMode_` / `secureCookies_` members
2. All catch blocks: `devMode ? e.what() : "An internal error occurred"`
3. Log: `LOG_ERROR << "[portal] " << e.what()` in every catch
4. Any cookie: `if (secureCookies) cookie.setSecure(true)`

---

## 5. Security Checklist

### File uploads (SEC-16/SEC-19)

Before calling file.saveAs():
1. Size: `file.fileLength() > 10 * 1024 * 1024` → reject 400
2. Basename: strip all `/` and `\` path components via `rfind`
3. Extension: lowercase, must be in allowlist: `.pdf`, `.jpg`, `.jpeg`, `.png`
4. Save path: `data/upload_dir/{id}_{timestamp}_{baseName}`
5. Store `baseName` in DB, not raw filename

### SQL — qualify column names in JOINs

When using LEFT JOIN that introduces a column with the same name as a WHERE clause column,
always qualify:
```sql
-- BAD: ambiguous when res_partner is joined twice
WHERE company_id = $1

-- GOOD
WHERE rp.company_id = $1
```

### IrModule seeds — use ON CONFLICT, not COUNT guard

`seedActions_()` and `seedMenus_()` use `ON CONFLICT (id) DO NOTHING`.
Do NOT add `IF (COUNT(*) > 0) RETURN` early-return guards — they prevent new seeds from
being added when an older partial seed exists.

### WebSocket — validate session before accepting connection

`BusController::handleNewConnection` validates `session_id` cookie via `SessionManager`.
Unauthenticated connections are rejected:
```cpp
conn->send(error_json);
conn->shutdown();
return;
```

---

## 6. Frontend (OWL 2 IIFE)

### Event delegation inside t-foreach — mandatory pattern

Named methods in `t-on-*` inside `t-foreach` loops cannot resolve in the OWL 2 IIFE build.

```xml
<!-- WRONG — method not found at runtime -->
<t t-foreach="lines" t-as="line" t-key="line.id">
    <input t-on-change="onLineChange"/>
</t>

<!-- CORRECT — delegation on parent, data-* on children -->
<div t-on-change="onTableChange">
    <t t-foreach="lines" t-as="line" t-key="line.id">
        <input data-line-field="qty" data-key="line.id"/>
    </t>
</div>
```

Handler reads `e.target.dataset.*` to dispatch.

### Avoid && in XML templates

`&&` in `t-if` becomes `&amp;&amp;` which breaks XML parsing.

```js
// WRONG in template:  t-if="isDraft && hasLines"
// CORRECT — use a getter:
get canConfirm() { return this.isDraft && this.hasLines; }
// Template: t-if="canConfirm"
```

### Script files share global scope — wrap in IIFE

Multiple `<script>` tags share global scope. New component files must be wrapped:
```js
const MyComponent = (() => {
    const { Component, useState, xml } = owl;
    class MyComponent extends Component { ... }
    return MyComponent;
})();
```
Never declare `const { Component, ... } = owl` at file top-level in a second script file.

### Define child components before parent

`static components = { Foo }` uses `Foo` at class definition time.
Define child components (InvoiceFormView, SaleOrderFormView, etc.) BEFORE the parent
(ActionView) that references them in `static components`.

### complete_name does not exist on product.product

This codebase has no `complete_name` on `product.product`.
Always use only `['id', 'name']` when loading product options.

---

## 7. Database / SQL

### json::items() — store fields() result in a named local

```cpp
// WRONG — temporary destroyed before loop
for (auto& [k,v] : view->fields().items()) ...

// CORRECT — named local keeps json alive
auto flds = view->fields();
for (auto& [k,v] : flds.items()) ...
```

### exec() not exec_params()

libpqxx API: use `exec(query, pqxx::params{...})` not `exec_params(query, pqxx::params{...})`.
`exec_params` is deprecated and generates build warnings.

### Idempotent seeds — always ON CONFLICT

```sql
INSERT INTO ir_ui_menu (id, name, ...) VALUES (...)
ON CONFLICT (id) DO NOTHING;
```

Never use `IF NOT EXISTS` for seed data logic — use ON CONFLICT.

---

## 8. Session Management

### SessionManager — shared_mutex + two-phase get()

`get()` uses shared lock (fast path) for age < kTouchInterval_ (60s).
Upgrades to exclusive lock only when accessedAt needs refreshing.

**NEVER** create a new `SessionManager` inside a ViewModel factory lambda.
The shared instance lives in `ServiceFactory::sessions_` and is passed to Container.

### Session enrichment on login

After authenticate(), the session is enriched with:
- `isAdmin` — checked via `EXISTS(SELECT 1 FROM res_groups_users_rel WHERE uid=$1 AND gid=3)`
- `groupIds` — all gids from `res_groups_users_rel WHERE uid=$1`

Both are stored in the SessionManager via `update()`. Subsequent requests resolve them
from `callerSession_()` → `sessions_->get(sid)`.

### PortalSessionManager — same patterns as SessionManager

Both use `shared_mutex` + two-phase `get()` + `kTouchInterval_`.
Any future session store must follow the same pattern.

---

## 9. Performance Rules

### Rate limiter prune — throttle with lastPrune_

```cpp
// WRONG — O(n) on every allow() call
prune_(now);

// CORRECT — at most once per window
if ((now - lastPrune_) >= std::chrono::seconds(kWindowSeconds)) {
    prune_(now);
    lastPrune_ = now;
}
```

Add `Clock::time_point lastPrune_ = Clock::now()` to every rate limiter.

### Session ID hex — use lookup table

```cpp
// WRONG — ostringstream has locale+allocation overhead
std::ostringstream ss;
ss << std::hex << std::setfill('0');
for (unsigned char b : buf) ss << std::setw(2) << (unsigned)b;

// CORRECT
static constexpr char kHex[] = "0123456789abcdef";
std::string id; id.reserve(32);
for (unsigned char b : buf) { id += kHex[b >> 4]; id += kHex[b & 0x0f]; }
```

### ServiceFactory is the config bus

Add new config values to `ServiceFactory` — do NOT read env vars or files inside
module constructors. Current config carried:
- `db()` — database connection
- `devMode()` — whether to expose exception details
- `secureCookies()` — whether to set Secure flag on cookies
- `sessions()` — the shared SessionManager

---

## 10. Naming and ID Conventions

### IR Action Window IDs (ir_act_window)

```
1–21   core (contacts, accounting, product, sales, purchase, HR, stock)
22–25  stock special (receipts, deliveries, internal, warehouses)
26     Lots/Serial Numbers (Phase 28)
27     Reordering Rules (Phase 29)
28     Putaway Rules (Phase 29)
29     ir.mail_server (Phase 17f)
34     Bills of Materials ✅
35–36  MRP production + work centers (Phase 26-MRP)
37+    CRM, Project, HR Leave, Payroll, etc.
```

### IR Menu IDs (ir_ui_menu)

```
10–17  Accounting
20–21  Contacts
30–34  Settings
50–54  Products
60–64  Sales
70–72  Purchase
80–91  Employees
90–102 Inventory
100+   CRM, Project, Calendar, Fleet, Manufacturing
```

Never reuse an ID. Check `026-ir-id-registry-and-checklist.md` before assigning new IDs.

### Group IDs (Groups.hpp)

```
1  BASE_PUBLIC          2  BASE_INTERNAL       3  BASE_ADMIN
4  SETTINGS_CONFIGURATION
5  ACCOUNT_BILLING      6  ACCOUNT_MANAGER
7  SALES_USER           8  SALES_MANAGER
9  PURCHASE_USER       10  PURCHASE_MANAGER
11 INVENTORY_USER      12  INVENTORY_MANAGER
13 MRP_USER            14  MRP_MANAGER
15 HR_EMPLOYEE         16  HR_MANAGER
```

Use `Groups::` constants (not bare integers) when checking group membership in C++ code.
Use bare integers only in `checkModelAccess_()` which has inline comments mapping them.

### Model naming

- C++ class: `PascalCase` (e.g., `StockPicking`)
- Odoo model name: `snake.dot` (e.g., `"stock.picking"`)
- SQL table: `snake_underscore` (e.g., `"stock_picking"`)
- View key: `"model.name.list"` / `"model.name.form"`
- ViewModel key: same as model name `"stock.picking"`

---

## Quick Reference — Adding a New Module

1. Create `modules/mymod/MyModule.hpp`
2. Implement `IModule`: `registerModels`, `registerServices`, `registerViews`, `registerViewModels`, `registerRoutes`, `ensureSchema_`, `seed*()`
3. Schema: `CREATE TABLE IF NOT EXISTS` + `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` for any post-creation columns
4. Seeds: use `ON CONFLICT (id) DO NOTHING`, no COUNT guards
5. If model has sensitive data: add to `checkModelAccess_()` map in JsonRpcDispatcher
6. If ViewModel needs session: capture `sf.sessions()` from ServiceFactory
7. Register list + form views in `registerViews()`
8. Add ir_act_window + ir_ui_menu entries in `seedActions_()` / `seedMenus_()`
9. Add module to `main.cpp` with `container.addModule<MyModule>()`
10. If adding a portal route: capture `devMode_` + `secureCookies_`; apply to all catch blocks and cookies
