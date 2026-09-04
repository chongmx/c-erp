# User Group Management UI — Progress Log (2026-03-23)

Build clean after all changes.

---

## Where to Access

```
Settings
├── Users              ← list all users; click row to open form
│   └── [User Form]    ← General Information + Access Rights tabs
└── Technical
    └── Groups         ← read-only list of all 16 groups with member counts
```

---

## Feature 1 — User Form with Access Rights Tab

**File:** `web/static/src/app.js` — new `UserFormView` component

### General Information tab

| Field | Input type |
|-------|-----------|
| Login (Email) | text input |
| Password | password input (leave blank to keep existing) |
| Display Name (Partner) | select from res.partner |
| Company | select from res.company |
| Active | checkbox |

### Access Rights tab

Groups are organised by module in a table. Technical groups use **checkboxes** (independent). Module groups use **radio buttons** (mutually exclusive: None / User / Manager).

| Section | Type | Options |
|---------|------|---------|
| Technical | checkboxes | Internal User · Administrator · Configuration |
| Accounting | radio | None · Billing · Accountant |
| Sales | radio | None · User · Manager |
| Purchase | radio | None · User · Manager |
| Inventory | radio | None · User · Manager |
| Manufacturing | radio | None · User · Manager |
| Human Resources | radio | None · Employee · Manager |

Saving sends a **many2many command 6** (replace-all) to `res.users/write`:
```js
groups_id: [[6, 0, [2, 5, 7]]]   // replaces all group memberships
```

### New component: `UserFormView`

Replaces the generic `FormView` for `res.users` in form mode. Registered in `ActionView` via:
```js
get isUsersModel() { return this.currentAction.res_model === 'res.users'; }
```

---

## Feature 2 — Groups List (Settings → Technical → Groups)

**File:** `web/static/src/app.js` — new `GroupsListView` component

Shows all 16 groups in a table:

| Column | Description |
|--------|-------------|
| Full Name | e.g. "Accounting / Accountant" |
| Name | e.g. "Accountant" |
| Portal | ✓ if `share = true` |
| Users | count of users in this group |

Read-only — groups are predefined and seeded at boot. No create/edit/delete in the UI (groups are code-controlled constants).

Registered in `ActionView` via:
```js
get isGroupsModel() { return this.currentAction.res_model === 'res.groups'; }
```

---

## Backend Changes

### `modules/auth/AuthModule.hpp` — new `GroupsViewModel`

Full ViewModel for `res.groups` registered under `"res.groups"`:

| Method | Behaviour |
|--------|-----------|
| `search_read` | Returns all groups with `user_count` via COUNT LEFT JOIN |
| `read` | Returns group + `user_ids` array (members) |
| `create` | Insert new group |
| `write` | Update name / full_name / share |
| `unlink` | Delete group (CASCADE removes memberships) |
| `fields_get` | Field introspection |
| `search_count` | Count groups |

### `modules/auth/AuthViewModel.hpp` — `res.users` extended

**`handleRead`**: accepts explicit `ids` (not just current session uid); appends `groups_id: [2, 7, …]` for each record when `groups_id` is in the requested fields.

**`handleSearchRead`**: appends `groups_id` array to each user record (used by user list).

**`handleWrite`**: processes `groups_id` many2many commands before writing scalar fields:
- Command 6 `[6, 0, [ids]]` — replace all group memberships
- Command 4 `[4, id]` — add one group
- Command 3 `[3, id]` — remove one group

**`handleCreate`**: accepts `groups_id` commands, applies them after INSERT.

**`seedAdminUser_`**: admin user is now added to both groups 2 (Internal User) and 3 (Administrator) on first boot.

### `modules/ir/IrModule.hpp` — new menu entries

```cpp
// ir_act_window
(4, 'Groups', 'res.groups', 'list', 'groups', '{}')

// ir_ui_menu — Settings app
(33, 'Technical', parent=30, sequence=90, action=NULL)   // section header
(34, 'Groups',    parent=33, sequence=10, action=4)       // leaf
```

---

## CSS Added (`web/static/src/app.css`)

```css
.form-tab-btn         /* tab button base style */
.form-tab-btn.active  /* blue underline for active tab */
.ar-table             /* access rights table */
.ar-section-label     /* module name column */
.ar-options           /* radio/checkbox column */
.ar-check-label       /* checkbox row layout */
.ar-radio-label       /* radio row layout (inline) */
.ar-hint              /* grey hint text */
.field-row label      /* form field label */
.field-input          /* form field input/select */
```

---

## GROUP_DEFS constant

Defined in `app.js` before `UserFormView`. Drives the Access Rights table layout — add a new row here to expose future groups without changing component code:

```js
const GROUP_DEFS = [
    { label: 'Technical', type: 'checkboxes', groups: [
        { id: 2, label: 'Internal User', hint: 'Required for all internal access' },
        { id: 3, label: 'Administrator', hint: 'Full access to all modules' },
        { id: 4, label: 'Configuration', hint: 'Can modify system settings' },
    ]},
    { label: 'Accounting', type: 'radio', noneLabel: 'None',
      groups: [{ id: 5, label: 'Billing' }, { id: 6, label: 'Accountant' }] },
    // ... Sales, Purchase, Inventory, Manufacturing, Human Resources
];
```
