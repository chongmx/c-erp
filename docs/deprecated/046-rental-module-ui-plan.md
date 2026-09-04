# 046 — Rental Module UI Plan

**Date:** 2026-08-02
**Relates to:** `040` §3 (module design), `045` (prerequisites)
**Frontend:** OWL 2, no build step, served statically from `web/static/`

---

## 0. The frontend problem to solve first

`web/static/src/app.js` is **9,227 lines** and holds 25 component classes. View routing is a
`t-elif` ladder inside one template (`ActionView`, `app.js:8697`):

```xml
<t t-elif="isERPSettingsModel"><ERPSettingsView/></t>
<t t-elif="isReportTemplateModel"><DocumentLayoutEditor/></t>
<t t-elif="isPortalPartnerModel"><PortalUserListView/></t>
<t t-elif="isGroupsModel"><GroupsListView/></t>
…
```

The rental module needs 6–8 views. Appending them adds ~2,000 lines to a file that is already
hard to navigate, and four more rungs to the ladder.

A `components/` directory already exists and `index.html` loads from it in order:

```html
<script src="/src/services/rpc.js"></script>
<script src="/src/components/LoginPage.js"></script>
<script src="/src/components/UserMenu.js"></script>
<script src="/src/components/DatePicker.js"></script>
<script src="/src/app.js"></script>
```

So the pattern exists and works — it just stopped being used. Note `Dashboard.js`,
`FieldsInspector.js` and `PartnerList.js` are **dead code**: zero references in `app.js` and
absent from `index.html`. Delete them; a stale `Dashboard.js` next to a new rental dashboard is
an active trap.

### Decision: build rental in `components/rental/`, not in `app.js`

```
web/static/src/components/rental/
  RentalDashboard.js      KPI row, occupancy, revenue, aging, activity
  RentalUnitGrid.js       visual locker map (the signature view)
  RentalUnitListView.js   table view of units
  RentalContractForm.js   contract header + per-unit lines
  RentalContractList.js
  RentalExpenseView.js    one-off + recurring
  RentalEventFeed.js      shared: dashboard panel + contract sidebar
  rental.css              scoped styles + the viz tokens
```

Each file declares `class X extends owl.Component` with `owl.xml` templates — the fully
qualified form the existing `components/` files use, because they load before `app.js`
destructures the OWL globals.

**Registration:** replace the ladder with a lookup so rental adds a map entry rather than a
rung:

```js
// app.js — replaces the t-elif chain
const CUSTOM_VIEWS = {
    'ir.report.template': DocumentLayoutEditor,
    'portal.partner':     PortalUserListView,
    'res.groups':         GroupsListView,
    'rental.unit':        RentalUnitGrid,
    'rental.contract':    RentalContractList,
    'rental.expense':     RentalExpenseView,
};
```

~30 lines of refactor, contained, and it stops the ladder growing. Do it as the first rental
commit, before any rental UI exists.

---

## 1. Screens

| Screen | Component | Purpose |
|---|---|---|
| **Dashboard** | `RentalDashboard` | Landing page — occupancy, money, what needs attention |
| **Unit grid** | `RentalUnitGrid` | Visual map of every locker/room by state |
| **Unit list** | `RentalUnitListView` | Sortable table; bulk edits; CSV export |
| **Contract form** | `RentalContractForm` | Customer, lines with per-unit dates, invoices, payments |
| **Contract list** | `RentalContractList` | Filter by state; overdue first |
| **Expenses** | `RentalExpenseView` | One-off + recurring templates |
| **Portal** | (portal.js) | Customer: my units, invoices, balance |

---

## 2. Dashboard

The requirement was "display important info". The discipline that makes that real is deciding
**what the reader must do** with each number, then letting the job pick the form — colour last.

### Layout

```
┌─────────────────────────────────────────────────────────────┐
│  KPI ROW  — 5 stat tiles                                    │
│  Occupancy · MRR · Outstanding · Overdue · NOI (MTD)        │
├──────────────────────────────┬──────────────────────────────┤
│  OCCUPANCY BY UNIT TYPE      │  RECEIVABLES AGEING          │
│  horizontal stacked bar      │  4-step ordinal bar          │
├──────────────────────────────┼──────────────────────────────┤
│  REVENUE vs EXPENSES, 12 mo  │  NEEDS ATTENTION             │
│  2-series line               │  table (not a chart)         │
├──────────────────────────────┴──────────────────────────────┤
│  RECENT ACTIVITY — last 20 rental.event rows                │
└─────────────────────────────────────────────────────────────┘
```

