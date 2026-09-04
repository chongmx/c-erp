# 056 — Rental phase 3: unit state derivation and the grid

**Date:** 2026-08-04
**Implements:** `054` phase 3 · `046` §4
**Status:** ✅ Complete and verified

---

## The decision that matters: derivation lives in the database

`rental_unit.state` is derived from the contract lines by a **trigger** (migration 811),
not by C++.

The reason is not performance and not elegance. It is that derived state must not
depend on *which code path wrote the line*. Lines are written by the contract
ViewModel, by the billing engine, by cancellation, and by repair SQL — and
"remember to call `deriveState()` from every one of those" is precisely the class of
bug this project keeps finding:

- `tax_ids_json` existed in the DB and was read by the engine, but was never added to
  `registerFields()`, so every write silently discarded it (`053`)
- `GenericViewModel` kept its manual `AuditService::log()` calls after the
  `REGISTER_MUTATOR` conversion, so every model double-audited (`055`)

Both are the same shape: a step that had to be remembered, and wasn't. A trigger
cannot be forgotten by a future caller.

The split is therefore:

| | |
|---|---|
| **trigger** | state — because it must always be right |
| **C++** (`RentalUnits`) | events — because they need the acting user, which the database does not have |

Events being best-effort is acceptable; state being best-effort is not. Raw SQL that
bypasses the C++ layer still gets correct state, and simply produces no activity-feed
entry — the right trade in that direction.

---

## The two rules a naive derivation gets wrong

**Maintenance and retired are operator facts, not consequences.** The trigger never
overwrites them. Without this, a contract line silently puts a broken locker back into
service.

**Returning from maintenance must RE-DERIVE, not assume `available`.** The unit may
have been let while it was out of service. `closeMaintenance` drops the state to
`available` only to lift the trigger's guard, then re-derives — so a unit that is
still under contract comes back **occupied**. Assuming `available` would offer an
already-let locker to a second tenant.

Both are asserted:

```
    returned to service while still let: occupied
    PASS  re-derived to occupied, not assumed available
```

---

## Verification, driven through several paths on purpose

The claim is "state cannot drift no matter who writes the line", so the test writes
lines through **raw SQL** as well as the API — raw SQL being exactly the path a C++
helper would miss.

```
1  pending line          -> reserved      (via raw SQL)
2  activate              -> occupied
3  end                   -> available
4  DELETE the line       -> available     (the branch easiest to omit; the
                                           symptom is a permanently unlettable unit)
5  MOVE to another unit  -> old released, new claimed
6  maintenance           -> survives line changes in both directions
7  return to service     -> re-derives to occupied, not available
8  create via the API    -> identical result
9  whole-table drift     -> 0
```

17 checks, all passing.

---

## The grid

`web/static/src/components/rental/RentalUnitGrid.js` + `rental.css`, registered with a
single `CUSTOM_VIEWS` entry — the payoff for the P0 refactor in `055`, which replaced
a `t-elif` ladder that needed a rung *and* a matching `isXxxModel` getter per screen.

Following `046` §9: colour **plus glyph plus text label** on every cell, so state is
never colour alone; per-mark hover tooltips with the whole cell as the hit target;
filters in one row above the grid; and a table view that is always available — it is
both the accessible alternative and the obliged relief for the light-mode contrast
WARN recorded in `046` §3.

Colours are the already-validated palette from `046` §3, not re-picked by eye. Dark
mode is declared under `prefers-color-scheme` **and** `:root[data-theme]` in both
directions, so the app's toggle wins either way.

Occupancy excludes retired units from the denominator — they are not lettable stock,
and counting them would understate occupancy permanently.

---

## Two bugs caught before they shipped

### `type_id` arrives as a bare id, not `[id, label]`

The component assumed the reference ERP's many2one pair. This backend returns a bare integer —
`formatCell()` in `app.js` copes with both, and the bare id is the convention here. The
Type column would have rendered blank and the type filter would have matched nothing,
**with no error anywhere**.

Fixed in the component rather than by changing the server-side convention, which would
have touched every model and every consumer. `typeId()` accepts both shapes and
`typeName()` resolves the label from the unit types the grid already loads.

This is the same failure mode as the `tax_ids_json` bug: a shape mismatch across a
boundary that produces silence rather than an error. It was found by asserting the RPC
payload shape, not by reading the code.

### `seq -w` padded inconsistently in the demo seed

`seq -w 1 16` yields `01..16`, but `seq -w 1 8` yields `1..8` — it pads to the width of
the largest value. So zones A and B got `A01`, while C, R and P got `C1`, `R1`, `P1`,
and every later reference to `C01` matched nothing. The symptom was 12 occupied units
where 18 were intended — easy to mistake for intentional demo data.

Replaced with explicit `printf '%02d'`, and the helper now **warns loudly** on a code
that matches nothing instead of returning silently.

---

## Looking at it

`scripts/render_grid_preview.py` renders the real data through the real CSS to a static
page. The verification script checks wiring; it cannot check layout. Per `046` §9 —
*render it and look at it before calling it done* — 45 units across five zones, 40%
occupancy, no label collisions, reflows cleanly.

`scripts/seed_rental_demo.sh` creates that facility (`--clear` removes it). States are
never set directly there; they come from the contract lines through the trigger, which
is the phase-3 behaviour demonstrating itself.

---

## Verification summary

```
verify_rental_unit_state   17 checks   derivation across raw SQL and API paths
verify_rental_grid         24 checks   assets, load order, RPC shapes, state
                                       coverage, dark mode, occupancy rule
full suite                 all green
```

---

## Next: phase 4 — contract lifecycle

`rental.contract` and `rental.contract.line` with activate / add line / end line /
cancel, contract numbering from `ir.sequence`, and per-line `next_period_start` derived
from each line's own start date.
