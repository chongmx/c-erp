# 095 — `read_group`, and the five views built on it

First slice of the reference ERP-14 gap list from docs/093. The two items that everything
else in that list leans on: grouped aggregation, and the generic view layer.

Suite: **67 passed, 0 failed** (`verify_read_group.sh`, 29 checks).

---

## 1. `read_group`

The primitive. Before this, no model had it and the frontend contained no
group-by code at all — which is *why* every dashboard in this codebase is a
bespoke screen with its own hand-written SQL. Grouped lists, pivots, graphs and
kanban boards are all this one call with a different renderer.

```
read_group(domain, fields, groupby, limit, offset, orderby)
  -> [ { <key>: value, __count: n, <measure>: sum, __domain: [...] }, ... ]
```

- **Group keys** may be selection, many2one, boolean, date or datetime. A date
  key carries a granularity the way the reference ERP spells it — `date:month` — and defaults
  to month when none is given. Multiple keys nest.
- **Measures** are summed. The caller passes its whole field list and the
  non-numeric ones are skipped, which is what the client expects. Money columns
  are stored as micro-units, so sums are converted back on the way out —
  a raw `SUM` would come back a million times too large.
- **`__domain`** selects exactly that group's rows, so a client drills in without
  reconstructing the filter. Date buckets are half-open (`>= start`, `< end`),
  and the end is computed by PostgreSQL rather than by reimplementing month
  lengths and leap years.
- Record rules and company scoping apply, because a grouped total that counted
  rows the caller cannot open would leak by arithmetic even though no row is
  ever returned.

### Served from the model, not the ViewModel

Registering it on `GenericViewModel` would have given grouping to the generic
models and silently withheld it from every hand-written one — which is most of
the interesting ones: `account.move`, `sale.order`, `res.partner`,
`purchase.order`, `stock.picking`. That is the same defect shape this codebase
has hit repeatedly (S-35, S-37, S-38, S-47, and company scoping in docs/094).

So `readGroup` is on `IModel` with a throwing default, `BaseModel` overrides it,
and the **dispatcher answers `read_group` centrally** from the `ModelFactory` —
the same way `get_views` is already answered from the `ViewFactory`. Every model
with a field registry gets grouping whether its ViewModel knows about it or not.
The test asserts this on the five hand-written ones by name.

---

## 2. The five views

`web/static/src/components/RecordViews.js`. All generic: they read `fields_get`
to discover what a model can be grouped and measured by, then drive
`read_group`. No per-model code, so they work on every action.

| View | What it does |
|---|---|
| **Grouped list** | Collapsible groups with counts and a chosen measure subtotalled. Expanding a group fetches its rows with the group's own `__domain`, so the drill-down can never disagree with the count above it. |
| **Kanban** | A column per group, a card per record. Cards are capped at 20 per column with a "+N more" — a board is for scanning. |
| **Pivot** | Two group keys crossed, one measure, with row, column and grand totals. |
| **Graph** | Bar, line or pie as inline SVG. The axis top is rounded up to a value a human would pick; x-labels are rotated so a dozen of them do not overlap. |
| **Calendar** | Month grid, Monday-first, six weeks. Loads the whole visible grid rather than the month, so events in the leading and trailing days are not mysteriously missing. |

A switcher sits above every list. It resets to the plain list on navigation —
carrying a pivot across to a different model would open on a cross-tab of fields
that model may not have.

Chart colours are the eight hues from the Database Tools palette (docs/093),
already checked for contrast and for separation under the three common
colour-vision deficiencies, rather than a fresh unchecked set.

---

## 3. Two bugs worth recording

**A control character blanked the entire pivot.** The cell key joins a row and a
column label, and the separator needs to be something no label can contain — a
comma collides with "Kuala Lumpur, Malaysia". U+0001 was the obvious choice, and
putting it *in the template* was the mistake: OWL parses templates as XML, where
a control character is invalid, so the component failed at compile time and took
the two views mounted after it down with it. The separator now lives only in JS
behind `cellKey()`, and the test greps the served file for control characters.

This is the second time in two features that an invisible character in source
caused a silent, total failure — the first was a stray NUL in docs/093's schema
map. Both were found by rendering the page, neither by reading the code.

**The calendar drew 500 events into 42 cells.** A general ledger puts most of a
month on a handful of dates, so a naive render turned every cell into an
unreadable stack of slivers. Four per day, then a "+N more".

---

## 4. Files

| File | |
|---|---|
| `modules/base/BaseModel.hpp` | `readGroup` — keys, intervals, measures, `__domain` |
| `core/interfaces/IModel.hpp` | `readGroup` on the interface, throwing default |
| `core/infrastructure/JsonRpcDispatcher.hpp` | central `read_group` dispatch via the model factory |
| `core/Container.hpp` | injects the model factory into the dispatcher |
| `modules/base/GenericViewModel.hpp` | registers it for the generic path too |
| `web/static/src/components/RecordViews.js` | the five views |
| `web/static/src/components/recordviews.css` | their styles |
| `web/static/src/app.js` | the switcher in `ActionView` |
| `scripts/verify_read_group.sh` | 29 checks |

---

## 5. Still open from the docs/093 list

Requested alongside this and **not** started:

- **Product templates, variants and attributes** — the structural one. No
  `product.template` exists; every product is a standalone `product.product`.
  Touches sale, purchase, stock, BoM and valuation together.
- **Pricelists** — no customer pricing, quantity breaks or date-bounded rules.
- **Project, tasks and timesheets** — `account.analytic.line` already exists and
  is the natural backbone for the timesheet half.
- **Label and QR printing** — scanning works; printing does not.
- **Distributor catalogue lookup** — needs a provider abstraction, and API keys
  that are not in this repository.

### Known limits of what shipped

- `read_group` aggregates **stored columns only**. A computed field a custom
  ViewModel adds at read time cannot be grouped or summed.
- `SUM` is the only aggregate. the reference ERP also offers avg/min/max per field; nothing
  here needs them yet.
- The views group on **one** key each (pivot takes two). the reference ERP nests arbitrarily.
- The graph has no drill-down on click yet — `__domain` is already returned for
  it, so that is a small follow-up rather than a redesign.
