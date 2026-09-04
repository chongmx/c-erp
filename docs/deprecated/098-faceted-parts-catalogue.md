# 098 — The faceted parts catalogue

Status: **done**. `./scripts/run_tests.sh` → 71 passed, 0 failed
(`verify_part_catalog.sh`, 53 checks).

The brief was a distributor-style browser: a parametric filter strip on top that
scrolls sideways with each attribute scrolling down inside its own box, and a
result table underneath that scrolls both ways. The reference was an LCSC
category page.

docs/097 gave parts values that could be *compared* (`value_base`). This is the
screen that makes them findable.

---

## 1. Faceting has three rules, and the third is the one people get wrong

```
within one facet   the selected values are OR'd    (0402 or 0603)
across facets      they are AND'd                  (0402 AND YAGEO)
a facet's counts   are computed with ITS OWN selection removed
```

The third rule is the whole design. Count a facet with its own clause applied and
every unselected value collapses to zero: the user picks 0402, the Package facet
reports that no 0603 exists, and multi-select becomes impossible — you could never
widen a selection, only replace it.

`where_(q, skipKey, params, n)` is the single place this lives. Every facet's
count query passes its own key as `skipKey`; the result total passes `""`.
Because `facets` and `search` share that one parser, **the count in the header is
by construction the count of the rows underneath it** — there is no second code
path that could drift. `verify_part_catalog.sh` asserts they agree.

## 2. What can be filtered

| facet key | source |
|---|---|
| `mfr` | `part_manufacturer_info` → `res_partner.name` |
| `pkg` | `product_product.footprint_id` → `part_footprint.name` |
| `param:<name>` | `part_parameter`, discovered per result set |

Parameter facets are **discovered, not configured**: `facets` groups the
parameters of the current result set and returns the top ones by coverage. Filter
to capacitors and the strip becomes Capacitance / Voltage Rating / Dielectric;
filter to resistors and it becomes Resistance / Power / Tolerance / Type. Nothing
declares that anywhere — it falls out of the data.

**Range or enum** is decided by whether the parameter carries a `quantity_kind`
— i.e. a real unit — not by whether it holds numbers:

> `seedPartUnits_` backfills `value_base = value_numeric` for every *unitless*
> parameter, and `value_numeric` defaults to 0. So a text attribute like
> "Thick Film" has a perfectly good numeric `value_base` of 0, and a
> "does it have numbers?" test renders a min/max slider for it. Keying on the
> quantity kind is both simpler and correct.

A range facet returns its own kind's units (Ω/kΩ/MΩ/mΩ — never farads) and the
min/max span of the current result set. The client picks the display unit whose
scale puts the data in 1–1000, so a catalogue of kilohm parts opens showing
`0.015 – 820 kΩ` rather than `15 – 820000 Ω`.

## 3. S-49 in a screen made of user-named columns

A faceted browser is exactly the shape S-49 warns about — the client names the
thing to filter on. Two guards:

- a facet key must match a fixed shape (`mfr`, `pkg`, `param:<name>`); anything
  else is **dropped**, never interpolated. `verify_part_catalog.sh` sends
  `enum: {password: [...]}` and asserts the result is unchanged.
- the `<name>` is bound as `$n` — it is a *value* in `pa.name = $n`, not an
  identifier. Same for every filter value.
- `ORDER BY` comes from a fixed map, so an unknown sort key falls back instead of
  reaching SQL. The test sends `sort: "name; DROP TABLE product_product"` and
  then asserts the fixtures still exist.

Company scoping follows docs/094: `pp.company_id IS NULL OR IN (allowed)`, so
shared catalogue rows stay visible while company-owned ones don't leak.

## 4. The screen

`web/static/src/components/PartCatalog.js` + `partcatalog.css`, action **107**,
menu **86** under Products.

The layout is a fixed-height column and **the page itself never scrolls**. Only
two things scroll vertically — a facet's value list and the table body — and the
facet strip scrolls horizontally. That is what keeps 40 attributes and a large
result set feeling like one screen instead of an endless document.

- **Filter layout: Stacked | Scrolling** — scrolling is one sideways row (default,
  keeps the results visible while you filter); stacked wraps to show every
  attribute at once.
- Enum values apply on click; **ranges apply on Enter or via Apply**, because a
  half-typed `1` in a min box should not narrow the world to nothing before you
  have typed the `k`.
- Applied filters appear as removable chips — the only way to see everything
  currently narrowing the list without scrolling the whole strip.
- Result columns are derived per page: parameters present on at least half the
  rows become their own column.

## 5. Demo data

`scripts/seed_demo_parts.sh` — 163 parts (E12 resistors 10 Ω–1 MΩ across four SMD
packages plus axial through-hole, E6 MLCCs 10 pF–100 µF), 935 parameters, real
manufacturers and MPNs. Idempotent, marked by the `DP-` code prefix,
`--clean` removes it.

Resistance values are stored in **deliberately mixed notation** — some `4k7`,
some `4.7k` — because a range query that only works when everyone types the same
way is not a range query. Both land in the same `value_base`.

`seedFootprints_()` seeds the standard package vocabulary (45 packages) as
reference data, on the same argument as `part_unit`: 0402 and SOIC-8 are a fixed
public vocabulary, and without them the Package facet is empty on a fresh install.

## 6. Bugs this found

Worth recording because all four were only visible once rendered, not in any
assertion I had written:

1. **`to_char(1.0, 'FM999999.999')` returns `"1."`** — FM strips trailing zeros
   but keeps the point, so the catalogue filled with `1.kΩ` and `±5.%`.
2. **Tolerance stored against the wrong base.** `%` is the *base* of the ratio
   kind (factor 1), so 5% is `value_base = 5`, not `0.05`. Storing the fraction
   made the Tolerance facet default to ppm and report a span of `1e+3 – 5e+4`.
3. **0805 and 1206 chip resistors filed under "Through-Hole Resistors"** — the
   seeder's category branch disagreed with the Mounting parameter it wrote.
4. **`part_footprint(name)` unique index broke `verify_partkeepr`**, which minted
   its own `SOIC-8`. The empty id then corrupted every JSON body built from it,
   surfacing as `Invalid JSON` rather than as a collision. Fixed the *test* —
   same lesson as the earlier `part_unit(symbol)` case: a fixture must not squat
   on a public name.

And one testing note: a `psql -tAc "INSERT … RETURNING id"` prints the row **and**
the `INSERT 0 1` command tag. The two-line id silently corrupted every statement
built from it, which is why the first run of `verify_part_catalog.sh` failed 12
checks that had nothing to do with faceting. `| head -1`.

## 7. Not done

- Counting is one query per facet against the full predicate. Correct, and fine at
  this size; a catalogue in the millions would need a different approach
  (precomputed counts or a search index). Correctness first, and stated plainly.
- No "compare selected" or BOM upload — both are real distributor features and
  neither is in scope here.
- The result table has no inline quantity/price-break widget; pricing is a single
  list price per row.
