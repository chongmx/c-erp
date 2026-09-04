# CLAUDE.md — c-erp project

## Build commands

```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -B ./build

# Build
cmake --build ./build

# Clean (full rebuild)
rm -rf ./build
```

## Test commands

The whole suite lives in `tests/`, with one entry point.

```bash
# Everything — unit + integration. This is what CI runs; exit 0 means green.
./tests/run.sh

# Unit only: no database, no server, runs in milliseconds
./tests/run.sh --unit

# A group, or one test, while iterating
./tests/run.sh --group security
./tests/run.sh --only bank
bash tests/integration/account/bank-recon/test.sh

# What would run, in order, and what database state each demands
./tests/run.sh --list

# One unit suite
./tests/run.sh --unit --filter Money
./build/erp_tests Tax
```

`scripts/run_tests.sh` still works — it forwards to `tests/run.sh`.

**Before writing or debugging a test, read `tests/docs/tooling.md`** — every
tool, flag and helper function, with the trap each one hides (`pg()` strips
spaces, `has_error` matches the error *object*, an empty id means the INSERT
failed). `tests/docs/browser-render-checks.md` covers driving a real browser,
which is the only way to catch an OWL template error.

`erp_tests` is **not** part of the default build target — `cmake --build ./build`
stays the fast path. `tests/run.sh` builds it explicitly. Its source list is a
glob over `tests/unit/*/`, evaluated at **configure** time, so a newly added
unit file needs `cmake -B ./build` re-run once before it compiles.

## Database snapshots — the testing workflow

**A clean database means one thing: `db/snapshots/baseline.dump`.** It holds the
schema plus the reference data every module seeds (chart of accounts, journals,
units, footprints, stages, menus, help), one company and the admin user — and no
transactions, no products, no test debris.

```bash
./scripts/make_baseline.sh                    # rebuild the baseline
./scripts/db_snapshot.sh restore db/snapshots/baseline.dump   # reset to clean
./scripts/db_snapshot.sh take   my.dump       # capture any state
./tests/tools/audit_test_leaks.sh             # which tests leave rows behind
./scripts/seed.sh                             # optional demo data — never needed by the suite
```

`tests/run.sh` does this on every run, in order:

1. snapshots the working database to `log/pretest.dump`,
2. loads `baseline.dump` so the run starts from identical data,
3. runs the suite,
4. **restores `log/pretest.dump`** — your data comes back exactly as it was.

Flags: `--no-baseline` (run against the working database), `--keep-db` (skip the
restore; the snapshot is still taken), `--baseline <file>`.

### Writing tests under this workflow

- **Seed your own fixtures.** A clean baseline has zero products and zero
  orders. A script that assumes one exists is not testing what it claims to.
  Running against the baseline is how you find out.
- **Synthesise a snapshot for a corner case.** Build the state you need, dump it
  with `db_snapshot.sh take`, and restore it at the start of the test. That is
  the supported way to test against a large, awkward or historical database
  without carrying it in the working one.
- **Never rely on rows another script left behind.** `audit_test_leaks.sh`
  measures who leaks; the restore stops it accumulating, but a script that reads
  another's debris is still broken.

Both tiers are load-bearing:

- **unit** (`tests/unit/<subject>/*.cpp`, registered with `ERP_TEST`) — pure
  functions only. Never let these acquire a database dependency.
- **integration / functional / security** (`tests/**/test.sh`) — drive the real
  HTTP API against real PostgreSQL. These catch what unit tests structurally
  cannot: migrations, field registration, SQL, and whether the wiring between
  them is connected at all.

**A test is a folder**, holding a `test.sh` and a `meta` that declares its group,
order, database `scenario` and whether it `needs=fixtures`. Order and database
state are declared, not implied by filename. See `tests/README.md` for the
template and the full `meta` reference.

Every test must end with `All checks passed.` or `*** FAILURES ***`. The runner
scores on that line rather than the exit code, and treats a **missing** verdict
as a failure — so a test that dies early can never be scored as a pass.

