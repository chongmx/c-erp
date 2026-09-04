# 096 — Product templates, variants, attributes, and pricelists

Second slice of the docs/093 gap list: the two structural product-side gaps.

Suite: **69 passed, 0 failed** — `verify_product_variants.sh` (24 checks),
`verify_pricelists.sh` (23 checks).

---

## 1. Templates and variants

Before this, `product.product` was the only product model, so anything sold in
two sizes was two unrelated records sharing nothing — no common description, no
common price, no way to see them as one thing.

Now:

- **`product.template`** is what a person means by "a product" — *T-Shirt*.
- **`product.product`** is what stock, accounting and every order line actually
  move — *T-Shirt (Red, L)*.
- **`product.attribute`** / **`product.attribute.value`** — Size: S, M, L.
- **`product.template.attribute.line`** — which attributes a template varies on.
- **`product.template.attribute.value`** — which values are in play, and what
  each adds to the price (`price_extra`).
- **`product_variant_combination`** — one row per attribute per variant, so a
  variant's identity is the *set* of its rows.

### `product.product` did not move

Every existing row keeps its id and gains a `product_tmpl_id`. A startup
migration gives each product its own template, copying its fields across. No
other module changed, and a single-variant product behaves exactly as before —
the test asserts no stock move lost its product.

### Generation

`generate_variants` builds the cartesian product of the selected values. Its
contract, all asserted:

- Identity is the **combination**, not the name — running it twice creates
  nothing the second time. Without this, every save would double the catalogue.
- A variant that is no longer possible is **archived, never deleted**.
  `product_product` ids are referenced by stock moves and invoice lines;
  deleting one would either fail on a foreign key or quietly corrupt history.
- `price_extra` accumulates onto the template price, so *L* costing +5 gives
  exactly the two L variants a price of 105 and leaves the other four at 100.
- A template with no attributes still has exactly one variant.

### The bug this shook out

Dropping an attribute value and putting it back created a **duplicate** variant
instead of reviving the archived one.

The cause was my own inconsistency: variants were archived, but the
`product_template_attribute_value` rows they are keyed on were **deleted** — and
the cascade took the archived variant's combination with them. Its identity
vanished, so re-adding the value minted a second variant beside the first.

Attribute values now deactivate rather than delete, and reviving one reuses its
id. Same principle, applied to both halves.

---

## 2. Pricelists

A product had exactly one price. A pricelist is an ordered set of **rules**, and
the first match wins — which is what lets a trade customer, a quantity break and
a dated promotion coexist without any of them being the "real" price.

**Precedence is narrowest-first**: a rule for this exact variant beats one for
its product, which beats its category, which beats a global rule. Within one
level, `sequence` then id decide. The `applied_on` codes (`0_product_variant`
… `3_global`) are numbered so that a plain `ORDER BY` sorts them correctly.

Three ways to compute:

| Mode | Result |
|---|---|
| `fixed` | the stated price |
| `percentage` | base less N% |
| `formula` | base (list *or* cost) less a discount %, plus a surcharge |

Money stays in micro-units end to end; the only division is the percentage, done
on `int64` so a float never creeps into a price. A price is floored at zero — a
500% discount gives 0, not a negative.

`price_breakdown` returns every candidate rule in the order they are considered,
so a surprising price can be *explained* rather than argued about.

`res_partner.property_product_pricelist_id` puts a customer on a list, and
`sale_order.pricelist_id` records which list an order was priced with.

### The bug this shook out

Every rule creation failed with `invalid input syntax for type date: ""` — the
model serialised an unset date as an empty string, and a DATE column rejects
that outright. Unset dates now go to SQL as NULL. This broke *almost every*
create, since most rules have no promotion window.

---

## 3. Two test-harness bugs worth recording

Both produced convincing false failures, and both cost more time than the real
bugs did:

- A `$(...)` nested inside quoted JSON built a malformed body for every dated
  call, so the date logic looked broken when the shell escaping was.
- Earlier in this series, a greedy `sed` paired the *last* group's domain with
  the *first* group's count and reported a leak that did not exist.

The pattern is the same each time: an assertion that is loose enough to be
wrong in a way that looks like a product defect. Building the request line by
line, and comparing exact values rather than pattern-matching, avoids it.

---

## 4. Files

| File | |
|---|---|
| `modules/product/ProductModule.cpp` | five new models, `ProductTemplateViewModel` (generation), `ProductPricelistViewModel` (resolution), schema, migration, views, menus |
| `modules/product/ProductModule.hpp` | `migrateTemplates_` |
| `scripts/verify_product_variants.sh` | 24 checks |
| `scripts/verify_pricelists.sh` | 23 checks |

Menus: actions **102** Product Templates, **103** Attributes, **104**
Pricelists, **105** Price Rules; menus **75–78**. No id collisions
(`verify_menu_ids.sh`); next free are menu 79 / action 106.

---

## 5. Known limits

- **Variants are not yet chosen on an order line.** The line still picks a
  `product.product` directly, which works — variants *are* products — but there
  is no "pick template, then pick size" widget.
- **The pricelist is not yet applied automatically to a sale order line.** The
  engine, the customer field and the order field all exist and are tested;
  wiring it into `SaleOrderLine`'s price default is the remaining step, and it
  belongs with that module's own pricing code rather than bolted on here.
- **No `pricelist` base** — a rule cannot yet be based on another pricelist.
- Attribute values have no colour/radio display type; the UI lists them plainly.

---

## 6. Still open from the docs/093 list

- **Project, tasks and timesheets** — not started.
- **Label and QR printing** — not started.
- **Distributor catalogue lookup** — not started; needs Digi-Key/Mouser/Octopart
  API keys, which are not in this repository, so only the provider abstraction
  and a manual path can be built and tested here.
