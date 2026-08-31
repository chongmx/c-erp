# LOOKUP_API — the contract between c-erp and a part-lookup agent

Version **1** (`schema_version` in `describe`).

You are writing an agent that browses the web and returns component data. This
is the shape it must speak. Read §1 first — the exchange is three calls, and the
middle one deliberately does **not** write anything.

---

## 0. Why staging exists

An agent that reads datasheets will sometimes be confidently wrong. A wrong
resistance that lands silently in the catalogue is a part somebody solders into
a board. So:

- **`submit` never writes to the catalogue.** It validates, records every doubt
  it has, and parks the result as a proposal.
- **`apply` writes**, and only when a person asks it to.
- Nothing is ever thrown away — a result that fails validation is stored as
  `invalid` with its issues, because an agent that got the unit wrong probably
  still got the datasheet URL right.

---

## 1. The three calls

All three are JSON-RPC over `POST /web/dataset/call_kw`, model `part.lookup`,
authenticated with a session like any other call.

```
describe  →  what vocabulary should I answer in?
submit    →  here is what I found            (staged; nothing written)
apply     →  a human confirmed; write it     (creates/updates the product)
```

### 1.1 `describe`

```jsonc
{"model":"part.lookup","method":"describe","args":[{}],"kwargs":{"context":{...}}}
```

Returns the target vocabulary. **Call this first and answer inside it** — then
nothing has to be fuzzy-matched afterwards.

```jsonc
{
  "schema_version": 1,
  "categories": [ {"id": 52, "path": "All / Electronics / Passives / Resistors / SMD Resistors"}, ... ],
  "units":      [ {"symbol":"kΩ","name":"Kilohm","quantity":"resistance","factor_to_base":1000}, ... ],
  "known_parameters": ["Resistance","Tolerance","Power", ...],
  "footprints": ["0805","SOT-23", ...],
  "value_formats": ["4700","4.7k","4k7","100n","2R2","10 uF"]
}
```

- `categories` are **full paths**, so the agent can pick the most specific one
  it is confident about.
- `units` is the closed set. A symbol not in this list is an **error** on submit.
- `known_parameters` are names already in use — prefer them over synonyms, so
  "Resistance" does not acquire a rival called "Ohms".

### 1.2 `submit`

```jsonc
{"model":"part.lookup","method":"submit","args":[ <LookupResult> ],"kwargs":{"context":{...}}}
```

**LookupResult**

| field | type | required | notes |
|---|---|---|---|
| `query` | string | one of query/mpn | what was searched for |
| `mpn` | string | one of query/mpn | manufacturer part number |
| `manufacturer` | string | no | created as a contact on apply if unknown |
| `name` | string | no | product name; falls back to `mpn`, then `query` |
| `description` | string | no | |
| `category_id` | int | no | from `describe.categories` — preferred |
| `category_path` | string | no | used when `category_id` is absent; matched on the leaf name |
| `footprint` | string | no | applied only if it already exists (see §4) |
| `parameters` | array | no | see below |
| `datasheet_url` | string | no | stored in the payload |
| `source` | string | no | where the data came from — a URL is ideal |
| `confidence` | number | no | 0–1 |

**parameters[]**

| field | type | notes |
|---|---|---|
| `name` | string | **required**; prefer a `known_parameters` entry |
| `value` | string or number | may be written as `4k7`, `100n`, `2R2`, `10 uF`, `0.125` |
| `unit` | string | a `describe.units` symbol; omit for a dimensionless value |

Example:

```jsonc
{
  "query": "4.7k 0805 1% resistor",
  "mpn": "RC0805FR-074K7L",
  "manufacturer": "Yageo",
  "name": "Yageo RC0805FR-074K7L",
  "category_path": "All / Electronics / Passives / Resistors / SMD Resistors",
  "footprint": "0805",
  "datasheet_url": "https://example.com/rc0805.pdf",
  "source": "https://example.com/rc0805.pdf",
  "confidence": 0.92,
  "parameters": [
    {"name": "Resistance", "value": "4k7",  "unit": "Ω"},
    {"name": "Tolerance",  "value": "1",    "unit": "%"},
    {"name": "Power",      "value": "125m", "unit": "W"},
    {"name": "Mounting",   "value": "SMD"}
  ]
}
```

