# The frontend

Plain JavaScript and OWL 2, served statically from `web/static/`. **There is no
build step** — no npm, no webpack, no bundler. Edit the file, reload the page.

That constraint is the reason for most of the patterns below, and for the OWL
rules in [../development/conventions.md](../development/conventions.md#owl-2-iife-build).

---

## Four separate front ends

| Entry point | Served at | Who uses it |
|---|---|---|
| `web/static/index.html` | `/` | staff — the ERP application |
| `web/static/portal.html` + `portal.js` | `/portal` | customers |
| `web/static/kiosk.html` | `/kiosk` | a shared tablet at the door (HR punch) |
| server-rendered pages | `/site` | the public — the CMS |

The ERP application is the only one that loads OWL. The portal and kiosk are
hand-written pages against their own narrow APIs; the public site is rendered
server-side from typed blocks and ships no framework at all.

The website's **in-place editor** (`web/static/website-editor.js`) is injected
into a public page for an authenticated editor.

## The ERP application

`index.html` loads OWL, then `rpc.js`, then each component file, then `app.js`
last. Script order is load-bearing — a component must be defined before the
component that names it in `static components`.

```
web/static/
  lib/owl.iife.js                the framework (vendored)
  src/services/rpc.js            the single JSON-RPC client
  src/components/*.js            one screen each (19 loaded, 3 unused)
  src/app.js                     ~12,300 lines: the shell and the custom forms
  src/app.css                    the stylesheet
```

### app.js

`app.js` holds the navigation shell and every form that is too specific to be
generic:

| Region | Components |
|---|---|
| Shell | `App` (auth gate) → `MainApp` → `HomeScreen` / `AppTopNav` → `ActionView` |
| Generic | `ListView`, `FormView` |
| Panels | `ChatterPanel`, `AttachmentPanel`, `AuditLogPanel` |
| Document forms | `InvoiceFormView`, `SaleOrderFormView`, `PurchaseOrderFormView`, `TransferFormView`, `ProductFormView`, `ContactFormView`, `BomFormView` |
| Config forms | `LocationFormView`, `WarehouseFormView`, `UserFormView`, `GroupsListView`, `PortalUserListView`, `ProductCategoryTree` / `ProductCategoryListView` |
| Accounting | `AssetFormView`, `BudgetFormView`, `BankAccountFormView`, `AccountDashboard`, `AccountSettings` |
| HR | `ExpenseSheetFormView` |
| Settings | `ERPSettingsView`, `ReportSettingsView`, `DocumentLayoutEditor` |

`CUSTOM_VIEWS` is the map that lets a model replace the generic `ActionView`
entirely.

### What the generic `FormView` renders

It builds itself from `get_views`, so a model gets a usable form with no
frontend code at all. One control per field type:

| `fields_get` type | Control |
|---|---|
| `many2one` | `M2OSelect` (below), plus a **＋** that creates a record from a name |
| `selection` | a `<select>` built from the field's `selection` list |
| `boolean` | a checkbox |
| `text` | a textarea |
| `date`, `datetime`, `integer`, `float`, `monetary` | a typed `<input>` |
| `one2many` | an editable line table, each line's m2o also an `M2OSelect` |

A **selection renders as a combobox only if the server declares it one.** A
field registered as `FieldType::Char` gets a free text box however few values
the database constraint allows, and the user is left guessing the spelling —
which is what `rental.contract.billing_period` did before it was declared
`FieldType::Selection`.

The blank option means *unset* and is sent as `null`, never `''`: an empty
string is not one of the allowed values and fails the CHECK with a raw SQL
error instead of taking the column default.

`CONDITIONAL_FIELDS` near the top of `app.js` hides a field for some values of
another — the whole of this form's `attrs` support, currently one entry:
rental contracts show "Every X" and its unit only when the billing period is
Custom.

### Navigation is IR-driven

Nothing about the menu is hardcoded in the frontend. The shell reads
`ir_ui_menu` and `ir_act_window` from the server:

```
HomeScreen (app tiles)
  → click a tile → app context
      → AppTopNav section → direct link  → load action
      → AppTopNav section → dropdown     → leaf → load action
```

An action names a model and a view mode; `ActionView` then either loads the
custom view for that model or falls back to `ListView` / `FormView`, which
build themselves from `get_views` and `search_read`.

Adding a screen therefore means seeding a menu and an action row — not editing
the navigation code. Menu and action ids are hardcoded in the seeds and must
never be reused; see [../reference/id-registry.md](../reference/id-registry.md).

### Components

| File | Screen |
|---|---|
| `LoginPage.js` | login, including the company chooser |
| `UserMenu.js` | the top-right menu and company switcher |
| `DatePicker.js` | the shared date widget |
| `M2OSelect.js` | the shared many-to-one picker — **every** relation field, see below |
| `RecordViews.js` | grouped list, kanban, pivot, graph and calendar — **generic** |
| `AccountReports.js` | financial statements and the tax report |
| `BankReconcile.js` | bank statement reconciliation |
| `DbStudio.js` | Database Tools — browser, SQL console, schema map |
| `DbBackups.js` | backup / restore / export / import |
| `CompanyAdmin.js` | companies and per-user company access |
| `AiSettings.js` | AI provider, model, caps |
| `PartLookup.js` | the part-lookup agent review queue |
| `PartCatalog.js` | the faceted parts catalogue |
| `CategoryTree.js` | the category tree |
| `BomEditor.js` | the BOM editor and importer |
| `BarcodeScan.js` | barcode scanning |
| `TaskBoard.js` | the project kanban |
| `TimesheetGrid.js` | timesheet entry |
| `HelpCenter.js` | the Help Centre |
| `WebsitePages.js` | the page editor and view switcher |
| `Dashboard.js`, `PartnerList.js`, `FieldsInspector.js` | present in the tree but **not loaded** by `index.html` — dead unless re-added |

`rental/` holds `RentalUnitGrid.js`, `RentalDashboard.js` and
`RentalDemoData.js`.

### Picking a related record — `M2OSelect`

Every many-to-one field in the ERP is rendered by one component,
`web/static/src/components/M2OSelect.js`. It is a search box, not a `<select>`,
and it never holds the whole table:

| | |
|---|---|
| typing | `search_read(domain + [['name','ilike',term]], limit 20, order 'name ASC')`, debounced 250 ms |
| the count | `search_count(domain)` — the dropdown states how many rows it is **not** showing |
| "Browse all" | the same search, paged 50 at a time, with Prev/Next |
| the current value | `read([id], fields)` — resolved **by id**, never looked up in a page of results |

That last row is the important one. The control it replaced was a `<select>`
filled once on form open with `search_read([[]], limit: 200)` and no `ORDER BY`,
which fails three separate ways once a table passes the limit:

- **truncation** — the default order is `id ASC`, so the list held the *oldest*
  200 rows and a newly created company was never in it;
- **staleness** — one fetch per form open, so a record created elsewhere never
  appeared;
- **silent value loss** — a `<select>` whose value is not among its options
  falls back to the first one, so opening such a record and pressing Save
  quietly cleared the link.

Resolving the current value by id is what makes the third impossible.

Props: `model`, `value`, `label`, `domain`, `fields` (extra columns to read),
`format` (a record → label function, for `code — name` style labels),
`readonly`, `onSelect(id, displayName)`.

Two rules when using it:

- **name it in `static components`.** OWL resolves a sub-component at first
  render, so a class that renders `<M2OSelect/>` without declaring it throws
  only when a user opens that exact form — nothing else catches it.
  `tests/functional/13-form-pickers` checks every class both ways.
- **no prefetching alongside it.** The point is that the table is never listed;
  a leftover `search_read([[]], limit: 500)` next to a picker is a round trip
  whose result nothing reads.

The one deliberate exception is the expense line's tax `<select>`: that list
carries `amount` and `price_include`, which the form reads to keep totals live
as the user types. It is a data source, not a picker.

### The five generic views

`RecordViews.js` is worth singling out. Grouped list, kanban, pivot, graph and
calendar are **model-agnostic**: they call `fields_get` to discover what a model
can be grouped and measured by, then drive `read_group`. No per-model code.

```
groupable = selection | many2one | boolean | date | datetime
measure   = integer | float | monetary
```

A date group key carries its granularity the way the reference ERP spells it —
`"date:month"`.

## The client

`rpc.js` is the only place that talks to the server. Everything goes through
`POST /web/dataset/call_kw` with the session cookie; see
[../reference/http-api.md](../reference/http-api.md).
