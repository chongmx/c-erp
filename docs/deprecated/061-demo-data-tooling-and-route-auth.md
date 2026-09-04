# 061 — Demo data tooling, and authentication on the rental routes

**Date:** 2026-08-06
**Status:** ✅ Complete and verified

---

## The security fix that came with it

Adding a destructive endpoint meant looking at how the existing rental routes authenticate.
They did not.

```
/rental/billing/run        POST — creates invoices
/rental/expenses/generate  POST — creates expense entries
/rental/cashflow           GET  — discloses projected revenue
/rental/dashboard          GET  — discloses MRR, receivables, overdue
```

All four were open to anyone who could reach the port. Loopback binding and nginx decide who
can *knock*; they are not access control. Every rental route now runs the same `checkAuth`
shape `ReportModule` established — session cookie, `isAuthenticated()`, else 401.

The test asserts all seven routes refuse an anonymous caller, that the same route **succeeds
with a session** (so the 401s are scoping and not breakage), and that a refused billing run
created no invoices on its way out.

This was self-inflicted — those routes were written in this project — and it is the argument
for checking the surrounding convention before adding a route rather than after.

---

## Demo data: one implementation, two front doors

`modules/rental/RentalDemo.cpp` holds the seed definition. `scripts/seed_rental_demo.sh` is now
a thin wrapper over the HTTP endpoints, and the UI panel calls the same ones.

The script previously carried its own copy of the SQL, and it had **already drifted**: after
`billing_mode` arrived it kept creating every tenancy as a walk-in, which showed as zero MRR
on a visibly 40%-occupied facility. Two definitions of "what the demo facility is" do not stay
in step, and the shell copy is the one nobody remembers to update.

```
Settings → Technical → Demo Data     the UI panel
./scripts/seed_rental_demo.sh        create
                        --clear      remove
                        --status     what exists now
```

Placed under **Technical**, not under Rental: it is a tool for evaluating the module, not part
of running a facility, and a "delete everything" button does not belong in an operator's daily
navigation.

---

## Making a destructive button safe

Three things, in order of how much they actually help:

**1. What counts as demo data is defined once**, narrowly, and read identically by seed and
clear:

```
units      site = 'Demo Warehouse'
contracts  name LIKE 'DEMO/%'
expenses   name IN (a fixed list of seven)
```

**2. The panel shows what exists before you press anything**, so the confirmation is about real
numbers rather than a vague warning.

**3. Removal requires typing `REMOVE`.** An "Are you sure?" on a data-deleting action is a
reflex click, not a decision. The red button is honest signalling; the typed word is what
prevents the accident.

### The control row

`verify_rental_demo.sh` plants a unit and an expense **just outside** the demo set — same
shape, different site — and asserts they survive a clear. If the scope ever widens, that is
what notices. Nothing else in the suite would.

---

## Invoices are kept, deliberately

Generated invoices are posted accounting documents carrying `ir.sequence` numbers. Deleting
them would leave a gap in the invoice series, which is exactly what an auditor asks about. So
clear removes the link rows and leaves the invoices, and the test asserts the invoice count is
unchanged **and** that no dangling links remain.

This collided with migration 813's foreign key — `account_move.rental_contract_id` — which
refused to let the contract be deleted while invoices referenced it.

The FK was right to refuse. For real data that refusal is the feature: a contract with invoices
against it should not silently vanish. So clear **detaches** the kept invoices explicitly
rather than weakening the constraint to `ON DELETE SET NULL`, which would let a real contract
be deleted out from under its invoices too. `invoice_origin` still carries the contract name as
text, so the invoice stays self-describing afterwards.

---

## Two bugs found while building it

**`CURRENT_DATE + $4` is ambiguous.** A bound parameter arrives untyped, so PostgreSQL cannot
choose between `date + integer` and `date + interval` and refuses:
*"operator is not unique: date + unknown"*. Needs `$4::int`. The shell version never hit this
because psql interpolated a literal.

**The test leaked its own control row.** A run that failed part-way left the control expense
behind, so the next run created a second and the "exactly 1 survivor" assertion failed against
2 — reporting a data-destruction bug that had not happened. Both control rows are now cleared
before being planted.

---

## Adding auth broke five suites, correctly

Every suite that drove a rental route was relying on it being open. All five now sign in and
present the session cookie — which is also a better test, because it exercises the path a real
client takes.

Three smaller things surfaced with them:

**Test isolation.** `verify_rental_demo` ends by clearing the demo facility, and
`verify_rental_grid` was reading that facility as ambient context — so the grid failed on a
machine where nothing was wrong. Both were fixed rather than one: the demo suite now **restores
the state it found on entry**, and the grid suite **creates its own probe unit** when the
facility is empty. A suite that changes the world for its neighbours is not isolated just
because it tidied up after itself.

**`python3 - <<PY` cannot also receive piped stdin.** That form reads the *script* from stdin,
so piping curl's JSON into it made Python see an empty document. The checker goes to a file
instead.

**A cookie jar must exist before the call that needs it.** `verify_rental_portal` authenticated
half-way down, after the billing run at the top; the sign-in moved to the top.

---

## Verification

```
verify_rental_demo   31 checks   auth on all 7 routes + the authenticated
                                 counterpart, seed shape and derived states,
                                 idempotency, clear scope, THE control row,
                                 invoice retention, panel registration,
                                 type-to-confirm present, state restored on exit
full suite           26 suites   all green
```