**Response**

```jsonc
{
  "ok": true,          // false when any issue has level "error"
  "id": 42,            // the staged result — pass this to apply
  "state": "pending",  // or "invalid"
  "issues": [
    {"field": "parameters[1].unit", "level": "error",
     "message": "Unknown unit 'furlongs' — see describe.units"}
  ]
}
```

Issue levels: **`error`** — the field cannot be used, and the result is staged as
`invalid`. **`warning`** — usable but doubtful; a reviewer should look.

### 1.3 `apply`

```jsonc
{"model":"part.lookup","method":"apply",
 "args":[{"id":42, "category_id":52, "product_id":0}], ...}
```

`category_id` and `product_id` are the reviewer's overrides — supply
`product_id` to enrich an existing part instead of creating one.

```jsonc
{"ok": true, "product_id": 1607, "parameters": 3}
```

Applying twice is refused. Use **`reject`** (`args:[[42]]`) to mark a result
declined without writing anything.

### 1.4 Listing

`search_read` with `domain: [["state","=","pending"]]` returns the review queue.
`read` with an id returns the full stored payload and its issues.

---

## 2. Values and units — the part that matters

Every parameter is stored **twice**: as the number a person typed, and as
`value_base`, the same quantity in the SI base of its kind. That is the only
reason a range search works across prefixes.

- `4.7` + `kΩ` → base **4700**
- `4700` + `Ω` → base **4700**
- `4k7` + `Ω` → base **4700**

All three match a `4k`–`5k` search. Before this existed, they did not match each
other and the parametric screen was quietly untrustworthy.

### Write the magnitude ONCE

The base is `parseSiValue(value) × unit.factor`. A prefix in the value **and** a
prefix in the unit is applied twice:

- `4k7` + `kΩ` → base **4 700 000** — 4.7 MΩ, not the 4.7 kΩ that was meant

This is the single most common way to get a numerically valid, completely wrong
part into the catalogue, and it is invisible in review unless you are looking
for it: every field reads plausibly. It is what the first live AI lookup did.

Prefer **shorthand value + unprefixed base unit** (`4k7` + `Ω`). It is the form
the parser is strongest on, and there is no second place for a multiplier to
hide.

Payloads submitted through the built-in agent bridge are checked for this and
corrected, with the change reported. Payloads posted directly to `submit` are
**not** — an external agent is trusted to state its own units, so this one is
yours to get right.

**Write values however the datasheet does.** These are all understood:

| written | means |
|---|---|
| `4700` | 4700 |
| `4.7k` | 4700 |
| `4k7` | 4700 — prefix as decimal point |
| `2R2` | 2.2 (R is the resistance stand-in) |
| `3V3` / `6V3` | 3.3 / 6.3 (V likewise, as marked on electrolytics) |
| `100n` | 1e-7 |
| `10p` | 1e-11 |
| `32k768` | 32768 |
| `10 uF` / `10 µF` | 1e-5 |

**`uF` is accepted and canonicalised.** The micro sign does not survive most
tooling, so `uF`/`uH`/`uA`/`us` are read as the µ spelling and stored as it.
`ohm`, `kohm`, `Mohm` likewise resolve to `Ω`, `kΩ`, `MΩ`. Only alternative
spellings of the *same* unit resolve — nothing is guessed, and `C` stays
Coulomb.

### Two things that must NOT be sent as parameter values

| Send instead | Why |
|---|---|
| the **`footprint`** field, not a `package` parameter | `"0603"` reads as the number 603 and the package is gone. Take the list from `describe.footprints`. |
| **two** parameters for a range | `"-55 to 125"` reads as −55 and the upper limit is gone. Send `temperature_min` and `temperature_max`. |

Both are now kept as text rather than silently truncated, but text is not
searchable — the right field is.

`4k7` matters because it is what appears on schematics and in BOMs — it survives
a photocopier when a `.` does not. An agent that only ever emits `4.7k` is fine;
one that passes through `4k7` from a listing is also fine.

**The `unit` fixes the quantity, not just the scale.** A resistance search will
never return a capacitance that happens to share a number.

### Supported quantities