## Project structure

- `main.cpp` — entry point, module registration (in dependency order), boot
- `core/` — infrastructure (HTTP server, JSON-RPC dispatcher, session manager,
  DB pool, DI container, migration runner) plus the cross-cutting engines:
  `Money`, `TaxEngine`, `RuleEngine`, `StockQuant`, `IrCron`, `IrSequence`,
  `ControlPlane`, `Filestore`, `DbExplorer`, `DbBackup`, `AuditService`
- `modules/` — 20 feature modules: account, auth, base, bom, help, hr, ir, mail,
  mrp, portal, product, project, purchase, rental, report, sale, stock, uom,
  website (`auth` supplies two: `AuthModule` and `AuthSignupModule`)
- `web/static/` — the browser code, no build step. `src/app.js` is the ERP shell
  and its custom forms; `src/components/` one screen each; `portal.html`,
  `kiosk.html` and `website-editor.js` are separate front ends
- `docs/` — **the system as it is**, in `architecture/`, `reference/`,
  `development/`, `security/`, `operations/`, `guides/`. `docs/deprecated/` is
  the frozen historical log — nothing in it describes the current system.
  `docs/temp/` is for work in progress only, and is emptied as it lands
- `tools/` — tenant provisioning, cross-tenant migration, the admin console
- `deploy/` — nginx config and the production runbook
- `3rdparty/` — vendored: drogon, libpqxx, nlohmann/json, qrcodegen
- `tests/` — the whole suite, one entry point (`tests/run.sh`); `tests/tools/` holds
  the by-hand instruments (leak audit, menu doc, ledger integrity, and the two
  doc audits that check `database-schema.md` and every doc link)
- `scripts/` — operational only: `build.sh`, `deploy.sh`, `server.sh`, the database
  snapshot scripts, `seed.sh`. Machine setup is `scripts/deps/install.sh`; dead
  one-offs are parked in `scripts/deprecated/`. See `scripts/README.md`.
  **Nothing test-related belongs here** — a test is a folder under `tests/`.

## Key notes

- C++20, **split translation units** (PERF-E): each module has a slim `.hpp` (declaration only) and a `.cpp` (implementation + inner classes). `CMakeLists.txt` picks up `*.cpp` from `main.cpp`, `core/`, `modules/`, and `factories/`.
- Frontend is plain JS/OWL — edit `web/static/src/app.js` directly, no npm/webpack needed
- Database: PostgreSQL; schema is created/migrated automatically on startup via `ensureSchema_()` in each module
- Config: `config/system.cfg` — an INI file (DB credentials, HTTP bind, `dev_mode`,
  `secure_cookies`, `trusted_proxies`, logging, SMTP). Override the path with
  `--config`. Tenants are separate, in `config/tenants.json`.
- Namespace is `cerp` throughout — `cerp::core`, `cerp::infrastructure`,
  `cerp::modules::<module>`.

## Where to read more

`docs/README.md` is the index. The pages you are most likely to need:

| | |
|---|---|
| `docs/architecture/overview.md` | layers, request path, boot order |
| `docs/architecture/modules.md` | what each module owns |
| `docs/development/conventions.md` | the long form of the rules below |
| `docs/reference/database-schema.md` | every table, by module |
| `docs/reference/http-api.md` | JSON-RPC, the access model, every route |
| `docs/reference/id-registry.md` | menu / action / group ids |
| `docs/security/README.md` | what is enforced and where |
| `docs/operations/` | build, configure, deploy, test, database |
| `tests/docs/tooling.md` | every test tool and the trap each one hides |

**Never pick a menu, action or group id by reading a document.** Run
`bash tests/integration/core/menu-ids/test.sh` — it fails on a collision and
prints the next free id in each space.

## Security rules (mandatory — apply to every new file)

### SEC-28: Never expose `ex.what()` unconditionally in HTTP responses

Every catch block that writes to an HTTP response MUST gate the error detail behind `devMode`:

```cpp
// In registerRoutes(): capture devMode ONCE before the lambda
bool devMode = services_.devMode();

// In every catch block:
} catch (const std::runtime_error& ex) {
    cb(htmlError(404, devMode ? ex.what() : "Record not found"));
} catch (const std::exception& ex) {
    LOG_ERROR << "[module/route] " << ex.what();   // always log
    cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
}
```

`ex.what()` from pqxx contains full SQL text, table names, and schema details.
Exposing it enables information-disclosure attacks. See
`docs/security/error-handling.md`; `tests/security/disclosure/error-masking` asserts it.

`AccessDeniedError` is the **only** exception that is always passed through (user must know why access was denied).

### SEC-29: Allowlist-validate any DB value used in shell commands

Any field read from the database that is interpolated into a shell command must be
validated against a fixed allowlist first:

```cpp
static const std::set<std::string> kAllowed = {"A3","A4","A5","Letter","Legal"};
const std::string safe = kAllowed.count(dbValue) ? dbValue : "default";
```

Never use raw DB values in `std::system()` calls via string concatenation.

### S-49: A column name reaching SQL must be allowlisted, not just charset-checked

Any user-supplied identifier interpolated into SQL — a domain filter field, an `ORDER BY`
column, a `GROUP BY` — must be checked against the model's **registered fields**, not merely
validated for `[A-Za-z0-9_]`. A charset check stops injection but still lets an authenticated
user *name any real column* (e.g. `password`) and read it blind via a `like` filter, one
substring at a time — the SELECT list being restricted does not help, because the leak is in
the `WHERE`.

```cpp
// domain compile: pass the stored-column allowlist
domainFromJson(merged).toSql(&filterableColumns_());   // rejects unregistered columns
// ORDER BY: validateOrder_ already checks fieldRegistry_.has(col)
```

Values are bound (`$N`); it is the column *name* that needs the allowlist. See
`tests/security/injection/domain-field-allowlist` and `docs/security/README.md`.

## Coding conventions (PERF-E — mandatory for every new module)

### Module file split: `.hpp` (declaration) + `.cpp` (implementation)

Every module **must** follow this layout:

**`modules/xxx/XxxModule.hpp`** — declaration only, no implementations:
```cpp
#pragma once
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::xxx {

class XxxModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "xxx"; }
    explicit XxxModule(core::ModelFactory&, core::ServiceFactory&,
                       core::ViewModelFactory&, core::ViewFactory&);
    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;
    void registerModels()     override;
    void registerServices()   override;
    void registerViews()      override;
    void registerViewModels() override;
    void registerRoutes()     override;
    void initialize()         override;
private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;
    void ensureSchema_();
    void seedMenus_();
};

} // namespace cerp::modules::xxx
```

**`modules/xxx/XxxModule.cpp`** — ALL inner classes and ALL method bodies:
```cpp
#include "XxxModule.hpp"          // slim declaration above
#include "BaseModel.hpp"          // heavy includes here, not in .hpp
#include "GenericViewModel.hpp"
#include "BaseViewModel.hpp"
#include "DbConnection.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
// ... etc.

namespace cerp::modules::xxx {
using namespace cerp::infrastructure;
using namespace cerp::core;

// Inner model/viewmodel classes defined here
class XxxFoo : public BaseModel<XxxFoo> { ... };

// All XxxModule:: method implementations here
XxxModule::XxxModule(...) : ... {}
void XxxModule::registerModels() { ... }
// ...
} // namespace cerp::modules::xxx
```

**Why**: enables incremental compilation — changing a module's `.cpp` only recompiles that one TU, not the entire codebase. The heavy headers (`pqxx`, `BaseModel`, `nlohmann/json`) are isolated to `.cpp` files so `main.cpp` compiles quickly.

### Pool exhaustion: catch PoolExhaustedException → 503

```cpp
} catch (const PoolExhaustedException& e) {
    LOG_ERROR << "[route] pool: " << e.what();
    cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
}
```

Add this ABOVE `catch (const std::exception&)` in every HTTP route that acquires a DB connection.
