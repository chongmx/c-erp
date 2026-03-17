# Port Plan

Reference: `zzref/odoo/` (Odoo 19.0 source)
Target: C++ port in `modules/` using Drogon + libpqxx

---

## Completed

### Infrastructure (`core/`)
- DI Container, factories (Model/Service/View/ViewModel/Module), interfaces
- `DbConnection` pool, `HttpServer` (Drogon), `SessionManager`, `JsonRpcDispatcher`, `WebSocketServer`
- `BaseFactory` preserves insertion order — deterministic boot sequence
- `BaseModel<T>` CRTP ORM: `create`, `read`, `write`, `unlink`, `searchRead`, `fieldsGet`, domain filter

### Base Module (`modules/base/`)
Reference: `zzref/odoo/odoo/addons/base/models/res_partner.py`
- `ResPartner` + `PartnerService` + `PartnerFormView` + `PartnerViewModel`
- Schema: `res_partner` — id, name, email, phone, is_company, company_id, active
- `GenericViewModel.hpp` — shared CRTP ViewModel template for simple CRUD modules
→ See `docs/base-module-progress.md`

### Auth Module (`modules/auth/`)
Reference: `zzref/odoo/odoo/addons/base/models/res_users.py`, `res_groups.py`, `res_company.py`
- `ResUsers`, `ResGroups`, `ResCompany` models
- `AuthService` — PBKDF2-SHA512 (passlib-compatible), constant-time compare
- `AuthViewModel` — `authenticate`, `logout`, `read`, `fields_get`, `change_password`
- Session: `SessionManager` stores uid, login, db, context per cookie token
- Schema: `res_users`, `res_groups`, `res_company`, `res_groups_users_rel`
- Seeds: 3 default groups + admin user on first boot
→ See `docs/auth-module-progress.md`

### Frontend (`web/static/`)
- OWL app with login gate, `LoginPage`, `UserMenu`, session restore on reload
- Odoo 14-style navigation: home screen app tiles → horizontal top nav with dropdowns
→ See `docs/ui-redesign-progress.md`

---

## What's Already Good: The ORM

The C++ `BaseModel<T>` covers the core ORM operations Odoo uses most:
- `search_read`, `read`, `write`, `create`, `unlink`, `fields_get`, `search_count`
- `FieldRegistry` with type metadata (Char, Integer, Boolean, Many2one, etc.)
- Domain filter compilation to SQL WHERE clauses

**What the ORM is NOT yet doing** (needed for later modules):
- Computed fields (derived at query time — needs a compute callback system)
- Related fields (`related='partner_id.name'` — denormalized reads)
- Onchange triggers (for UI live-update)
- Access rules / record rules (row-level security)
- `@api.depends` invalidation cache

These are not needed immediately — skip until account module requires them.

---

## The Missing Layer: IR (Information Repository)

Odoo's OWL webclient cannot render anything after login without the **IR layer**.
After `authenticate` succeeds, the webclient immediately calls:

```
GET  /web/session/get_session_info   → who am I, what companies, what currencies
POST /web/action/load                → what is my home action (dashboard)?
POST /web/dataset/call_kw            → ir.ui.menu.load_menus()  → sidebar
POST /web/dataset/call_kw            → ir.ui.view.get_views()   → form/list layouts
POST /web/dataset/call_kw            → ir.model.fields_get()    → field metadata
```

Without these, the client gets errors and shows a blank page.
The **custom frontend** (`web/static/`) bypasses this — it calls `call_kw` directly
with known model names. So IR stubs are needed only when targeting the real Odoo client.

---

## Next Steps (priority order)

### ✅ Phase 3 — Base Module Completion (COMPLETE — see base-module-progress.md)
**Why first:** these are FK targets for nearly every other model.

Reference files:
- `zzref/odoo/odoo/addons/base/models/res_lang.py`
- `zzref/odoo/odoo/addons/base/models/res_currency.py`
- `zzref/odoo/odoo/addons/base/models/res_country.py`

