# 107 — The BOM editor and its importer

Status: **done**. `./scripts/run_tests.sh` → 76 passed, 0 failed
(`verify_bom_import.sh`, 38 checks). **Manufacturing → BOM Editor**.

---

## 1. The rule the whole feature is built on

> The importer never decides what a line **is**. It resolves candidates from the
> catalogue, reports what it found, and a person commits.

Same rule as `part.lookup`, for the same reason: a wrong capacitor that lands
silently becomes a board that does not work. Everything below follows from it.

## 2. Phase 1 of docs/105, delivered

`mrp_bom` gains `bom_kind`, `revision`, `revision_of_id`.
`mrp_bom_line` gains `reference_designators`, `note`, `fitted`.

**A kit is forced to a phantom BOM.** `bom_kind = kit` sets `bom_type = phantom`
at creation, because a kit is picked and packed and never manufactured — a kit
with a manufacturing order would reserve components to build something that does
not physically exist (docs/105 §5b). A PCBA gets `normal`: it is made.

## 3. Status is computed on the server, for typed and imported lines alike

Every line carries a severity, and the editor recomputes it after every edit —
so a hand-typed line is checked exactly as hard as an imported one.

| | Meaning |
|---|---|
| **red** — error | No part chosen · quantity ≤ 0 · designator count ≠ quantity · a designator used twice on the same board |
| **yellow** — warning | Several parts matched · a PCBA line with no designators · matched on value+footprint when the MPN was not found |
| plain | Resolved and consistent |

Colour is never the only signal: each row shows a dot, a left edge **and** the
reason spelled out beneath it. A red row that does not say why is just alarming.

**The designator/quantity check is the one that earns its keep.** `C1,C2,C5` with
quantity 4 is the commonest hand-edited BOM error there is, and it becomes an
unpopulated pad or a short order.

**Ranges are expanded and stored expanded.** `R1-R4` becomes `R1,R2,R3,R4` in the
BOM line — the first version validated the expansion but wrote the raw string,
so the count that had just been checked was not the count that got saved. The
test caught it.

## 4. Import: parse → resolve → review → commit

Staged in `mrp_bom_import_line`, which is deliberately **not** `mrp_bom_line`.
Nothing reaches the BOM until commit — asserted directly.

Resolution order, strongest first:

1. **MPN** against `part_manufacturer_info` — exact.
2. **Value + footprint** against `part_parameter` + `part_footprint` — a real
   match but a weaker one, so it warns if an MPN was given and missed.
3. Neither → **error**. Never a guess.

Several matches is a **warning with the candidates listed**, never a silent pick.

**Commit refuses while any line is in error.** A half-imported BOM is worse than
none, because it looks complete. A refused commit leaves the existing lines
untouched — also asserted.

Headers are matched against a table of aliases (Designator/RefDes/Reference,
Qty/Quantity, MPN/Manufacturer Part Number, …). An unrecognised layout is
**refused with guidance** rather than guessed at.

## 5. Where the AI goes

The assistant rail on the right is the seam, and it is deliberately narrow.

A model may supply **the column mapping** for a layout the header matcher does
not recognise, or hand over **already-normalised rows**. It never supplies a
product id.

That line is where it is for a reason: mapping headers is a judgement call that
varies per vendor and is exactly what a model is good at. Choosing which part a
line means is a lookup that has to be **reproducible and reviewable** — if a
model picks it, nobody can tell later why that capacitor and not the other one.

`bom.import describe` returns the contract an agent needs: row fields, header
aliases, known footprints, known units, and the boundary stated in words. The
test asserts that boundary is actually in the response.

Today the rail parses deterministically — paste or drop a CSV, headers matched,
rows resolved, statuses shown. Wiring a model in replaces the parse step, not the
screen.

## 6. Three bugs found while building it

1. **`COALESCE(l.product_id,0)` with no alias.** The result column is named
   `coalesce`, so reading it by name threw — and SEC-28 correctly masked it as
   "An internal error occurred", which is useless for diagnosis. Turning devMode
   on for one call gave the real message immediately.
2. **Expanded designators were validated but not stored** (§3).
3. **A test bug of my own:** the `call` helper wraps `"args":[$3]`, and I passed
   already-bracketed arrays, producing `[[{...}]]`. Every field then read back as
   missing. Worth noting because the symptom — "product_id is required" on a
   call that plainly supplies it — points at the server rather than the caller.

## 7. Not done

- **No alternates / approved manufacturer list** (docs/105 §3.3). It is Phase 2
  and the single thing that would make a BOM usable for purchasing.
- **No where-used report** — "which assemblies use this part" is unanswerable,
  and it is asked every time a component goes end-of-life.
- **No BOM revisions in the UI.** The columns exist; nothing creates Rev B from
  Rev A, and the selection rule (active, lowest sequence) is not implemented, so
  two BOMs on one product still resolve arbitrarily.
- **No file attachment on the BOM** — Gerbers still attach to the product rather
  than to the revision (docs/105 §5c, docs/106).
- The assistant rail has no model behind it yet.
