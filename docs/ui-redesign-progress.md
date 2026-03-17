# UI Redesign Progress — Odoo 14-Style Navigation

## Summary
Replaced the flat left-sidebar navigation with an Odoo 14-style two-level navigation: a **home screen** showing colored app tiles, and an **app context** with a horizontal top nav bar featuring dropdown sections. Backend `ir_ui_menu` seeds were restructured from a flat list to a 3-level hierarchy (App → Section → Leaf).

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## Changed Files

| File | Change |
|------|--------|
| `web/static/src/app.js` | Full rewrite of MainApp; added HomeScreen, AppTopNav components |
| `web/static/src/app.css` | Replaced sidebar layout; added home screen, app nav, dropdown styles |
| `modules/ir/IrModule.hpp` | `seedMenus_()` rewritten: 3-level hierarchy + deletes old flat rows |
| `modules/account/AccountModule.hpp` | `seedMenus_()` rewritten: Accounting sub-menus in 3-level structure |

---

## Frontend Architecture

### Before
- Single `.shell` with 220px left `.sidebar`
- All `ir.ui.menu` root children rendered as flat `.nav-item` elements
- No home screen; welcome message when nothing selected
- No dropdowns

### After

#### Navigation states
```
App → mode='home'  → HomeScreen (app tiles)
    → mode='app'   → AppTopNav (horizontal bar) + content area
```

#### New OWL components

| Component | Purpose |
|-----------|---------|
| `HomeScreen` | Grid of colored app tiles; one per root-level menu (parent=NULL) |
| `AppTopNav` | Horizontal nav bar: ⊞ Home button, app title, section buttons, user menu |
| `DropdownMenu` | (inline in AppTopNav) floating list of leaf items |

#### HomeScreen
- Queries `root.children` from `load_menus` response
- Renders each as an `.app-tile` with `--tile-color` CSS variable from `APP_COLORS` palette
- Click → `selectApp(app)` → build `sections[]` tree → switch to app mode

#### AppTopNav
- Section buttons with `▾` caret when they have children
- Click section with children → toggle `.dropdown-menu` overlay
- Click section without children → direct navigation
- Click outside → close dropdown (via `t-on-click` on nav root)
- `⊞` Home button → `goHome()` → back to home screen

#### MainApp state machine
```js
state = {
    mode: 'home' | 'app',
    apps: [],          // root-level menus (app tiles)
    allMenus: {},      // full load_menus dict for lookup
    activeApp: null,
    sections: [],      // level-1 children of active app, with .children[] populated
    activeMenuId: null,
    action: null,
    loadingAction: false
}
```

---

## CSS Changes

### Removed
- `.shell { flex-direction: row }` (was flexbox row with sidebar)
- `.sidebar`, `.sidebar-logo`, `.nav-item` — sidebar components gone

### Added

| Class | Purpose |
|-------|---------|
| `.shell` | Now `flex-direction: column` |
| `.home-screen` | Full-height column with header + grid |
| `.home-header` | Logo left, UserMenu right |
| `.app-grid` | `display: flex; flex-wrap: wrap; gap: 24px` |
| `.app-tile` | 120×120px colored square, `--tile-color` CSS var |
| `.app-tile-icon` | 2rem emoji icon |
| `.app-tile-name` | White text below icon |
| `.app-nav` | 52px horizontal bar (`--nav-h: 52px`) |
| `.app-nav-left` | ⊞ + separator + app title |
| `.app-nav-items` | Flex row of section buttons |
| `.app-nav-right` | UserMenu |
| `.nav-section-wrap` | `position: relative` — dropdown anchor |
| `.nav-section-btn` | Section button; `.active` = accent bottom border |
| `.nav-caret` | `▾` indicator |
| `.dropdown-menu` | `position: absolute; top: var(--nav-h)` overlay |
| `.dropdown-item` | Dropdown list row; `.active` = accent color |

---

## Backend Menu Hierarchy

### Old (flat)
```
1  Contacts  (parent=NULL, action=1)
2  Users     (parent=NULL, action=2)
3  Companies (parent=NULL, action=3)
4  Chart of Accounts (parent=NULL, action=4)
5  Journals          (parent=NULL, action=5)
6  Journal Entries   (parent=NULL, action=6)
7  Payments          (parent=NULL, action=7)
```

### New (3-level)
```
10  Accounting  (app, parent=NULL)
  11  Journal Entries   → action 6
  12  Customers         (section)
    15  Payments        → action 7
  13  Vendors           (section, placeholder)
  14  Configuration     (section)
    16  Chart of Accounts → action 4
    17  Journals          → action 5
20  Contacts    (app, parent=NULL)
  21  Contacts          → action 1
30  Settings    (app, parent=NULL)
  31  Users             → action 2
  32  Companies         → action 3
50  Products    (app, parent=NULL)   ← added by UomModule
  51  Products          → action 9
  52  Configuration     (section)
    53  Units of Measure → action 8
    54  Categories        → action 10
```

### Migration
`IrModule::seedMenus_()` runs `DELETE FROM ir_ui_menu WHERE id < 10` before inserting new hierarchy. This removes old flat rows on existing databases automatically on next boot.

---

## load_menus Backend (unchanged, already hierarchical)
`IrMenuViewModel::handleLoadMenus_` already builds the full `children[]` map and computes `app_id`. No backend changes were needed — the frontend simply wasn't consuming the hierarchy before.

---

## App Tile Colors

```js
const APP_COLORS = [
    '#875A7B', // Accounting  — purple
    '#00A09D', // Contacts    — teal
    '#E74C3C', // Settings    — red
    '#1ABC9C', // Products    — green
    '#3498DB', '#F39C12', '#9B59B6', '#2ECC71',  // future modules
];
```

Assigned by index of the app in `root.children` order (sorted by sequence).