**3a. `res_lang`** (simplest, no deps)
```sql
CREATE TABLE IF NOT EXISTS res_lang (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    code        VARCHAR NOT NULL UNIQUE,   -- 'en_US', 'fr_FR'
    iso_code    VARCHAR,                   -- 'en', 'fr'
    url_code    VARCHAR NOT NULL,
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    direction   VARCHAR NOT NULL DEFAULT 'ltr',
    date_format VARCHAR NOT NULL DEFAULT '%m/%d/%Y',
    time_format VARCHAR NOT NULL DEFAULT '%H:%M:%S'
)
```
Seed: one row for `en_US`.

**3b. `res_currency`** (no FK deps)
```sql
CREATE TABLE IF NOT EXISTS res_currency (
    id             SERIAL PRIMARY KEY,
    name           VARCHAR(3) NOT NULL UNIQUE,  -- 'USD', 'EUR'
    symbol         VARCHAR NOT NULL,
    position       VARCHAR NOT NULL DEFAULT 'after',  -- 'before' | 'after'
    rounding       NUMERIC NOT NULL DEFAULT 0.01,
    decimal_places INTEGER NOT NULL DEFAULT 2,
    active         BOOLEAN NOT NULL DEFAULT TRUE
)
```
Seed: USD + EUR.

**3c. `res_country` + `res_country_state`** (currency FK)
```sql
CREATE TABLE IF NOT EXISTS res_country (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR NOT NULL,
    code        VARCHAR(2) NOT NULL UNIQUE,  -- ISO 3166-1 alpha-2
    currency_id INTEGER REFERENCES res_currency(id),
    phone_code  INTEGER
)
CREATE TABLE IF NOT EXISTS res_country_state (
    id         SERIAL PRIMARY KEY,
    country_id INTEGER NOT NULL REFERENCES res_country(id),
    name       VARCHAR NOT NULL,
    code       VARCHAR NOT NULL
)
```
Seed: a handful of common countries (US, GB, DE, FR, etc.) — full ISO list not needed now.

**3d. Extend `res_partner`** — add FK columns
```sql
ALTER TABLE res_partner
    ADD COLUMN IF NOT EXISTS street      VARCHAR,
    ADD COLUMN IF NOT EXISTS city        VARCHAR,
    ADD COLUMN IF NOT EXISTS zip         VARCHAR,
    ADD COLUMN IF NOT EXISTS country_id  INTEGER REFERENCES res_country(id),
    ADD COLUMN IF NOT EXISTS state_id    INTEGER REFERENCES res_country_state(id),
    ADD COLUMN IF NOT EXISTS lang        VARCHAR REFERENCES res_lang(code);
```

**3e. Extend `res_company`** — add currency FK, link partner
Currently `res_company` has no `partner_id`. Odoo's company is always backed by a partner.
```sql
ALTER TABLE res_company
    ADD COLUMN IF NOT EXISTS partner_id  INTEGER REFERENCES res_partner(id),
    ADD COLUMN IF NOT EXISTS currency_id INTEGER REFERENCES res_currency(id);
```

All these extensions go in `BaseModule::ensureSchema_()` using `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` — fully idempotent.

---

### ✅ Phase 4 — session_info Endpoint Hardening (COMPLETE)
Reference: `zzref/odoo/addons/web/models/ir_http.py` → `session_info()`

The current `GET /web/session/get_session_info` returns minimal JSON.
Odoo's webclient expects this shape:

```json
{
  "uid": 1,
  "is_admin": true,
  "is_system": true,
  "user_context": { "lang": "en_US", "tz": "UTC", "uid": 1 },
  "db": "odoo",
  "name": "Administrator",
  "username": "admin",
  "partner_id": 1,
  "server_version": "19.0",
  "user_companies": {
    "current_company": 1,
    "allowed_companies": {
      "1": { "id": 1, "name": "My Company", "sequence": 1,
             "child_ids": [], "parent_id": false }
    }
  },
  "currencies": {
    "USD": { "symbol": "$", "position": "before", "digits": [12, 2] }
  }
}
```