### Panel-by-panel, with the reasoning

**KPI row — stat tiles, not charts.** Five single current values, each with a delta vs prior
month. A one-bar bar chart for a single number is the classic mistake; a stat tile is the
form. Occupancy gets a **meter** (a ratio against a limit), not a 2-slice pie.

**Occupancy by unit type — horizontal stacked bar.** Part-to-whole across 4 states
(occupied / available / reserved / maintenance) per unit type. Horizontal because type names
are long. Categorical colour, because the states *are* the subject.

**Receivables ageing — ordinal bar, one hue.** 0–30 / 31–60 / 61–90 / 90+ is an **ordered
magnitude of badness**, not four identities. So it is a sequential/ordinal ramp — one hue,
light→dark — not four categorical hues. Reaching for status red here would be wrong twice:
status colours are reserved, and the buckets are a scale, not states.

**Revenue vs expenses — 2-series line, 12 months.** Two series, one axis. Both are currency,
so they share a scale legitimately. **Never a dual axis** — if a future panel needs
transactions-count against revenue, that is two charts, not two y-scales.

**Needs attention — a table.** Overdue >60d, units in maintenance, contracts in draft >7d,
unallocated payments. Mixed classes that all carry meaning: a table, optionally with a status
dot. More than ~7 classes is a table, not more colours.

**Recent activity — feed**, reading `rental.event`. Not a chart.

### Delivery: one endpoint, not N calls

`GET /rental/dashboard` returns the whole payload from a handful of aggregate queries, cached
60 s in `TtlCache`. Assembling this from a dozen `search_read` calls is the fastest route to a
4-second paint that hammers the connection pool.

---

## 3. Colour — validated, not chosen by eye

Run against the skill's validator; results reproducible with `scripts/validate_palette.py`.

### Occupancy — 4 categorical slots

| State | Light | Dark |
|---|---|---|
| Occupied | `#2a78d6` | `#3987e5` |
| Available | `#eb6834` | `#d95926` |
| Reserved | `#1baf7a` | `#199e70` |
| Maintenance | `#eda100` | `#c98500` |

```
LIGHT  PASS band · PASS chroma · PASS CVD (worst adjacent ΔE 9.1 protan)
       PASS normal-vision (ΔE 22.9) · WARN contrast: #1baf7a 2.74, #eda100 2.11
DARK   PASS all five (worst adjacent CVD ΔE 8.4, normal-vision 19.8, contrast ≥3:1)
```

The light-mode contrast WARN is **not dismissable** — it obliges relief. Satisfied here by
direct labels on every stacked segment plus the table view; both are in the spec anyway.
At 4 series direct labels are mandatory regardless (yellow and orange share the screen).

### Receivables ageing — ordinal ramp, one hue

| Bucket | Light | Dark |
|---|---|---|
| 0–30 | `#86b6ef` | `#cde2fb` |
| 31–60 | `#3987e5` | `#9ec5f4` |
| 61–90 | `#256abf` | `#5598e7` |
| 90+ | `#104281` | `#256abf` |

```
LIGHT  PASS monotone · PASS ΔL gaps · PASS light-end contrast 2.06:1 · PASS single hue (3°)
DARK   PASS monotone · PASS ΔL gaps · PASS light-end contrast 3.23:1 · PASS single hue (3°)
```

Light starts at step 250 and dark stops at step 500 — the ordinal rule that the step nearest
the surface must still clear 2:1.

### Revenue vs expenses — categorical slots 1–2

`#2a78d6` / `#eb6834` light, `#3987e5` / `#d95926` dark. Two series: legend present **and**
both direct-labeled at the line ends.

### Status — reserved, never a series colour

`good #0ca30c` · `warning #fab219` · `serious #ec835a` · `critical #d03b3b`, each shipped with
an **icon + label**, never colour alone. Used for unit state dots and overdue flags — never to
paint a chart series.

### Tokens

Declared once in `rental.css` as custom properties under `.viz-root`, with dark values under
**both** `@media (prefers-color-scheme: dark)` and `:root[data-theme="dark"]` so the app's
theme toggle wins in both directions. Dark is a **selected** set of steps, not an automatic
inversion.

---

## 4. Unit grid — the signature view

The one screen a storage business actually lives in: a visual map of the facility.

```
Zone A — Small Lockers                    ▓ occupied  ░ available  ▒ reserved  ⚠ maint.
┌────┬────┬────┬────┬────┬────┬────┬────┐
│A01▓│A02▓│A03░│A04▓│A05▒│A06▓│A07⚠│A08░│
└────┴────┴────┴────┴────┴────┴────┴────┘
```

