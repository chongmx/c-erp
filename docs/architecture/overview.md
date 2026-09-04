# Architecture

c-erp is a C++20 ERP server. One process serves the JSON-RPC API, the OWL
single-page application, the customer portal, the HR kiosk and the public
website, against one PostgreSQL database per company.

```
core/     infrastructure — HTTP, JSON-RPC, sessions, DB pool, DI container
          plus the cross-cutting engines: Money, TaxEngine, RuleEngine,
          StockQuant, IrCron, ControlPlane, Filestore
modules/  20 feature modules, each owning its tables, models and routes
web/      the browser code — no build step, served statically
tests/    the whole test suite, one entry point (tests/run.sh)
```

Everything is in namespace `cerp` — `cerp::core`, `cerp::infrastructure`,
`cerp::modules::<module>`.

---

## The request path

```
Browser
  │  GET  /                        → web/static/index.html (OWL app)
  │  POST /web/session/authenticate
  │  POST /web/dataset/call_kw
  ▼
HttpServer (Drogon)                core/infrastructure/HttpServer.hpp
  │   applies security headers, resolves the session cookie
  ▼
JsonRpcDispatcher                  core/infrastructure/JsonRpcDispatcher.hpp
  │   authenticated?  →  model access check  →  route by model name
  ▼
ViewModelFactory ──[Transient]──►  XViewModel
  │                                REGISTER_METHOD dispatch table
  ▼
BaseModel<TDerived>                modules/base/BaseModel.hpp
  │   CRTP ORM: search / searchRead / read_group / create / write / unlink
  │   FieldRegistry allowlist, Domain → parameterised SQL,
  │   RuleEngine record filter, AuditService log
  ▼
DbConnection (libpqxx pool)  ►  PostgreSQL
```

`ViewFactory` answers `get_views` on a parallel path — view metadata, never
record data.

## Boot

`main.cpp` builds a `Container` from `config/system.cfg`, registers the 20
modules **in dependency order**, then calls `boot()`:

```
base → auth → mail → ir → account → uom → product → sale → purchase → hr
     → auth_signup → stock → mrp → project → help → bom → report → portal
     → rental → website
```

For each module, `boot()` runs `registerModels`, `registerServices`,
`registerViews`, `registerViewModels`, `registerRoutes`, then `initialize()`.
`initialize()` is where `ensureSchema_()` runs — every table is created with
`CREATE TABLE IF NOT EXISTS` and extended with `ALTER TABLE … ADD COLUMN IF NOT
EXISTS`, so booting against an existing database is a no-op and booting against
an empty one provisions it in full. Reference data is seeded with
`ON CONFLICT (id) DO NOTHING`.

`main.cpp --provision` (alias `--migrate`) boots, provisions and migrates every
tenant database, then exits without serving. That is what deployment migrations
and `tools/provision_tenant.sh` call.

## The layers

| Layer | Where | Lifetime | Responsibility |
|---|---|---|---|
| `IModule` | `modules/*/XxxModule.{hpp,cpp}` | one per process | registration + schema + seeds |
| Model | `BaseModel<T>` subclasses | Transient | one table: fields, CRUD, SQL |
| ViewModel | `BaseViewModel` / `GenericViewModel` | Transient | one JSON-RPC method map |
| View | `BaseView` subclasses | Singleton | `get_views` arch + field metadata |
| Service | `ServiceFactory` | Singleton | shared state: db, sessions, config |

`ServiceFactory` is the config bus. It carries `db()`, `devMode()`,
`secureCookies()` and `sessions()`. Modules read configuration from it — never
from the environment or a file inside a module constructor.

**All state between requests lives in `SessionManager`.** ViewModels are
transient; one is constructed per request and discarded.

## Cross-cutting engines in `core/`

| File | What it owns |
|---|---|
| `Money.{hpp,cpp}` | exact fixed-point money/price/quantity, int64 at scale 6 |
| `DecimalPrecision` | user-configurable **display** precision (storage stays scale 6) |
| `TaxEngine` | tax computation, including price-included taxes |
| `PaymentAllocation` | open-item allocation across invoices, realised FX |
| `RuleEngine` | `ir.rule` record-level authorization, called by every CRUD path |
| `StockQuant` | the single authority on on-hand quantity and reservation |
| `IrSequence` | document numbering — gapless (caller's txn) or high-concurrency |
| `IrCron` | scheduled jobs (recurring billing, session GC, dunning) |
| `IrModelData` | external-id ↔ database-id mapping |
| `Filestore` | content-addressed attachment bytes, `data/filestore/<h[:2]>/<h>` |
| `ControlPlane` | cross-tenant identity directory (the company switcher) |
| `CompanyIdentity` | the one answer to "who is this company" |
| `DbExplorer` | read-only schema/row introspection for Database Tools |
| `DbBackup` | `pg_dump` / `pg_restore` via argv arrays, never a shell string |
| `LabelRenderer` | part/product labels and QR codes as SVG |
| `MigrationRunner` | ordered, versioned SQL migrations in `schema_migrations` |
| `AuditService` | records every create/write/unlink into `audit_log` |

## Schema evolution

Two mechanisms, both idempotent and both run at boot:

1. **`ensureSchema_()`** — `CREATE TABLE IF NOT EXISTS` plus
   `ALTER TABLE … ADD COLUMN IF NOT EXISTS`. This is the normal path for a new
   table or a new column.
2. **`MigrationRunner`** — numbered migrations recorded in `schema_migrations`,
   each applied once in its own transaction. Use this when the change is a data
   transformation, not a shape change (the Money scale-6 migrations are the
   worked example, `core/MoneyMigrations.cpp`).

## Related

- [modules.md](modules.md) — what each of the 20 modules owns
- [frontend.md](frontend.md) — the browser side
- [multi-company.md](multi-company.md) — tenants, the control plane, record rules
- [../development/conventions.md](../development/conventions.md) — the rules every new file follows