`resistance` · `capacitance` · `inductance` · `voltage` · `current` · `power` ·
`frequency` · `charge` · `time` · `temperature` · `ratio` · `length` · `mass` ·
`gain` · `data`

Always take the exact symbol list from `describe.units` rather than this page.

---

## 3. Category conventions

The tree is a real electronics taxonomy — 86 nodes, e.g.

```
All / Electronics / Passives / Resistors / SMD Resistors
All / Electronics / Semiconductors / Transistors / N-Channel MOSFET
All / Electronics / Semiconductors / Voltage Regulators / Linear Regulators (LDO)
All / Electronics / Electromechanical / Connectors / JST Connectors
```

Pick the **deepest node you are confident about**. A resistor placed in
`Passives` is not wrong, but `SMD Resistors` is what makes the sidebar useful.
If unsure, send `category_path` anyway — an unmatched path becomes a warning and
the reviewer picks from a list, which is better than a silent guess.

---

## 4. What apply will and will not do

**Will**: create the product (or update the one you name); insert or update each
parameter, normalising it; create the manufacturer as a contact if needed and
record the MPN; set the footprint **only if that footprint already exists**.

**Will not**: invent a footprint, invent a category, set prices, create stock,
or touch anything not listed above.

Footprints are deliberately not created from scraped text — that is how a
library fills with `0805`, `0805 `, `SMD0805` and `0805 (2012 Metric)` as four
separate things. Add the footprint first, then re-apply.

---

## 5. Agent guidance

1. **Call `describe` at the start of a session** and cache it. It is small.
2. **Prefer `category_id` over `category_path`.**
3. **Reuse `known_parameters` names.** New names are allowed, but every synonym
   is a parameter that parametric search will not find alongside its siblings.
4. **Never invent a unit.** Omit the unit rather than guess it — a dimensionless
   value is honest, a wrong unit is a wrong number.
5. **Never write the magnitude twice.** `4k7` + `Ω`, or `4.7` + `kΩ`. Never
   `4k7` + `kΩ` — see §2. This is the one mistake that produces a wrong number
   nothing downstream can spot.
6. **Set `confidence` truthfully** and put the URL in `source`. A low-confidence
   result is still worth submitting; it just gets read more carefully.
7. **One part per submit.** Batch by calling it repeatedly.
8. **Expect to be reviewed.** Nothing is lost by being uncertain, and staging is
   not a failure state.

---

## 5b. The proposal state machine

```
submit ──► pending ─────────► applied     (a person said yes)
       └─► invalid  ◄──┐  └─► rejected    (a person said no)
              │        │
              └── update ──┘   re-validated on every save
```

| State | `apply` | `update` | `reject` |
|---|---|---|---|
| `pending` | yes | yes | yes |
| `invalid` | **refused** | yes | yes |
| `rejected` | **refused** | yes → back to `pending` | — |
| `applied` | refused | **refused** | **refused** |

Only `pending` may be applied. `invalid` is the one that matters: a value the
ERP cannot read must never reach a catalogue somebody orders parts from.

`applied` is frozen in every direction — it is the record of what was written,
and editing it would erase the evidence.

`update` re-runs the *same* validation as `submit`, so correcting one field is
not a way to slip a bad one past the checks. It merges onto the stored payload,
so a field you do not send survives. `confidence` is **ignored** by `update`:
it is the agent's statement about its own certainty, and a reviewer rewriting
it destroys the one signal saying how hard the rest needs checking.

## 6. Errors

- Transport/auth failures come back as ordinary JSON-RPC errors.
- `submit` only raises when neither `query` nor `mpn` is present, or the body is
  not an object. Everything else becomes an `issue`.
- `apply`, `update` and `reject` raise on an id that does not exist, on a
  forbidden transition (see §5b), and on an `apply` naming a `product_id` or
  `category_id` that does not exist.
- `apply` raises on an unknown id and on a second apply.

---

## 7. Not in version 1

- No callback the other way — c-erp does not push a "please look this up" job to
  the agent. The agent (or the UI) initiates.
- No attachment upload in the payload; `datasheet_url` is stored but the PDF is
  not fetched. Use `/web/attachment/upload` separately if you want the file.
- No price or stock data is applied, even if the payload carries it.
- No bulk submit endpoint.
