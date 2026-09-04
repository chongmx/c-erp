# 060 — Rental dashboard and the cashflow panel

**Date:** 2026-08-05
**Implements:** `046` §2 · `040` §3.4
**Status:** ✅ Complete and verified

---

## One endpoint, one payload

`GET /rental/dashboard?months=12` returns every panel's data from a handful of aggregate
queries, cached 60 s in `TtlCache`.

Not a dozen `search_read` calls assembled in the browser (`040` §3.4). That is the fastest
route to a four-second paint that hammers the connection pool, and it makes every panel a
separate failure with its own loading state.

The cashflow series is **delegated to `RentalForecast`**, not reimplemented. The dashboard and
the standalone `/rental/cashflow` endpoint therefore cannot disagree about the projection —
there is one implementation.

---

## Panels

| Panel | Form | Why that form |
|---|---|---|
| KPI row | 5 stat tiles | Single current values. A one-bar bar chart for one number is the classic mistake |
| Occupancy | **meter** inside its tile | A ratio against a limit, not a two-slice pie |
| **Cashflow** | grouped bars / cumulative line / table | see below |
| Occupancy by type | horizontal stacked bar | Part-to-whole across 4 states; horizontal because type names are long |
| Receivables ageing | **one-hue ordinal ramp** | 0–30 / 31–60 / 61–90 / 90+ is an ordered magnitude of badness, not four identities |
| Needs attention | table | Mixed classes that each carry meaning — more than ~7 classes is a table, not more colours |
| Activity | feed | Not a chart |

Ageing deliberately does **not** use status red. Status colours are reserved, and the buckets
are a scale rather than four states — reaching for red there would be wrong twice.

---

## One departure from `046` §2, stated rather than slipped in

The plan specified a **2-series line** for revenue vs expenses. The implementation uses
**grouped bars**.

These are discrete monthly totals. A line drawn between September and October implies a value
in between, and there isn't one — you cannot have "mid-October income". Lines are right for a
continuous measure sampled over time; bars are right for per-period totals.

The rest of `046` §9 is followed exactly: one axis, legend always present for two series, a
table view, per-mark hover tooltips, recessive grid, thin marks with 4px rounded ends anchored
to the baseline, and a 2px surface gap between adjacent fills.

### Cumulative is a separate view, not a third series

Income and expense are **flows**; cumulative net is a **stock**. Putting all three on one
scale would be the dual-axis mistake wearing a disguise — the numbers share a unit but not a
meaning.

So it is two charts behind a toggle, per `046` §9's rule that two measures of different scale
become two charts. The cumulative view is where "when do I run out of money" is answerable,
and it draws its zero line distinctly because below it is the month you cannot pay for.

In that view the mark colour follows the **value's meaning** — a negative cumulative point is
`--st-critical` — rather than a series identity. That is the one legitimate use of a status
colour here: it is a state, not a series.

---

## Two numbers that are easy to get plausibly wrong

**MRR normalises the billing interval.** A quarterly tenancy at RM 900 contributes RM 300, not
RM 900. Counting it whole would inflate the single number most likely to be quoted out loud.
Asserted: adding a RM 100 monthly line and a RM 900 quarterly line moves MRR by exactly
RM 400.

**Occupancy excludes retired units from the denominator.** They are not lettable stock, and
counting them would depress the figure permanently every time a locker is decommissioned. The
test adds a retired unit and requires occupancy **not** to move.

Walk-ins are excluded from MRR for the same reason they are excluded from the forecast: they
are not recurring, and calling them recurring revenue would be a claim the data does not
support.

---

## The panels must agree with each other

Two panels contradicting each other destroys trust in both, so the "Net this month" tile reads
the **first month of the same series the chart draws** rather than computing its own figure.
The test asserts they are identical, and that the outstanding total matches an independently
computed SQL sum.

The ageing buckets are asserted to **partition** the outstanding total exactly — if they did
not sum to it, one of the two panels beside each other would be lying.

---

## Two bugs found while wiring it up

**The demo seed showed zero MRR on a visibly 40%-occupied facility.** It predated
`billing_mode` and so created every tenancy as `manual`, which the forecast and MRR correctly
exclude. Fixed to create recurring tenancies at each unit type's own rate, and to seed seven
recurring expense budgets (wifi, electricity, security, cleaning, quarterly maintenance,
annual insurance, the lease) so the cashflow panel has an outgoing side.

**`verify_rental_cashflow.sh` leaked events into the activity feed.** Its cleanup deleted
`rental_event` rows by `partner_id`, but an expense-generated event has no partner — so five
identical `CFTEST Wifi` entries had accumulated and were the first thing the dashboard showed.
Cleanup now filters by summary as well.

**And the seed change then broke that suite, which turned out to be right.** The cashflow
tests asserted **absolute** monthly totals — "September budgeted expense is 200.00". The
forecast is a *global aggregate*, so the moment the demo facility gained recurring expenses,
September became 8,500.00 and six assertions failed.

The tests were only ever correct on an empty database. They now capture a **baseline before
creating anything and assert deltas against it**, which is the property a test over a global
aggregate needs: independent of whatever else the database holds. The suite is stronger than
before it broke, and the demo seed did not need to be weakened to accommodate it.

---

## Looking at it

`scripts/render_dashboard_preview.py` renders the live payload through the real CSS, both
cashflow views side by side, so layout can be inspected without a browser. Per `046` §9 —
*render it and look at it before calling it done*.

The demo facility is loss-making: RM 3,600 MRR against RM 8,300/month of expenses including a
RM 6,500 lease, at 40% occupancy. That is realistic for a half-empty warehouse, and it is what
makes the cumulative view worth having — the line goes down and keeps going.

---

## Verification

```
verify_rental_dashboard   34 checks   payload shape, caching, occupancy denominator,
                                      MRR interval normalisation, walk-in exclusion,
                                      panel agreement, bucket partitioning,
                                      registration and load order, docs/046 §9 rules
```
