# 036 — Audit Trail & Optimistic Concurrency Control

**Date:** 2026-04-02
**Status:** Implemented

---

## Overview

Two P0 features implemented:

1. **Optimistic Concurrency Control (OCC)** — `BaseModel::write()` now accepts an `__expected_write_date` sentinel in the values dict; if the DB record has a different `write_date`, the UPDATE matches 0 rows and a `ConcurrencyConflictException` is thrown.  The frontend GenericFormView catches it and shows a conflict banner with a Reload button instead of an error toast.

2. **Audit Trail** — Every `create`, `write`, and `unlink` operation through `GenericViewModel` is recorded to an `audit_log` table.  The table is created automatically by a `MigrationRunner` migration (v1, registered by `IrModule`).  A read-only `AuditLogPanel` OWL component shows the trail per-record on FormViews.

---

## 1. Optimistic Concurrency Control

### Problem

Two users can open the same record, both edit it, and the second save silently overwrites the first without warning.

### Solution

**Server — `BaseModel::write()` (`modules/base/BaseModel.hpp`)**

- Extracts and erases `__expected_write_date` from the values dict before building the SET clause.
- Always injects `write_date = now()` as a literal in the SET clause when the model has a `write_date` field (no extra parameter needed).
- When `__expected_write_date` is present, appends `AND write_date = $N` to the WHERE clause.
- Checks `res.affected_rows() == 0` after `txn.exec()`.  When the OCC guard rejected the update, throws `ConcurrencyConflictException`.

**Server — `Errors.hpp`**

```cpp
class ConcurrencyConflictException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
```

**Server — `JsonRpcDispatcher.hpp`**

New catch block, before `std::exception`:

```cpp
} catch (const ConcurrencyConflictException& e) {
    return errorResponse_(id, 409, "Conflict", e.what(),
                          "odoo.exceptions.ConcurrencyConflict");
```

`errorResponse_` gained an optional `name` parameter (5th arg, default `"odoo.exceptions.UserError"`) so the frontend can distinguish error types.

**Frontend — `rpc.js`**

`call()` now attaches `err.code` and `err.type` to thrown Errors:

```js
const err = new Error(data.error.data?.message || data.error.message);
err.code  = data.error.code;
err.type  = data.error.data?.name || '';
throw err;
```

**Frontend — `FormView` (`app.js`)**

- Added `state.conflictError` (string).
- `onSave()` builds `vals = { ...state.record, __expected_write_date: record.write_date }` and sends it.
- Catches `err.type === 'odoo.exceptions.ConcurrencyConflict'` → sets `state.conflictError` (shows a yellow banner with a **Reload** button).
- All other errors go to `state.error` (existing red banner).
- `onReload()` clears the conflict state and calls `load()`.

---

## 2. Audit Trail

### Problem

No record of who changed what and when.  Compliance and debugging require an immutable audit log.

### Solution

### Files

#### `core/infrastructure/AuditService.hpp` (new)

Singleton declaration following the `RuleEngine` pattern:

```cpp
class AuditService {
public:
    static void initialize(std::shared_ptr<DbConnection> db);
    static AuditService& instance();
    static bool ready();

    void log(const std::string& model, const std::string& operation,
             const std::vector<int>& recordIds, int uid);
};
```

Forward-declares `DbConnection` — no pqxx in header (PERF-E).

#### `core/infrastructure/AuditService.cpp` (new)

`log()` inserts one row per CRUD call:

```sql
INSERT INTO audit_log(model, operation, record_ids, uid, created_at)
VALUES ($model, $operation, $ids::int[], $uid, now())
```

Errors are caught and logged (never re-thrown) so audit logging cannot break the main operation.

#### `modules/base/GenericViewModel.hpp` (modified)

`handleCreate`, `handleWrite`, `handleUnlink` now call `AuditService::instance().log()` after the operation succeeds and `AuditService::ready()` returns true.

`handleCreate` uses `if constexpr (std::is_integral_v<...>)` to extract the new id regardless of whether `proto.create()` returns `int` or `nlohmann::json`.

#### `modules/ir/IrModule.cpp` (modified)

**`AuditLog` model** — CRTP `BaseModel<AuditLog>`, table `audit_log`, fields: `model`, `operation`, `record_ids`, `uid`, `created_at`.

**`AuditLogViewModel`** — read-only ViewModel (no create/write/unlink methods); results are sorted `id DESC`.

