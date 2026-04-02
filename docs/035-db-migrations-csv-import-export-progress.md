# 035 — Versioned DB Migrations & CSV Import/Export

**Date:** 2026-04-02
**Status:** Implemented

---

## Overview

Two P0 features implemented:

1. **Versioned DB Migrations** — a `MigrationRunner` that tracks applied migrations in a `schema_migrations` table and applies pending ones in order at startup.
2. **CSV Import/Export** — `GET /web/export/{model}` and `POST /web/import/{model}` HTTP routes on `IrModule`, plus Export/Import buttons in the `ListView` toolbar.

---

## 1. Versioned DB Migrations

### Problem

`ensureSchema_()` uses idempotent DDL (`CREATE TABLE IF NOT EXISTS`, `ADD COLUMN IF NOT EXISTS`). It cannot handle column renames, type changes, index additions, or data migrations, and there is no record of when or whether a schema change was applied.

### Solution

`MigrationRunner` — a lightweight migration runner that:
- Creates `schema_migrations(version INTEGER PRIMARY KEY, description TEXT, applied_at TIMESTAMP)` automatically on first use.
- Applies each registered migration that is not yet in that table, in ascending version order.
- Wraps each migration in its own transaction; rolls back and throws on failure (halting startup with a clear message).

### Files

#### `core/infrastructure/MigrationRunner.hpp` (new)

```cpp
class MigrationRunner {
public:
    struct Migration { int version; std::string description; std::string upSql; };
    explicit MigrationRunner(std::shared_ptr<DbConnection> db);
    void registerMigration(Migration m);
    void runPending();
};
```

Forward-declares `DbConnection` — no pqxx in the header (PERF-E).

#### `core/infrastructure/MigrationRunner.cpp` (new)

Full implementation: `ensureMigrationsTable_()`, `appliedVersions_()`, `runPending()` (sort + apply loop). Uses `db_->acquire(0)` (infinite timeout) since this runs at startup, not inside an HTTP request.

#### `core/interfaces/IModule.hpp` (modified)

New default-no-op virtual:

```cpp
virtual void registerMigrations(odoo::infrastructure::MigrationRunner& /*runner*/) {}
```

Modules override this to register their versioned migrations. Forward declaration keeps pqxx out of IModule.hpp.

#### `core/Container.hpp` (modified)

New `runMigrations_()` private method called between `initializeServices_()` and `initializeModules_()`:

```cpp
void runMigrations_() {
    auto runner = std::make_shared<MigrationRunner>(db);
    for (const auto& name : modules->registeredNames()) {
        auto mod = modules->create(name, core::Lifetime::Singleton);
        mod->registerMigrations(*runner);
    }
    runner->runPending();
}
```

Boot order is now:
1. `modules->bootAll()` — register models/services/views/routes
2. `initializeServices_()` — cross-module service wiring
3. **`runMigrations_()`** — apply pending schema changes ← new
4. `initializeModules_()` — DDL (`CREATE TABLE IF NOT EXISTS`), seeding
5. HTTP route mounting

### Version number ranges

Non-overlapping per-module ranges (globally unique integers):

| Range | Module |
|-------|--------|
| 1–99 | core / base |
| 100–199 | account |
| 200–299 | sale |
| 300–399 | purchase |
| 400–499 | stock |
| 500–599 | mrp |
| 600–699 | portal / report |
| 700–799 | auth / auth_signup |

### How to add a migration

Override `registerMigrations()` in the relevant module:

```cpp
// In SaleModule.cpp:
void SaleModule::registerMigrations(MigrationRunner& r) {
    r.registerMigration({200, "add_sale_order_note",
        "ALTER TABLE sale_order ADD COLUMN IF NOT EXISTS note TEXT"});
    r.registerMigration({201, "add_sale_order_tag_ids_index",
        "CREATE INDEX IF NOT EXISTS sale_order_tag_ids_idx ON sale_order(tag_ids)"});
}
```

The existing `ensureSchema_()` methods are kept as-is. New DDL changes go into `registerMigrations()` instead. `ensureSchema_()` can be retired module-by-module as migrations become the full schema source of truth.

---

## 2. CSV Import/Export

### Problem

No bulk data entry or extraction path. Admins must create/read records one by one through the UI.

### Solution

Two plain HTTP routes registered by `IrModule::registerRoutes()`:

| Route | Method | Purpose |
|-------|--------|---------|
| `/web/export/{model}` | GET | Download records as a CSV file |
| `/web/import/{model}` | POST | Upload a CSV file and create records |