**Status: COMPLETE.** All fields implemented. `currencies` dict queries `res.currency` via `vmFactory_` in `JsonRpcDispatcher::handleGetSessionInfo_`. `name`, `partner_id`, `company_id`, `is_admin`, `user_companies` all populated from session after `authenticate`.

---

### ✅ Phase 5 — IR Stubs (COMPLETE — see ir-module-progress.md)
Reference: `zzref/odoo/odoo/addons/base/models/ir_model.py`, `ir_ui_view.py`, `ir_ui_menu.py`

These are **not needed** for the custom frontend. Port them when targeting full Odoo client compatibility.

**5a. `ir.model`** — reflects registered C++ models
- `search_read`: return list of registered models from `ModelFactory`
- `fields_get`: delegate to `FieldRegistry` of the named model
- Schema: no DB table needed — derive from factory at runtime

**5b. `ir.ui.menu`** — sidebar navigation
Schema:
```sql
CREATE TABLE IF NOT EXISTS ir_ui_menu (
    id        SERIAL PRIMARY KEY,
    name      VARCHAR NOT NULL,
    parent_id INTEGER REFERENCES ir_ui_menu(id),
    sequence  INTEGER NOT NULL DEFAULT 10,
    action    VARCHAR,   -- 'ir.actions.act_window,<id>'
    web_icon  VARCHAR,
    active    BOOLEAN NOT NULL DEFAULT TRUE
)
```
Method: `load_menus()` — returns nested dict of all visible menus.
Seed: minimal menu tree pointing at `res.partner` and `res.users`.

**5c. `ir.ui.view`** — view architectures
- `get_views(views, options)` — returns `{arch, fields, model}` per view type
- Can be backed by C++ `ViewFactory` — serialize each registered `IView::arch()` + `fields()` as JSON
- No DB table needed initially — generate from registered views

**5d. `ir.actions.act_window`**
Schema:
```sql
CREATE TABLE IF NOT EXISTS ir_act_window (
    id        SERIAL PRIMARY KEY,
    name      VARCHAR NOT NULL,
    res_model VARCHAR NOT NULL,
    view_mode VARCHAR NOT NULL DEFAULT 'list,form',
    domain    VARCHAR,
    context   VARCHAR,
    target    VARCHAR NOT NULL DEFAULT 'current'
)
```
Method: `/web/action/load` — returns action dict for a given id.
Seed: one action per registered model (partner, users, etc.).

---

### ✅ Phase 6 — Account Module (COMPLETE — see account-module-progress.md)
Reference: `zzref/odoo/addons/account/models/`

Depends on: Phase 3 (res_currency, res_country), Phase 4 (session_info).

Port in dependency order:

| Step | Model | Key fields | Depends on |
|------|-------|-----------|------------|
| 6a | `account.account` | code, name, account_type, currency_id | res_currency |
| 6b | `account.journal` | name, code, type, currency_id, company_id | account.account, res_company |
| 6c | `account.tax` | name, amount, amount_type, tax_group_id | res_company |
| 6d | `account.move` | name, date, journal_id, partner_id, state | account.journal, res_partner |
| 6e | `account.move.line` | move_id, account_id, debit, credit, partner_id | account.move, account.account |
| 6f | `account.payment` | partner_id, amount, currency_id, journal_id | account.journal |
| 6g | `account.payment.term` | name, line_ids | — |

Each step: schema in `AccountModule::initialize()`, model class, service with CRUD + domain search, viewmodel registered with `ViewModelFactory`.

---

### ✅ Phase 8 — UOM + Product Modules (COMPLETE — see uom-product-progress.md)
- `uom.uom` — 1 table, 15 seeds (Units, kg, L, Hours, m, …)
- `product.category` — 3 seeds (All, Goods, Services)
- `product.product` — single-model (no template/variant split)
- `GenericViewModel<T>` in `modules/base/GenericViewModel.hpp`
- Products app tile (id=50) + Configuration section (id=52) in menu hierarchy

---