- Colour by state, **plus a glyph** — never state by colour alone.
- Hover: tenant, contract, since-date, next invoice, balance.
- Click: open the contract, or start one if available.
- Filter row above the grid: zone, unit type, state.
- Falls back to the list view below a breakpoint; the list is also the accessible table view.

Grid cells are a heatmap-shaped form, so per-mark hover tooltips are required, with hit targets
larger than the visible cell.

---

## 5. Contract form

The interesting part is **per-line dates** — the thing that makes this domain awkward.

```
Contract RENT/2026/0042            [Active]        Customer: Acme Sdn Bhd
─────────────────────────────────────────────────────────────────────────
 Unit    Type          Start        Rate    Next invoice   State
 A03     Small Locker  2026-01-15   120.00  2026-09-15     active
 B12     Room          2026-03-01   450.00  2026-09-01     active
 C07     Small Locker  2026-08-20   120.00  2026-09-20     pending
─────────────────────────────────────────────────────────────────────────
 Deposit: 500.00 (held)      MRR: 690.00      Balance due: 240.00
```

Each line shows its own `next_invoice_date`, because lines with different start dates bill on
different days — that is the data model surfacing honestly rather than being hidden behind a
single contract-level date.

Tabs: **Lines · Invoices · Payments · Expenses · Events**.

- Invoices tab: generated invoices with residual + a "what does this cover" link to
  `rental.invoice.link`.
- Payments tab: allocations, including **unallocated credit** — the advance-payment case has to
  be visible or the number is unexplainable.
- Events tab: `RentalEventFeed` filtered to this contract.

Reuses the existing `ChatterPanel` and `AuditLogPanel`, and the OCC conflict banner from `036`.

---

## 6. Interaction & accessibility

- **Hover by default.** Tooltips on every mark: crosshair on the line chart, per-mark on bars
  and grid cells. A chart in HTML is interactive; a static one is a missed default.
- **Filters in one row above the charts** — date range (preset rows) + unit type. Dashboard
  filters apply to all panels.
- **Legend for ≥2 series; ≤4 also direct-labeled**, so identity is never colour-alone.
- **Table view toggle** on every chart panel — also the relief for the light-mode contrast WARN.
- **Tabular figures** (`font-variant-numeric: tabular-nums`) in table columns and axis ticks;
  proportional for stat-tile values.
- **Status is icon + label**, never a bare colour dot.

---

## 7. Portal (customer-facing)

Separate stack (`portal.html` / `portal.js`), deliberately minimal:

- My units — card per unit: code, type, since, monthly rate
- My invoices — list with status, due date, PDF download
- Balance — one hero figure, with overdue called out
- Payment history

No dashboard, no charts. Customers want one number and a list.

**Requires `ir.rule` record rules scoped by `partner_id`** — the portal is the public surface,
and S-41/S-47 are both reminders of what happens when a model joins the system outside the
framework.

---

## 8. Build order

| # | Step | Effort | Why here |
|---|---|---|---|
| 1 | `CUSTOM_VIEWS` map refactor; delete the 3 dead components | 0.5 d | Before adding to the ladder |
| 2 | `rental.css` tokens + light/dark scopes | 0.5 d | Everything else consumes it |
| 3 | Unit list + unit grid | 1 w | Usable as soon as units exist |
| 4 | Contract list + form | 1.5 w | Core data entry |
| 5 | `/rental/dashboard` endpoint + KPI row | 3 d | Tiles first — no chart code needed |
| 6 | Charts: occupancy, ageing, revenue | 4 d | Validated palette already fixed |
| 7 | Expenses view | 3 d | |
| 8 | Event feed | 2 d | Shared component, two mount points |
| 9 | Portal views | 1 w | After record rules exist |
| | | **~5 weeks** | Overlaps backend Phase 2 in `044` |

Steps 1–2 are worth doing early even if rental slips — they pay for themselves on any
subsequent view.

---

## 9. Rules for this UI

| Rule | Why |
|---|---|
| No dual-axis charts, ever | Two scales on one plot is the most common chart error |
| Categorical hues in fixed slot order, never cycled | A 9th series folds into "Other" or facets |
| Colour follows the entity, not its rank | A filter that changes series count must not repaint survivors |
| Status colours never become series colours | They are reserved, and ship with icon + label |
| Text wears text tokens, never the series colour | A coloured mark beside the label carries identity |
| Re-run the validator on any palette change | The colour checks are computable — compute them |
| Render and *look* at it before calling it done | The validator checks colour, not label collisions or overflow |