**`registerMigrations()`** — registered migration v1 (`create_audit_log`):

```sql
CREATE TABLE IF NOT EXISTS audit_log (
    id          SERIAL  PRIMARY KEY,
    model       VARCHAR NOT NULL,
    operation   VARCHAR NOT NULL,
    record_ids  INTEGER[] NOT NULL DEFAULT '{}',
    uid         INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS audit_log_model_idx   ON audit_log (model);
CREATE INDEX IF NOT EXISTS audit_log_uid_idx     ON audit_log (uid);
CREATE INDEX IF NOT EXISTS audit_log_created_idx ON audit_log (created_at DESC);
```

**`initialize()`** — calls `AuditService::initialize(services_.db())` after `RuleEngine::initialize()`.

**`registerModels()`** — registers `audit.log` creator.

**`registerViewModels()`** — registers `AuditLogViewModel` creator.

#### `core/infrastructure/JsonRpcDispatcher.hpp` (modified)

`audit.log` added to `kAllowed` so any authenticated internal user can read audit entries.

#### `web/static/src/app.js` (modified)

New `AuditLogPanel` component:
- Props: `model`, `recordId`.
- Fetches `audit.log` records via `search_read` filtered by `model`.
- Client-side filters to entries where `record_ids` contains `recordId`.
- Renders operation icons (`+`, `✎`, `✕`), uid, and `created_at`.

---

## 3. Version number ranges (reminder)

| Range | Module |
|-------|--------|
| 1–99  | core / base / ir |
| 100–199 | account |
| 200–299 | sale |
| 300–399 | purchase |
| 400–499 | stock |
| 500–599 | mrp |
| 600–699 | portal / report |
| 700–799 | auth / auth_signup |

---

## 4. Security checklist

| Rule | Status |
|------|--------|
| SEC-25 ConcurrencyConflictException message always passed through | ✓ Thrown directly (user must know why) |
| SEC-28 gate `ex.what()` behind devMode | ✓ Not applicable for OCC — message is user-facing by design |
| Audit log read gated by auth | ✓ `kAllowed` requires `BASE_INTERNAL` (group 2) |
| AuditService errors never propagate | ✓ Caught and logged in `AuditService::log()` |

---

## 5. Files changed

| File | Change |
|------|--------|
| `core/infrastructure/Errors.hpp` | Added `ConcurrencyConflictException` |
| `core/infrastructure/AuditService.hpp` | New — singleton declaration |
| `core/infrastructure/AuditService.cpp` | New — `log()` implementation |
| `core/infrastructure/JsonRpcDispatcher.hpp` | Added `ConcurrencyConflictException` catch + `name` param to `errorResponse_` + `audit.log` in `kAllowed` |
| `modules/base/BaseModel.hpp` | OCC logic in `write()`: extract `__expected_write_date`, inject `write_date=now()`, append `AND write_date=$N` guard, check `affected_rows()` |
| `modules/base/GenericViewModel.hpp` | Audit calls in `handleCreate/handleWrite/handleUnlink` |
| `modules/ir/IrModule.hpp` | Added `registerMigrations()` declaration + `MigrationRunner` forward declaration |
| `modules/ir/IrModule.cpp` | `AuditLog` model, `AuditLogViewModel`, `registerMigrations()` v1, `initialize()` calls `AuditService::initialize()` |
| `web/static/src/services/rpc.js` | `call()` attaches `err.code` and `err.type` to thrown Errors |
| `web/static/src/app.js` | `FormView` OCC: `__expected_write_date`, conflict banner, `onReload()`; `AuditLogPanel` component |

---

## 6. Limitations / future work

- `AuditLogPanel` is defined but not yet embedded into module-specific FormViews (InvoiceFormView, SaleOrderFormView, etc.) — those views have their own custom templates.  Add `<AuditLogPanel model="..." recordId="..."/>` below `<ChatterPanel>` in each form as needed.
- The generic `FormView`'s template does not include `AuditLogPanel` yet (the generic form is used for simple CRUD models without a custom view).  It can be added as a static `components` entry if desired.
- `record_ids` is stored as a PostgreSQL `integer[]`.  The `AuditLog` model's `serializeFields` currently serialises it as a string; a proper array serialiser can be added if the frontend needs to display individual ids as links.
- OCC is only enforced when the client explicitly sends `__expected_write_date`.  Clients that omit it (e.g. direct API callers) get the old last-write-wins behaviour — this is intentional to preserve backward compatibility.
