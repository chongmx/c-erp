# 078 — Generic form: polished card layout + combobox "Add new…"

## Problem

Records without a bespoke form (Units of Measure, taxes, journals, rental
configuration, HR, the part catalogue, …) fell through to the generic `FormView`,
which rendered a flat **line-by-line** stack of `label: input` rows in a plain
toolbar frame. It looked nothing like the polished Sales Order / Invoice / Delivery
forms, and a many2one dropdown offered no way to add a missing option without
leaving the form.

## Change (frontend only — `web/static/src/app.js`, `app.css`)

One component, `FormView`, was reworked — so **every** model that uses it is
uplifted at once.

1. **Polished card layout**, matching the SO/Invoice forms:
   - A header bar with a breadcrumb (`<list> › <record>`) and right-aligned
     **Create / Save / Delete / Discard** buttons.
   - Fields laid out in a **two-column responsive grid** inside a titled card,
     instead of one tall single column. Booleans render as real checkboxes; long
     `text` fields become full-width textareas.
   - System columns (`id`, `create_date`, `write_date`, `__last_update`,
     `display_name`) are hidden — the id is still kept on the record for `write()`.

2. **Combobox "Add new…"** — every many2one field (scalar *and* inside an o2m line)
   gets a small **＋** button beside its `<select>`. Clicking it opens a small modal
   ("New <Field>", a Name input, Create / Cancel). On Create the related record is
   created through the API, appended to the dropdown, and **selected immediately** —
   so a user can add a missing Vendor / Category / Company / Unit Type without
   leaving the form. If the target model needs more than a name, the modal surfaces
   the error rather than failing silently.

   Implementation notes: the ＋ is a sibling button (not a sentinel `<option>`), which
   avoids the native-select "stuck on the sentinel" redraw problem. The modal reuses
   the same dark, accent-topped card style as the down-payment wizard.

## Verified (headless Chrome, 0 JS errors)

- **Units of Measure** (product config) — card + two-column grid; the old dull
  line-by-line is gone.
- **Vendor Pricelists** (`product.supplierinfo`) — 3 comboboxes (Company / Vendor /
  Product) each with ＋; created a vendor inline and it auto-selected.
- **Rental → Unit Types** (`rental.unit.type`) — card layout, Company combobox with
  ＋; the ID/timestamp clutter is gone.

## Scope

`FormView` backs the large majority of "options that can be selected" across every
module, so this is the high-leverage fix for the request. Forms that already have a
bespoke component (Sales Order, Invoice, Delivery/Transfer, Product, BoM, Contact,
Warehouse, Location, User) were already polished and are unchanged; extending the
same inline-create ＋ to the Product form's Category/UoM pickers is a natural
follow-up. No backend change — the integration suite (docs/077, 47/0) is unaffected.