### Files

#### `core/infrastructure/CsvParser.hpp` (new, header-only)

Two functions:

```cpp
// RFC 4180 parser: handles quoted fields, "" escapes, CRLF+LF
std::vector<std::vector<std::string>> parseCsv(const std::string& text);

// Serialiser: quotes fields containing commas, double-quotes, or newlines
std::string buildCsv(const std::vector<std::vector<std::string>>& rows);
```

No external dependencies — pure string manipulation.

#### `modules/ir/IrModule.cpp` — `registerRoutes()` (modified)

**Export route — `GET /web/export/{model}`**

Query params:
- `fields` — comma-separated field names (optional; defaults to all stored non-computed fields)
- `limit` — max rows (default and cap: 1000)

Security:
- Session auth check (returns 401 if unauthenticated)
- SEC-29: each field name from `fields` param is validated against `model->fieldsGet()` before building the query; unknown names are silently skipped
- PERF-F: limit is capped at 1000

Response: `text/csv` attachment with filename `{model}_{date}.csv`.

**Import route — `POST /web/import/{model}`**

Body: `multipart/form-data` with a `file` field containing the CSV. Also accepts a raw `text/csv` body as fallback.

Security:
- Session auth check (returns 401 if unauthenticated)
- SEC-16: file content rejected if > 5 MB (checked twice: on raw body and on extracted content)
- SEC-29: CSV header names validated against `fieldsGet()`; unknown headers are skipped, `id` column is never written
- SEC-28: per-row error messages gate `ex.what()` behind `devMode`

Response JSON:
```json
{ "imported": 5, "errors": [{"row": 3, "message": "Invalid data"}] }
```

Rows that fail `create()` are collected into the `errors` array; import continues to the next row (partial success is allowed).

#### `modules/ir/IrModule.hpp` (modified)

Added two static private helpers:
- `buildExportFilename_(model)` — builds `{model_with_underscores}_{YYYY-MM-DD}.csv`
- `splitFields_(csv)` — splits a comma-separated field list and trims whitespace

#### `web/static/src/app.js` — `ListView` (modified)

Two new toolbar buttons:

**Export CSV:**
```javascript
onExport() {
    const fields = this.columns.map(c => c.name).join(',');
    window.open(`/web/export/${model}?fields=${encodeURIComponent(fields)}`, '_blank');
}
```

Opens the download in a new tab. Uses the currently visible columns as the field list.

**Import CSV:**
```javascript
onImportClick()  // creates a <input type="file"> programmatically, triggers click
async onImportFile(ev)  // POSTs via FormData, shows result in toolbar status span
```

After a successful import the list reloads automatically. Status message clears after 8 seconds. CSS classes `import-ok` / `import-err` for colour styling.

---

## 3. Security checklist

| Rule | Status |
|------|--------|
| SEC-16 file size limit ≤ 5 MB | ✓ Enforced on raw body and extracted file content |
| SEC-28 gate `ex.what()` behind `devMode` | ✓ Both routes; per-row import errors too |
| SEC-29 validate field names against FieldRegistry | ✓ Export `fields` param, import CSV headers |
| S-33 `PoolExhaustedException` → 503 | ✓ Both routes |
| Auth check | ✓ Both routes return 401 if session not authenticated |

---

## 4. Files changed

| File | Change |
|------|--------|
| `core/infrastructure/MigrationRunner.hpp` | New — `MigrationRunner` class declaration |
| `core/infrastructure/MigrationRunner.cpp` | New — full implementation |
| `core/infrastructure/CsvParser.hpp` | New — header-only RFC 4180 parser |
| `core/interfaces/IModule.hpp` | Added `registerMigrations()` virtual method |
| `core/Container.hpp` | Added `runMigrations_()`, called in `boot()` |
| `modules/ir/IrModule.hpp` | Added static helpers declaration |
| `modules/ir/IrModule.cpp` | Implemented `registerRoutes()` with export+import |
| `web/static/src/app.js` | Export/Import buttons + handlers in `ListView` |

---

## 5. Limitations / future work

- No existing module has yet registered any migrations via `registerMigrations()`. The infrastructure is in place; migrations should be added when the first breaking schema change is needed.
- Export does not support Many2one display names — relational fields export the raw integer id, not the display string. A join-based resolver can be added to `handleExport_` when needed.
- Import does not handle Many2one lookups by display name — the CSV must contain the integer id for relational fields.
- The import row limit is effectively whatever the CSV file contains (up to the 5 MB size cap). An explicit row count limit can be added if needed.
