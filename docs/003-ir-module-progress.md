# IR Module Progress

## Summary
Implemented the Odoo IR (Information Repository) layer. The frontend is now database-driven: menus load from `ir.ui.menu`, actions load from `ir.actions.act_window`, and views (arch + fields) are served from the C++ ViewFactory. Phase 5 is complete.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New Files

| File | Purpose |
|------|---------|
| `modules/ir/IrModule.hpp` | IR models, viewmodels, and module class |

---

## Work Completed

### New Models (in `modules/ir/IrModule.hpp`)

| Model | C++ Class | Table | Fields |
|-------|-----------|-------|--------|
| `ir.ui.menu` | `IrUiMenu` | `ir_ui_menu` | name, parent_id, sequence, action_id, web_icon, active |
| `ir.actions.act_window` | `IrActWindow` | `ir_act_window` | name, res_model, view_mode, domain, context, target, path, help |
| `ir.model` | `IrModelViewModel` | _(no table — in-memory)_ | model, name (from ModelFactory) |

### `IrMenuViewModel` — `ir.ui.menu`
Handles:
- `load_menus(debug)` — returns flat dict keyed by id (string) + `"root"` sentinel, Odoo 19 webclient format:
  ```json
  {
    "root": {"id": false, "name": "root", "children": [1, 2, 3]},
    "1": {"id": 1, "name": "Contacts", "app_id": 1,
          "action_model": "ir.actions.act_window", "action_id": 1,
          "web_icon": false, "children": [], "xmlid": "", "action_path": false}
  }
  ```
- `search_read`, `read`, `fields_get`

### `IrActWindowViewModel` — `ir.actions.act_window`
Handles `read`, `search_read`, `fields_get`.

### `IrModelViewModel` — `ir.model`
In-memory: `search_read` returns all registered models from `ModelFactory`, `fields_get` returns model/name field metadata.

### New HTTP Endpoints (in `JsonRpcDispatcher`)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `POST /web/action/load` | — | Load action dict by id. Body: `{params: {action_id: N}}` |
| `POST /web/action/load_breadcrumbs` | — | Stub returning `[]` |

#### Action dict shape (`/web/action/load`)
```json
{
  "id": 1,
  "type": "ir.actions.act_window",
  "name": "Contacts",
  "display_name": "Contacts",
  "res_model": "res.partner",
  "view_mode": "list,form",
  "views": [[false, "list"], [false, "form"]],
  "target": "current",
  "context": "{}",
  "domain": false,
  "path": "contacts"
}
```

### `get_views` — ViewFactory-backed (in `JsonRpcDispatcher`)
Intercepted before ViewModel dispatch when `method == "get_views"`.
Looks up views from `ViewFactory` by `"model.viewtype"` key:
```json
{
  "views": {
    "list": {"id": 0, "type": "list", "model": "res.partner",
             "arch": "<list>...</list>", "fields": {...}},
    "form": {"id": 0, "type": "form", "model": "res.partner",
             "arch": "<form>...</form>", "fields": {...}}
  },
  "models": {
    "res.partner": {"fields": {...}}
  }
}
```

### New View: `PartnerListView` (in `BaseModule.hpp`)
Registered as `"res.partner.list"`. Arch:
```xml
<list>
  <field name="name"/>
  <field name="email"/>
  <field name="phone"/>
  <field name="is_company"/>
</list>
```

### Seed Data

`initialize()` seeds (idempotent — skips if rows already exist):

**`ir_act_window`:**

| id | name | res_model | view_mode | path |
|----|------|-----------|-----------|------|
| 1 | Contacts | res.partner | list,form | contacts |
| 2 | Users | res.users | list,form | users |
| 3 | Companies | res.company | list,form | companies |

**`ir_ui_menu`:**

| id | name | action_id |
|----|------|-----------|
| 1 | Contacts | 1 |
| 2 | Users | 2 |
| 3 | Companies | 3 |

---

## Frontend Changes

The custom OWL frontend is now IR-driven. Boot sequence:
1. `GET /web/session/get_session_info` → restore session
2. `ir.ui.menu.load_menus()` → build sidebar dynamically
3. Menu click → `POST /web/action/load?action_id=N` → get action dict
4. `MODEL.get_views([[false,'list'],[false,'form']])` → load view descriptors
5. `MODEL.search_read()` → fetch records for list view
6. Row click → `MODEL.read([id])` → load form view

Removed hardcoded pages (Dashboard, PartnerList, FieldsInspector).
Added generic `ListView` and `FormView` OWL components backed by arch/fields from ViewFactory.

### New components in `web/static/src/app.js`
- `ListView` — renders tabular list from `get_views.list` + `search_read`. New/row-click navigation.
- `FormView` — renders editable form from `get_views.form` + `read`. Save/Create/Delete/Back.
- `ActionView` — orchestrates list↔form switching, calls `get_views` on mount.
- `MainApp` — loads menus from `load_menus()`, activates actions on click.

### Added to `web/static/src/services/rpc.js`
- `loadMenus()` — wraps `ir.ui.menu.load_menus`
- `loadAction(id)` — calls `POST /web/action/load`
- `getViews(model, views)` — wraps `MODEL.get_views`

---

## Module & Table Inventory (cumulative)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu` |

---

## Next Steps
Per `plan.md` Phase 6: Account module (`account.account`, `account.journal`, `account.move`, `account.move.line`, `account.payment`).
