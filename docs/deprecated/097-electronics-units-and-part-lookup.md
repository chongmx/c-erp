# 097 — Electronics units, parametric search, and the part-lookup agent API

Status: **done**. `./scripts/run_tests.sh` → 70 passed, 0 failed.

The goal was a lane for an external AI agent to browse distributor sites and feed
parts into the catalogue. That turned out to be three problems stacked on top of
each other, and only the third one is the API:

1. the catalogue could not **express** an electronic part's values,
2. so it could not be **searched** by them,
3. so there was nothing meaningful for an agent to write into.

Fixing them in that order is why this document starts with units.

---

## 1. Units (`part_unit`)

`uom_uom` models *how you buy a thing* — Units, Dozens, Metres. It cannot say
"4.7 kΩ", and overloading it would corrupt purchasing. Electronic values needed
their own table.

```
part_unit(id, name, symbol, quantity_kind, factor, is_base)
```

`factor` converts to the kind's base unit; `is_base` marks that base. `symbol` has
a **unique index** — the same symbol must not mean two things.

`ProductModule::seedPartUnits_()` seeds **45 units across 15 quantity kinds**:

| kind | units |
|---|---|
| resistance | Ω kΩ MΩ mΩ |
| capacitance | F mF µF nF pF |
| inductance | H mH µH nH |
| voltage | V mV kV |
| current | A mA µA nA |
| power | W mW kW |
| frequency | Hz kHz MHz GHz |
| charge | Ah mAh C |
| time | s ms µs ns |
| data | B kB MB |
| length | mm m · mass g kg · temperature °C · ratio % ppm · gain dB |

The seed is idempotent and keyed on `symbol`.

> **Regression worth remembering:** the unique index broke `verify_partkeepr.sh`,
> which created *its own* Ω and deleted it afterwards. The schema was right and the
> test was wrong — the fix was to make the test use the seeded unit. A test that
> deletes seed data is a test that only passes once.

## 2. Parameters and parametric search

```
part_parameter(id, product_id, name, value_numeric, value_text, unit_id, value_base, quantity_kind, ...)
```

`value_base` is the load-bearing column. `value_text` keeps what a human typed
(`4k7`, `100n`, `125m`), `value_numeric` the number behind it; `value_base` is
that value in the kind's base unit, as a double, computed on every create and
write by `PartParameterViewModel::normalise_()`.

`parseSiValue()` handles the three notations electronics actually uses:

- plain SI suffixes — `4.7k`, `100n`, `1M`
- **R-notation / infix suffix** — `4k7` = 4.7k, `1R2` = 1.2, `2M2` = 2.2M
- a bare number, taking its scale from the attached unit

Search compares `pa.value_base` and filters on `pa.quantity_kind`, so a range query
for 1 kΩ–10 kΩ finds a part stored as `4k7 Ω` *and* one stored as `4700 Ω`, and never
matches 4.7 kHz. `handleSearchParts` takes **one** `{name, min, max, unit}` clause;
multi-attribute AND arrived later, with `part.catalog` — see docs/098.

Without `quantity_kind` in the predicate, "between 1 and 10" would silently mix
resistance and frequency. That filter is not an optimisation; it is correctness.

## 3. The agent API (`part.lookup`)

Full request/response contract lives in **`docs/LOOKUP_API.md`** — that file is
what you hand to the agent author. Summary of the surface:

| method | who calls it | purpose |
|---|---|---|
| `describe` | agent, once | the schema to conform to: categories, known parameters, units, footprints, accepted value formats |
| `submit` | agent, per part | stage one proposal; returns id + issues |
| `search_read` / `read` | review UI | the queue and one proposal |
| `apply` | reviewer | promote a proposal into `product.product` + `part_parameter` |
| `reject` | reviewer | close it with a reason |
| `fields_get` | UI | field metadata |

**Nothing an agent submits touches the catalogue.** `submit` writes only to
`part_lookup_result`, a staging table, and validates as it goes: unknown unit
symbols, unparseable values, unknown category paths, and low confidence each
attach an issue at `error` or `warning` level. A proposal carrying an error lands
in state `invalid` and `apply` refuses it.

This is deliberate. An agent that browses the web will sometimes be confidently
wrong, and a wrong resistance that lands silently in the catalogue is a part
somebody solders.

`describe` exists so the agent does not have to guess: it returns the real
category tree (80 categories here), the parameter names already in use, and every
unit symbol — so a well-behaved agent submits values the system can already
express, and the issues list catches the rest.

## 4. The review desk (`part.lookup` screen)

`web/static/src/components/PartLookup.js` + `partlookup.css`, registered in
`CUSTOM_VIEWS`, action **106**, menu **79** under Products next to the parametric
search it feeds.

Two panes. Left is the queue, filtered by state (Pending / Needs fixing / Applied
/ Rejected / All) with MPN, manufacturer and confidence on each row. Right is one
proposal: query, MPN, manufacturer, confidence, source link, the parameter table,
then **where it will go** — a category picker preselected to whatever the agent's
`category_path` resolved to, and an optional existing-product id to merge into —
then Apply / Reject.

Two decisions worth stating:

- **Issues render above the parameter table, not below it.** The reviewer should
  know what is doubted *before* reading the numbers. Putting the warnings under
  the data means they get read after the decision is already made.
- **"Paste a result"** accepts a raw JSON payload and runs it through `submit`.
  During agent development you want to try a payload without wiring the agent up,
  and this is that path — it goes through exactly the same validation, so it is
  a real test of the contract rather than a bypass.

Verified rendering against a live server with a headless browser: 80 categories
load, the agent's `category_path` resolves and preselects (id 52, SMD Resistors),
parameters and state chips paint, zero JS errors.

> **Testing note, again.** The first render check reported `cats:0` and looked like
> a bug. It was not — OWL flushes on `requestAnimationFrame` and the DOM was one
> frame behind the count. Reading `component.state` alongside the DOM is what
> distinguished "empty" from "not painted yet". Under headless virtual time, a zero
> DOM count proves nothing on its own.

## 5. Tests

`scripts/verify_part_lookup.sh` — **41 checks**, covering:

- units seeded, symbols unique, factors correct per kind
- `parseSiValue` on plain SI, R-notation, and bare-number-with-unit
- `value_base` recomputed on write, not just create
- parametric range search across mixed input notations
- `quantity_kind` isolation (a resistance query must not match frequency)
- `describe` exposes categories / parameters / units / footprints
- `submit` flags unknown unit, unparseable value, unknown category, low confidence
- `apply` refuses an `invalid` proposal, succeeds on a clean one, creates the
  product and its parameters, and is not repeatable
- `reject` records its reason

Suite total: **70 suites, 0 failures.**

## 6. Not done

- The agent itself. This is the contract it will speak, not the browser.
- No dedupe on `submit` — two proposals for the same MPN both queue up. The
  reviewer sees both; the system does not merge them.
- `apply` into an existing product **adds** parameters; it does not reconcile
  conflicting values on one already there.
