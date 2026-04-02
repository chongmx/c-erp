# P0 Features — Implementation Plan

**Date:** 2026-04-01
**Status:** Planned
**Priority:** P0 — required for basic production readiness

Seven features currently absent from the ERP that block production deployment. Documented in dependency order with concrete implementation details for this codebase (C++20, CRTP BaseModel, Drogon, pqxx, nlohmann/json, OWL 2 frontend).

---

## Table of Contents

1. [Computed / Related Fields](#1-computed--related-fields)
2. [Onchange Handlers](#2-onchange-handlers)
3. [Automated Test Suite](#3-automated-test-suite)
4. [Versioned DB Migrations](#4-versioned-db-migrations)
5. [Audit Trail / Change Log](#5-audit-trail--change-log)
6. [Optimistic Concurrency Control](#6-optimistic-concurrency-control)
7. [Data Import / Export (CSV)](#7-data-import--export-csv)
8. [Implementation Order](#implementation-order)
9. [Cross-cutting Rules](#cross-cutting-rules)

---

## 1. Computed / Related Fields

**Size:** M
**Depends on:** nothing — implement first

### Problem

Field values derived from other fields (e.g. `amount_total = sum(line subtotals)`, `partner_name = partner_id.name`) are currently computed ad-hoc in individual ViewModel methods with no shared convention. The `fieldsGet()` response does not signal to the frontend that a field is computed/readonly.

### Approach

Two sub-variants:

- **Stored computed** (e.g. `amount_total`): already the pattern. The ViewModel calls `recompute_totals()` after writes. No framework change needed.
- **Non-stored computed** (e.g. `partner_name`): assembled at read time in the ViewModel via a JOIN or second query. Kept out of BaseModel to avoid generic ORM complexity.

### Changes

**`modules/base/FieldRegistry.hpp`** — extend `FieldDef`:

```cpp
struct FieldDef {
    // ... existing members ...
    bool        compute = false;  // true = server-derived, not user-editable
    std::string depends;          // comma-separated trigger fields (informational)
};
```

`toJson()` must emit `"compute": true` when set. This propagates through `fieldsGet()` automatically.

**Per-model ViewModel** — override `handleRead` to inject computed values after the base DB read:

```cpp
nlohmann::json handleRead(const CallKwArgs& call) {
    SaleOrder proto(db_);
    auto rows = proto.read(call.ids(), call.fields());
    for (auto& r : rows) {
        // e.g. fetch partner name with a second query and inject:
        // r["partner_name"] = fetchPartnerName_(r.value("partner_id", 0));
    }
    return rows;
}
```

For simple related fields one level deep, a SQL JOIN in the ViewModel's custom `handleRead` is preferred over a second round-trip.

### Frontend

`FormView` in `app.js` already renders `readonly` fields as `<span>`. No change needed once `"compute": true` appears in `fields_get` — treat it the same as `readonly: true`.

### Schema changes

None for non-stored fields. Stored computed fields that need new columns add them in `ensureSchema_()` via `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`.

---

## 2. Onchange Handlers

**Size:** M
**Depends on:** Feature 1 (`FieldDef.compute` flag)

### Problem

When a field changes in a form (e.g. selecting a product), the UI needs the server to compute derived values (price, UoM, description). Currently the frontend has hardcoded ad-hoc logic for this.

### Protocol

Standard Odoo onchange: client sends `{ method: 'onchange', args: [currentValues, changedFieldName, fieldsSpec] }`. Server returns `{ "value": { field: newVal, ... }, "warning": null }`. Frontend merges `value` into form state.

### Changes

**`core/interfaces/CallKwArgs`** — add `arg(int n)` accessor if missing:

```cpp
nlohmann::json arg(int n) const {
    return (n < (int)args.size()) ? args[n] : nlohmann::json{};
}
```

**Each ViewModel that needs onchange** — register in constructor and implement a dispatcher:

```cpp
// Constructor:
REGISTER_METHOD("onchange", handleOnchange)

// Implementation:
nlohmann::json handleOnchange(const CallKwArgs& call) {
    const auto& values  = call.arg(0);
    std::string changed = call.arg(1).is_string()
        ? call.arg(1).get<std::string>() : "";

    nlohmann::json result = { {"value", nlohmann::json::object()}, {"warning", nullptr} };

    if (changed == "product_id") onchangeProduct_(values, result["value"]);
    if (changed == "partner_id") onchangePartner_(values, result["value"]);
    return result;
}
```

Each `onchange<Field>_()` private helper queries the DB and populates the `value` dict with derived field values to return to the client.

**`app.js`** — in each form view's change handler, fire onchange after updating local state:

```javascript
async triggerOnchange(fieldName) {
    // Debounce 300 ms per field to avoid hammering on text input
    clearTimeout(this._onchangeTimers?.[fieldName]);
    this._onchangeTimers ??= {};
    this._onchangeTimers[fieldName] = setTimeout(async () => {
        try {
            const res = await RpcService.call(
                this.model, 'onchange',
                [this.state.record, fieldName, {}], {});
            if (res?.value) Object.assign(this.state.record, res.value);
            if (res?.warning) alert(res.warning.message);
        } catch (_) {}
    }, 300);
}
```

Call `triggerOnchange(field)` at the end of the delegated `onFieldChange` handler in every form view. Apply the same pattern to o2m line edits.

### Schema changes

None.

---

## 3. Automated Test Suite

**Size:** L
**Depends on:** nothing — can be developed in parallel with Features 1 and 2

### Problem

Zero test files. No regression safety net for any of the other features.

### Approach

Built-in test runner with no external dependencies. Compiles to a separate binary `erp_tests` alongside the main server.

### New files

**`tests/TestRunner.hpp`** — declaration:

```cpp
#pragma once
#include <functional>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> fn; };

class TestRegistry {
public:
    static TestRegistry& instance();
    void add(const std::string& name, std::function<void()> fn);
    int  runAll();  // returns number of failures
};

// Macros
#define ERP_TEST(suite, name) \
    static bool _reg_##suite##_##name = ([]{ \
        TestRegistry::instance().add(#suite "::" #name, []{ suite##_##name(); }); \
    }(), true); \
    void suite##_##name()

#define ASSERT_EQ(a, b)    do { if ((a)!=(b)) throw std::runtime_error(std::string("ASSERT_EQ failed: " #a " != " #b " at " __FILE__ ":") + std::to_string(__LINE__)); } while(0)
#define ASSERT_TRUE(x)     do { if (!(x))     throw std::runtime_error(std::string("ASSERT_TRUE failed: " #x " at " __FILE__ ":") + std::to_string(__LINE__)); } while(0)
#define ASSERT_THROW(expr, ex) do { bool caught = false; try { (expr); } catch (const ex&) { caught = true; } if (!caught) throw std::runtime_error("ASSERT_THROW: expected " #ex " not thrown"); } while(0)
```

**`tests/TestRunner.cpp`** — `runAll()` iterates all registered cases, catches exceptions, prints pass/FAIL, returns failure count.

**`tests/main_test.cpp`** — `int main() { return TestRegistry::instance().runAll() > 0 ? 1 : 0; }`

**`CMakeLists.txt`** — add target:

```cmake
file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
add_executable(erp_tests ${TEST_SOURCES}
    # shared module sources, excluding main.cpp
    modules/base/BaseModel.cpp
    modules/sale/SaleModule.cpp
    # ... etc
)
target_link_libraries(erp_tests pqxx drogon nlohmann_json)
target_include_directories(erp_tests PRIVATE modules/base core ...)
```

**DB test isolation pattern** — each DB test wraps in a rolled-back transaction:

```cpp
pqxx::work txn{conn.get()};
// ... create/read/write operations ...
txn.abort();  // always — zero side effects
```

### Test files (priority order)

| File | Type | Covers |
|------|------|--------|
| `tests/unit/FieldRegistryTest.cpp` | Unit, no DB | `FieldDef`, `toJson()`, duplicate detection |
| `tests/unit/DomainTest.cpp` | Unit, no DB | `domainFromJson().toSql()` output |
| `tests/model/SaleOrderTest.cpp` | Integration | create/read roundtrip, pagination cap |
| `tests/model/OnchangeTest.cpp` | Integration | onchange returns correct derived values |
| `tests/model/ConcurrencyTest.cpp` | Integration | stale `write_date` is rejected |
| `tests/http/ImportExportTest.cpp` | Integration | CSV round-trip, error rows, size limit |

---

## 4. Versioned DB Migrations

**Size:** M
**Depends on:** nothing — infrastructure, implement before Features 5 and 6

### Problem

`ensureSchema_()` uses idempotent DDL (`CREATE TABLE IF NOT EXISTS`, `ADD COLUMN IF NOT EXISTS`). This is safe for additive changes but cannot handle column renames, type changes, index additions, or data migrations.

### Approach

A `schema_migrations` table records applied versions. A `MigrationRunner` class applies pending migrations in order at startup. No external tools.

### New files

**`core/infrastructure/MigrationRunner.hpp`**:

```cpp
#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace odoo::infrastructure { class DbConnection; }

namespace odoo::infrastructure {

class MigrationRunner {
public:
    struct Migration {
        int         version;
        std::string description;
        std::string upSql;   // full SQL to apply; runs in one transaction
    };

    explicit MigrationRunner(std::shared_ptr<DbConnection> db);

    void registerMigration(Migration m);

    // Applies all unapplied migrations in version order.
    // Throws and halts startup on failure.
    void runPending();

private:
    void          ensureMigrationsTable_();
    std::set<int> appliedVersions_();

    std::shared_ptr<DbConnection> db_;
    std::vector<Migration>        migrations_;
};

} // namespace odoo::infrastructure
```

**`core/infrastructure/MigrationRunner.cpp`** — `runPending()` logic:
1. `ensureMigrationsTable_()` — creates the migrations tracking table.
2. Reads all applied version numbers.
3. For each unapplied migration (ascending version order): runs `upSql` in a transaction, then `INSERT INTO schema_migrations(version, description) VALUES (...)`, commits.
4. On failure: rolls back, re-throws (halts startup).

**`core/interfaces/IModule.hpp`** — add default-empty virtual:

```cpp
virtual void registerMigrations(MigrationRunner& /*runner*/) {}
```

**`main.cpp`** — run before `bootAll()`:

```cpp
auto migRunner = std::make_shared<MigrationRunner>(db);
for (auto& mod : container.modules())
    mod->registerMigrations(*migRunner);
migRunner->runPending();
```

### Schema

Created by `MigrationRunner` itself:

```sql
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    description TEXT    NOT NULL,
    applied_at  TIMESTAMP NOT NULL DEFAULT now()
);
```

### Migration numbering convention

Globally sequential integers starting at 1. Each module owns a non-overlapping range, documented in `docs/026-ir-id-registry-and-checklist.md` (extend that file with a migration range table).

```
1–99    core / base
100–199 account
200–299 sale
300–399 purchase
400–499 stock
500–599 mrp
600–699 portal / report
700–799 auth / auth_signup
```

### Transition

Existing `ensureSchema_()` methods are kept and continue to run. New schema changes go exclusively into `registerMigrations()`. `ensureSchema_()` is retired module-by-module as migrations cover the full initial schema.

---

## 5. Audit Trail / Change Log

**Size:** L
**Depends on:** Feature 4 (schema via migration)

### Problem

No record of who changed what or when. Required for compliance and debugging.

### Approach

`audit_log` table. Hook at the ViewModel layer (not BaseModel) to keep BaseModel lean. Modules opt in by passing an `AuditService` when registering their ViewModel.

### New files

**`core/infrastructure/AuditService.hpp`**:

```cpp
#pragma once
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace odoo::infrastructure { class DbConnection; }

namespace odoo::infrastructure {

class AuditService {
public:
    explicit AuditService(std::shared_ptr<DbConnection> db);

    void log(const std::string&     model,
             int                    recordId,
             const std::string&     operation,  // "create" | "write" | "unlink"
             int                    userId,
             const nlohmann::json&  values);    // new vals for create/write; old for unlink
private:
    std::shared_ptr<DbConnection> db_;
};

} // namespace odoo::infrastructure
```

**`modules/base/GenericViewModel.hpp`** — add optional `AuditService` pointer:

```cpp
template<typename TModel>
class GenericViewModel : public BaseViewModel {
public:
    explicit GenericViewModel(std::shared_ptr<infrastructure::DbConnection> db,
                              std::shared_ptr<infrastructure::AuditService> audit = nullptr)
        : db_(std::move(db)), audit_(std::move(audit)) { ... }
    ...
protected:
    std::shared_ptr<infrastructure::AuditService> audit_;

    nlohmann::json handleCreate(const CallKwArgs& call) {
        ...
        int newId = proto.create(v);
        if (audit_) audit_->log(TModel::MODEL_NAME, newId, "create", ctx.uid, v);
        return newId;
    }

    nlohmann::json handleWrite(const CallKwArgs& call) {
        ...
        proto.write(ids, vals);
        if (audit_) audit_->log(TModel::MODEL_NAME, ids[0], "write", ctx.uid, vals);
        return true;
    }

    nlohmann::json handleUnlink(const CallKwArgs& call) {
        if (audit_) {
            auto old = proto.read(ids, {});  // capture before deletion
            audit_->log(TModel::MODEL_NAME, ids[0], "unlink", ctx.uid, old);
        }
        proto.unlink(ids);
        return true;
    }
};
```

Modules opt in during ViewModel registration:

```cpp
// In SaleModule::registerViewModels():
auto audit = std::make_shared<AuditService>(db_);
viewModels_.register<SaleOrderViewModel>("sale.order",
    [db_, audit]{ return std::make_shared<SaleOrderViewModel>(db_, audit); });
```

Start with `sale.order` and `account.move`. Add others incrementally.

### Schema (via migration ~600)

```sql
CREATE TABLE audit_log (
    id         BIGSERIAL    PRIMARY KEY,
    model      VARCHAR(128) NOT NULL,
    record_id  INTEGER      NOT NULL,
    operation  VARCHAR(16)  NOT NULL,  -- create | write | unlink
    user_id    INTEGER,
    values     JSONB,
    created_at TIMESTAMP    NOT NULL DEFAULT now()
);
CREATE INDEX audit_log_model_record ON audit_log(model, record_id);
CREATE INDEX audit_log_user         ON audit_log(user_id);
CREATE INDEX audit_log_created      ON audit_log(created_at);
```

### API

New `AuditLogViewModel` registered as model `"audit.log"`:
- Exposes `search_read` only (no create/write/unlink)
- Filtered by `model` and `record_id` domain

### Frontend

New `AuditLogPanel` OWL component in `app.js` (mirrors existing `ChatterPanel`). Loads from `audit.log` filtered to current record. Embed below `ChatterPanel` in form views for audited models.

---

## 6. Optimistic Concurrency Control

**Size:** M
**Depends on:** Feature 4 (`write_date` migration for tables that lack it)

### Problem

Last-write-wins. Two users editing the same record simultaneously: the second save silently overwrites the first with no warning.

### Approach

Use the existing `write_date` column (already present in `sale_order`, `account_move`, etc.). Client sends back the `write_date` it last saw. Server adds `AND write_date = $N` to the UPDATE WHERE clause. If `affected_rows() == 0`, a concurrent write happened — return 409 Conflict.

No pessimistic lock is ever held.

### Changes

**`core/infrastructure/Errors.hpp`** — add:

```cpp
class ConcurrencyConflictException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
```

**`modules/base/BaseModel.hpp`** — in `write()`, detect the reserved key `__expected_write_date`:

```cpp
nlohmann::json write(const std::vector<int>& ids, nlohmann::json values) {
    // Extract and strip the concurrency hint
    std::string expectedWd;
    if (values.contains("__expected_write_date") &&
        values["__expected_write_date"].is_string()) {
        expectedWd = values["__expected_write_date"].get<std::string>();
        values.erase("__expected_write_date");
    }

    // Always update write_date if the column exists in the registry
    if (fieldRegistry_.has("write_date"))
        values["write_date"] = "now()";  // injected as SQL literal, not param

    // Build SET clause from remaining values
    auto [setCols, paramVec] = buildSetClause_(values);
    std::string sql = "UPDATE " + std::string(TDerived::TABLE_NAME)
        + " SET " + setCols
        + " WHERE id = ANY($" + std::to_string(paramVec.size()+1) + "::int[])";

    if (!expectedWd.empty())
        sql += " AND write_date = $" + std::to_string(paramVec.size()+2);

    // ... execute, then:
    if (res.affected_rows() == 0 && !expectedWd.empty())
        throw ConcurrencyConflictException(
            "Record was modified by another user. Reload to continue.");
    return true;
}
```

**`core/infrastructure/JsonRpcDispatcher.hpp`** — catch before `std::exception`:

```cpp
} catch (const ConcurrencyConflictException& e) {
    return errorResponse_(id, 409, "Conflict", e.what());
}
```

**`app.js`** — in `FormView.onSave()`:

```javascript
async onSave() {
    const vals = { ...this.state.record };
    if (this.state.record.write_date)
        vals.__expected_write_date = this.state.record.write_date;
    try {
        await RpcService.call(model, 'write', [[id], vals], {});
        // reload record to get new write_date
    } catch (err) {
        if (err?.code === 409) {
            // Show non-dismissable banner:
            // "This record was modified by another user.
            //  Your changes are still visible below.
            //  Reload to see the latest version, then re-apply your changes."
        }
    }
}
```

Do **not** auto-reload on conflict — the user may need to reconcile unsaved edits manually.

### Schema

For tables that lack `write_date`, add via migration:

```sql
ALTER TABLE <table> ADD COLUMN IF NOT EXISTS write_date TIMESTAMP DEFAULT now();
```

Also register `write_date` as a `FieldDef` in each model's `registerFields()` so `BaseModel::write()` can detect it.

---

## 7. Data Import / Export (CSV)

**Size:** L
**Depends on:** nothing — independent; implement last for stability

### Problem

No bulk data entry or extraction path. Admins must insert records one by one through the UI.

### Approach

Two plain HTTP routes (not JSON-RPC). Added to a new `ImportExportModule` or to `IrModule::registerRoutes()`.

```
GET  /web/export/{model}   ?fields=f1,f2&domain=[]&limit=1000
POST /web/import/{model}   multipart/form-data, field "file"
```

### New files

**`core/infrastructure/CsvParser.hpp`** — header-only RFC 4180 parser:

```cpp
#pragma once
#include <string>
#include <vector>

// Returns rows of fields. Handles: quoted fields, "" escape, CRLF + LF.
std::vector<std::vector<std::string>> parseCsv(const std::string& text);

// Builds a CSV string from rows. Quotes fields containing commas, quotes, or newlines.
std::string buildCsv(const std::vector<std::vector<std::string>>& rows);
```

**Export handler**:

```cpp
// GET /web/export/sale.order?fields=name,partner_id,amount_total&domain=[]
// Auth: validate session cookie before responding
// SEC-29: validate each field name against FieldRegistry before building SQL
// Response: Content-Type: text/csv
//           Content-Disposition: attachment; filename="sale.order_2026-04-01.csv"
```

Steps:
1. Parse `fields` query param, validate each against `fieldRegistry_.has(f)` — reject unknown fields.
2. Parse `domain` query param (JSON array).
3. `proto.searchRead(domain, fields, std::min(limit, 1000), 0, "id ASC")`.
4. Build CSV: header row = field names, data rows = values.
5. Stream response.

**Import handler**:

```cpp
// POST /web/import/sale.order
// Body: multipart/form-data with field "file" containing the CSV
// SEC-16: reject if file.fileLength() > 5 MB
// Response: {"imported": N, "errors": [{"row": R, "message": "..."}]}
```

Steps:
1. Size check: reject > 5 MB.
2. Parse CSV: row 0 = headers (validate each against `FieldRegistry`).
3. For each data row (1..N):
   - Build `nlohmann::json` object from `{header[i]: row[i]}`.
   - Call `proto.create(values)` inside try/catch.
   - On success: increment `imported`.
   - On error: append `{"row": rowNum, "message": devMode ? ex.what() : "Invalid data"}`.
4. Return summary JSON.

### Frontend

In `ListView` toolbar, add two buttons:

```javascript
// Export
onExport() {
    const fields = this.visibleColumns.map(c => c.name).join(',');
    const model  = this.props.action.res_model;
    window.open(`/web/export/${model}?fields=${encodeURIComponent(fields)}`, '_blank');
}

// Import — trigger hidden file input
onImportClick() { this.fileInput.el.click(); }
async onImportFile(ev) {
    const file = ev.target.files[0];
    if (!file) return;
    const fd = new FormData();
    fd.append('file', file);
    const res = await fetch(`/web/import/${this.props.action.res_model}`,
        { method: 'POST', credentials: 'include', body: fd });
    const data = await res.json();
    // Show modal: "Imported N records" + error list (row number + message)
    this.reload();
}
```

### Security checklist

- **SEC-16**: file size ≤ 5 MB, checked before any parsing.
- **SEC-28**: `ex.what()` in per-row error messages gated behind `devMode`.
- **SEC-29**: every field name from the `fields` query param and CSV header validated against `FieldRegistry` before reaching SQL.

---

## Implementation Order

| # | Feature | Size | Start condition |
|---|---------|------|-----------------|
| 1 | Computed / Related Fields | M | Anytime |
| 2 | Onchange Handlers | M | After Feature 1 (`FieldDef.compute` exists) |
| 3 | Test Suite | L | Anytime — set up in parallel |
| 4 | Versioned DB Migrations | M | Anytime — pure infrastructure |
| 5 | Audit Trail | L | After Feature 4 (needs migration for schema) |
| 6 | Optimistic Concurrency | M | After Feature 4 (`write_date` migration) |
| 7 | CSV Import / Export | L | Anytime — last for stability |

---

## Cross-cutting Rules

These apply to **every** new feature implementation:

| Rule | Requirement |
|------|-------------|
| **SEC-28** | Every `catch` writing to an HTTP response must gate `ex.what()` behind `devMode`. |
| **SEC-29** | Every user-supplied field name or column identifier must be validated against `FieldRegistry` before use in SQL. |
| **S-33** | Every new HTTP route handler that calls `db->acquire()` must add `catch (const PoolExhaustedException&)` above `catch (const std::exception&)`, returning HTTP 503. |
| **PERF-E** | Every new class gets a split `.hpp` (declaration) + `.cpp` (implementation). Heavy includes (`pqxx`, `nlohmann/json`) stay in `.cpp` files only. |
| **PERF-F** | `BaseModel::searchRead` is already capped at `kMaxPageSize = 1000`. Import/export has its own cap of 1000 rows per request. |
| **Test coverage** | Every new feature should have at least one test in `tests/` before the feature is marked done. |
