# 050 — P6: ViewModel Pattern Fix (ARCH-1)

**Date:** 2026-08-03
**Implements:** `045` P6 · `040` §1.2
**Status:** ✅ Complete — build clean, boot check active, full regression green

---

## 1. The defect this closes

S-35 (record rules), S-37 (audit), S-38 (CSV rules) and S-47 (identity audit) were **four
instances of one defect**: behaviour wired into `GenericViewModel` is silently absent from
hand-written ViewModels. Each was found by review, months apart, after shipping. Each retrofit
missed ViewModels written earlier — S-47 existed precisely because S-37's fix was scoped to the
five modules a review happened to name.

The rental module adds ~9 models. Without this, it becomes occurrence number five — on
contracts, invoices and payments, where a missing audit row is a compliance problem rather than
a cosmetic one.

---

## 2. What was built

### `REGISTER_MUTATOR` — audit by construction

`modules/base/BaseViewModel.hpp`. Same shape as `REGISTER_METHOD`, but wraps the handler:

```cpp
REGISTER_MUTATOR("write", handleWrite)
```

The wrapper extracts the user context, captures ids **before** the call (unlink destroys them),
runs the handler, and writes the audit entry on success. A throwing handler writes no row —
correct, since nothing changed.

**What it does not do:** record-rule enforcement. That needs the model prototype
(`proto.setUserContext()`), which `BaseViewModel` cannot reach — `GenericViewModel` can only
because it owns `TModel`. ViewModels built on hand-written SQL still merge the rule domain
themselves. The boot check reports which those are instead of leaving the gap invisible.

### Boot-time compliance check — the enforcement

`Container::verifyViewModelCompliance_()`, run as the last boot stage so every module has
registered. It enumerates every ViewModel, asks `unguardedMutators()` for any
create/write/unlink registered via `REGISTER_METHOD`, and **throws** unless the model is on a
named allowlist.

```
[arch] ViewModel compliance OK — 42 ViewModels checked
```

The allowlist is currently **empty** and deliberately explicit: adding to it is a decision
someone writes down and justifies, not a default.

### Migration of all 23 mutating ViewModels

Every `create`/`write`/`unlink` registration converted, and the ~36 manual
`AuditService::instance().log()` calls removed — audit is now automatic and single-sourced.

**Business-action audits were deliberately kept**: `action_confirm` ×3, `action_cancel` ×3,
`button_validate` ×1. Those are business events, not CRUD, and the mutator wrapper does not
cover them.

---

## 3. The negative control

A guard nobody has seen fail is a guard nobody should trust. Registration for `mail.message`
was temporarily reverted to `REGISTER_METHOD`:

```
[c-erp] Fatal: ViewModel compliance check failed (ARCH-1).
These ViewModels register mutating methods without REGISTER_MUTATOR,
so their changes would not be audited:
    - mail.message (create)
Use REGISTER_MUTATOR("write", handler) instead of REGISTER_METHOD, or add
the model to kAllowed in Container::verifyViewModelCompliance_() with a reason.
```

Boot refused, named the exact model and method, and told the author both ways out. Restored
afterwards.

---

## 4. Two mistakes made during the migration, both caught

Recorded because both were mine and both were the kind that ship quietly.

**Duplicate audit rows.** The first pass converted registrations but left manual `log()` calls
in purchase/stock/sale, so every operation would have written **two** audit rows — a silently
corrupted trail. Found by `scripts/verify_no_double_audit.sh`, which asserts exactly `+1` row
per operation rather than merely "a row appeared".

**Orphaned `if` guards — the worse one.** Removing a `log()` call left its guard behind:

```cpp
if (AuditService::ready() && newId > 0)      // body deleted
return newId;                                 // ← now the guard's body
```

The function returned nothing when the audit service was not ready — undefined behaviour, and
it compiled with only a `-Wreturn-type` warning. 16 of these across three modules. Fixed
line-precisely (delete a guard whose next non-blank line is not a `log()` call) rather than by
another regex, since regex is what created them.

**The lesson:** a regex that deletes a statement can leave its control structure behind. When
removing guarded code, remove the guard and the body as one unit, or verify by compiling — the
build error (`orderId was not declared in this scope`) was the only reason this surfaced at
all.

---

## 5. Verification

```
build                     clean, no warnings
compliance check          42 ViewModels, OK
negative control          boot refuses, names the violation
no-double-audit           exactly +1 row per create/write/unlink
money unit tests          52 checks, 0 failures
round-trip / display      pass          recompute: order totals still update
precision / currency      pass
security / session        pass
ledger integrity          10/10 exact assertions
```

---

## 6. What remains of the original class

`REGISTER_MUTATOR` closes **audit** by construction. Two related gaps are narrowed but not
closed, and the boot check makes them visible rather than silent:

| Gap | State |
|---|---|
| **Record rules** on raw-SQL ViewModels | Still per-ViewModel work. `GenericViewModel` and proto-based ViewModels set the user context; four hand-written SQL ViewModels do not (docs/041 §5) |
| **OCC** (`__expected_write_date`) | Enforced in `BaseModel::write()`, so proto-based paths get it; raw-SQL writers bypass it |

Extending the wrapper to cover these would require `BaseViewModel` to know the model
prototype, which is a larger refactor than P6 justifies. Worth revisiting if a third instance
of the pattern appears.

---

## 7. Rule

| Rule | Requirement |
|---|---|
| **ARCH-1** | Every `create`/`write`/`unlink` handler is registered with `REGISTER_MUTATOR`, never `REGISTER_METHOD`. Enforced at boot; a violation prevents startup. Exceptions go on the named allowlist in `Container::verifyViewModelCompliance_()` with a written reason. |
