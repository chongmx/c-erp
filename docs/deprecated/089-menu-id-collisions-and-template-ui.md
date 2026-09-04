# 089 — Menu-id collisions (incl. a regression I caused) + the template UI

Started as a question — *"do we have a technical page to maintain these templates?"* —
and turned up a cluster of menu-wiring bugs, one of them mine.

## The template UI: yes, and it is good

**Settings → Technical → Document Templates** (`ReportSettingsView` /
`DocumentLayoutEditor`, `ir.report.template`) is already a capable editor:

- a tab per document (Invoice · Sales Order · Purchase Order · Delivery);
- a **live preview** rendered from dummy data, so you see the actual PDF layout;
- a **Blocks** panel to show/hide sections (logo, header, addresses, items table,
  totals, payment terms, bank details, footer) and an **HTML** tab for the raw
  template;
- a **Properties** panel — paper format, orientation, margins, font family/size,
  colour, line height — and a Record ID box to preview against a real document;
- **Save** writes back to `ir_report_template`.

So the answer to *"is it feasible to add a UI"* is: it exists, and it is the right
shape. What it lacks is a **"Reset to default / compare with shipped template"**
action. Templates are seeded once (`ON CONFLICT DO NOTHING`) and then owned by the
database, which is why improving a template in code does **not** reach existing
installs — the sale-order section/note fix (docs/079) needed a hand-written regexp
migration. A *Reset to default* button plus storing the shipped HTML alongside the
row would remove that whole class of migration. Recommended, not yet built.

## The bug class this exposed: `ir_ui_menu` / `ir_act_window` id collisions

Two modules hardcoding the same id silently hijacks a menu — whichever seeds last
wins, and the loser's menu opens the winner's screen. docs/076 fixed four of these;
this pass found four more:

| Menu | Opened instead | Cause |
|---|---|---|
| Reordering Rules | Document Templates | Stock's act_window 30 lost to Report's |
| ERP Settings | Putaway Rules | Report's act_window 31 lost to Stock's |
| Groups | Work Centers | Report's act_window 36 lost to Mrp's |
| **Settings (whole app)** | *missing from home screen* | **my** Asset Types menu took `ir_ui_menu` id 30 |
| **Contacts (whole app)** | *missing from home screen* | Analytic Accounts took `ir_ui_menu` id 20 |

The last two are the serious ones: seeding a menu on top of an **app root** deletes
an entire app from the home screen. The Settings one was a regression I introduced
in docs/084 (Assets) — `ir_ui_menu` id 30 is the Settings app root, and my
`ON CONFLICT DO UPDATE` overwrote it, orphaning its five children. Contacts (id 20,
taken by Analytic Accounts) was pre-existing and had been missing for some time.

### Fixes

- Reordering Rules → act_window **94**; Groups → **95**; ERP Settings → **96**
  (all `DO UPDATE`, so existing databases self-heal).
- Asset Types **30 → 63**, Budgetary Positions **32 → 64**, Analytic
  Accounts/Items **20/21 → 65/66**.
- `IrModule` now seeds the app roots (10 Accounting, 20 Contacts, 30 Settings) and
  the Settings children (31 Users, 32 Companies) with **`DO UPDATE`** instead of
  `DO NOTHING`, so a clobbered root is restored on the next start rather than
  staying broken forever.

## Guard

`scripts/verify_new_forms.sh` now asserts, beyond the create-doesn't-500 contract:

- nine specific menus each open **their own** model (Sales Orders, Purchase Orders,
  Reordering Rules, Document Templates, Landed Costs, Putaway Rules, ERP Settings,
  Groups, Work Centers);
- **every app root is present on the home screen** (Accounting, Settings, Contacts,
  Sales, Purchase, Inventory, Products, Employees, Manufacturing) and Settings still
  has its submenus;
- it reports any action shared by differently-named menus (one remains, intentional:
  Bank Statements / Journals ▸ Bank and Cash).

The verify scripts also stopped hardcoding menu ids — they assert by **menu name**
(scoped by parent where the name isn't unique, e.g. Rental also has a "Dashboard"),
so repairing an id collision no longer breaks a test.

Full suite: **56 passed, 0 failed**.

## Standing rule

Before hardcoding an `ir_ui_menu` or `ir_act_window` id: grep every module for it.
App roots (10, 20, 30, 50, 60, 70, 80, 90, 110, 300) and the Settings children
(31, 32) are reserved. Ids ≥ 63 for menus and ≥ 94 for act_windows are the current
free space.