### ✅ Frontend UI Redesign (COMPLETE — see ui-redesign-progress.md)
- HomeScreen with colored app tiles
- AppTopNav with section buttons + dropdown menus
- 3-level `ir_ui_menu` hierarchy replacing flat list
- Old flat menu rows (id < 10) auto-deleted on boot

---

### ✅ Phase 9 — Sale Module (COMPLETE — see sale-module-progress.md)
Reference: `zzref/odoo/addons/sale/models/sale_order.py`

- `SaleOrder` + `SaleOrderLine` models
- `SaleOrderViewModel` — `action_confirm` (SO/YYYY/NNNN sequence), `action_cancel`, `action_create_invoices`
- `SaleOrderLineViewModel` — amount recomputation on create/write; UPDATE parent order totals
- IR: `ir_act_window` id=11, `ir_ui_menu` id=60 (Sales app tile), id=61 (Orders section), id=62 (Sales Orders leaf)
- Sequence: `sale_order_seq` for SO/YYYY/NNNN naming
- tax_ids stored as JSON text `'[]'`

---

### ✅ Phase 10 — Purchase Module (COMPLETE — see purchase-module-progress.md)
Reference: `zzref/odoo/addons/purchase/models/purchase_order.py`

- `PurchaseOrder` + `PurchaseOrderLine` models
- `PurchaseOrderViewModel` — `action_confirm` (PO/YYYY/NNNN sequence), `action_cancel`, `action_create_bills`
- `PurchaseOrderLineViewModel` — amount recomputation on create/write; UPDATE parent order totals
- IR: `ir_act_window` id=12, `ir_ui_menu` id=70 (Purchase app tile), id=71 (Orders section), id=72 (Purchase Orders leaf)
- Sequence: `purchase_order_seq` for PO/YYYY/NNNN naming
- Expense debit + AP credit move lines on bill creation

---

### ✅ Phase 12 — HR Module (COMPLETE — see hr-module-progress.md)
Reference: `zzref/odoo/addons/hr/models/`

- `ResourceCalendar`, `HrDepartment`, `HrJob`, `HrEmployee` models
- All use `GenericViewModel<T>` — no custom actions needed
- Circular FK (`hr_department.manager_id ↔ hr_employee`) resolved with `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`
- Seed: "Standard 40 hours/week" working schedule
- IR: app tile id=80, Employees/Departments direct leaves, Configuration section with Jobs + Working Schedules

---

### Phase 13 — auth_signup (DEFERRED — requires email infrastructure)
Reference: `zzref/odoo/addons/auth_signup/`
Depends on: `ir_config_parameter` stub (key-value store), email sending stub.

- Add `signup_token`, `signup_expiration` columns to `res_partner`
- `/web/signup` POST endpoint — create partner + user from form
- `/web/reset_password` POST — validate token, update password
- `ir_config_parameter` as a simple `key→value` table (no full ORM needed)

---

## Architecture Notes

### Boot Ordering Rule
Modules in `main.cpp` must be registered in dependency order.
`BaseFactory::registeredNames()` preserves insertion order (fixed).
`base` always before `auth`; `auth` always before `account`; etc.

### Schema Convention
- Every module owns its own `ensureSchema_()` called from `initialize()`
- All DDL uses `CREATE TABLE IF NOT EXISTS` and `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`
- Safe to run on every boot — fully idempotent

### Model Naming
`ResPartner` → `"res.partner"` → table `res_partner`
Wired via `ODOO_MODEL(model_name, table_name)` macro in `BaseModel<T>`.

### JSON-RPC Dispatch
`/web/dataset/call_kw` → `JsonRpcDispatcher` → `ViewModelFactory` → `IViewModel::callKw()`
ViewModels register handlers with `REGISTER_METHOD(name, handler)`.
Session auth checked by dispatcher for all methods except the bypass list.

### ORM Extension Points (for later)
When account/sale modules need them, add to `BaseModel<T>`:
- `compute(field, fn)` — register a computed field resolver
- `related(field, path)` — register a related field path
- `onchange(field, fn)` — register onchange trigger
These can be lazy additions — only implement when a concrete model requires them.
