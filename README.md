# c-erp

A C++20 ERP server. One process serves the JSON-RPC API, the OWL single-page
application, the customer portal, the HR kiosk and the public website, against
PostgreSQL.

**20 modules · 130 tables · 105 models.** Accounting, sales, purchasing, stock,
manufacturing, HR, projects, an electronics parts catalogue, a storage-rental
business, a customer portal and a CMS.

Third-party libraries are vendored under `3rdparty/` — nothing is fetched at
build time. The frontend has **no build step**: plain JavaScript and OWL 2,
served statically.

---

## Quick start

```bash
./scripts/deps/install.sh          # toolchain, wkhtmltopdf, OWL, PostgreSQL
cmake -B ./build
cmake --build ./build
./build/c-erp                      # http://127.0.0.1:8069
```

First boot provisions the whole schema and seeds a company and an admin user
(`admin` / `admin`) — but only when `res_users` is empty.

```bash
./tests/run.sh                     # the whole suite; exit 0 means green
./tests/run.sh --unit              # C++ only: no database, no server
```

## Documentation

**[`docs/`](docs/) describes the system as it is.** Start at
[`docs/README.md`](docs/README.md).

| | |
|---|---|
| [docs/architecture/overview.md](docs/architecture/overview.md) | layers, request path, boot order, the engines in `core/` |
| [docs/architecture/modules.md](docs/architecture/modules.md) | what each of the 20 modules owns |
| [docs/development/conventions.md](docs/development/conventions.md) | the rules every new file follows |
| [docs/reference/database-schema.md](docs/reference/database-schema.md) | every table, by module |
| [docs/reference/http-api.md](docs/reference/http-api.md) | JSON-RPC, the access model, every route |
| [docs/security/README.md](docs/security/README.md) | what is enforced, and where |
| [docs/operations/](docs/operations/) | build, configure, deploy, test, database |
| [`CLAUDE.md`](CLAUDE.md) | the mandatory subset, for agents working in this repo |

Documentation that lives beside what it describes:
[`tests/README.md`](tests/README.md),
[`tests/docs/tooling.md`](tests/docs/tooling.md),
[`scripts/README.md`](scripts/README.md),
[`deploy/README.md`](deploy/README.md).

`docs/deprecated/` is the frozen historical record. Nothing in it describes the
current system.

## Architecture at a glance

```
Browser
  │  GET  /                        → web/static/index.html (OWL app)
  │  POST /web/session/authenticate
  │  POST /web/dataset/call_kw
  ▼
HttpServer (Drogon)
  │   security headers, session cookie
  ▼
JsonRpcDispatcher
  │   tenant → rate limit → auth → model access → context → dispatch
  ▼
ViewModelFactory ──[Transient]──►  XViewModel
  │                                 REGISTER_METHOD dispatch
  ▼
BaseModel<TDerived>
  │   CRTP ORM · FieldRegistry allowlist · Domain → parameterised SQL
  │   RuleEngine record filter · AuditService log
  ▼
DbConnection (libpqxx pool)  ►  PostgreSQL
```

Everything is in namespace `cerp` — `cerp::core`, `cerp::infrastructure`,
`cerp::modules::<module>`.

| Lifetime | Behaviour | Used for |
|---|---|---|
| `Lifetime::Transient` | a new instance per `create()` | ViewModels (per-request state) |
| `Lifetime::Singleton` | one shared instance per key | Services, Views |

All state between requests lives in `SessionManager`.

## Layout

```
main.cpp              entry point, module registration, boot
core/
  interfaces/         IModel, IService, IViewModel, IView, IFactory, IModule
  factories/          BaseFactory, ModelFactory, ServiceFactory,
                      ViewModelFactory, ViewFactory
  infrastructure/     HttpServer, JsonRpcDispatcher, WebSocketServer,
                      DbConnection, SessionManager, MigrationRunner,
                      AuditService, ProcessRunner
  Container.hpp       DI container + AppConfig
  Money · TaxEngine · RuleEngine · StockQuant · IrCron · IrSequence
  ControlPlane · CompanyIdentity · Filestore · DbExplorer · DbBackup
modules/              base auth mail ir account uom product sale purchase hr
                      stock mrp project help bom report portal rental website
web/static/           index.html, portal.html, kiosk.html, src/app.js, components
tests/                the whole suite, one entry point (tests/run.sh)
scripts/              operational only: build, deploy, server, snapshots, seed
tools/                tenant provisioning, migration, the admin console
deploy/               nginx config and the production runbook
db/snapshots/         baseline.dump — what "a clean database" means
docs/                 this system, described
3rdparty/             drogon, nlohmann/json, libpqxx, qrcodegen (vendored)
```

## Dependencies

| Library | Source | Purpose |
|---|---|---|
| `drogon` | `3rdparty/drogon` | HTTP server, WebSocket, routing |
| `nlohmann/json` | `3rdparty/json` | JSON |
| `libpqxx` | `3rdparty/libpqxx` | PostgreSQL C++ client |
| `qrcodegen` | `3rdparty/qrcodegen` | QR codes for labels |
| `libpq` | system (`libpq-dev`) | required by libpqxx |
| `wkhtmltopdf` | system | server-side PDF rendering |

> **Do not install `libpqxx-dev` from apt.** libpqxx is built from source via
> `add_subdirectory`. The system package (7.8) has different exception
> constructor signatures from the bundled version (7.9), which produces
> `undefined reference to … std::source_location` at link time.

> **wkhtmltopdf must be the patched-Qt build** (`0.12.6.1 (with patched qt)`).
> Report footers use `--footer-html`, which stock builds do not support, and the
> failure surfaces only on the first PDF request — not at startup.

## Building

```bash
cmake -B ./build            # configure — first time, or after CMakeLists.txt changes
cmake --build ./build
rm -rf ./build              # full rebuild (also required if CMakeCache.txt is stale)
```

Two things to know:

- **The source list is a glob evaluated at configure time**, over `main.cpp`,
  `core/`, `modules/` and `factories/`. A newly added file needs
  `cmake -B ./build` re-run **once** before it compiles.
- **`erp_tests` is not in the default target.** `cmake --build ./build` stays
  the fast path; `tests/run.sh` builds the test binary explicitly.

`scripts/build.sh` wraps this and also builds `./build/erp-admin`, the
loopback-only operator console. In VS Code, `Ctrl+Shift+P` →
`Terminal: Run Task` → `CMake Build (WSL)` or `CMake Build (Windows)`.

## Configuration

`config/system.cfg` — an INI file, read by `AppConfig::fromFileOrEnv()`. Pass a
different path with `--config`. If the file is absent, configuration falls back
to environment variables (`DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`,
`DB_PASSWORD`, `DB_POOL_SIZE`, `HTTP_HOST`, `HTTP_PORT`, `HTTP_THREADS`).

The keys that matter most:

| Key | Production value | |
|---|---|---|
| `http_interface` | `127.0.0.1` | nginx must be the only ingress — the rate limiter is bypassable otherwise |
| `secure_cookies` | `True` | required whenever HTTPS is served |
| `trusted_proxies` | `127.0.0.1,::1` | whose `X-Forwarded-For` is believed |
| `dev_mode` | *unset* | true leaks `ex.what()` into responses; local machines only |

Full list: [docs/operations/configuration.md](docs/operations/configuration.md).

> `config/system.cfg` is tracked with a placeholder database password in
> plaintext. Untrack it and rotate before any real deployment — see
> [docs/operations/deployment.md](docs/operations/deployment.md).

## Running

```bash
./build/c-erp                            # config/system.cfg
./build/c-erp --config path/to.cfg
./build/c-erp --provision                # provision + migrate every tenant, then exit

./scripts/server.sh --install --start    # as a systemd service
./scripts/server.sh --status --logs -f
```

`GET /healthz` is the liveness probe.

## Users and groups

The admin password is set with a script, not by hand-writing SQL:

```bash
./scripts/set_admin_password.sh
```

Users, groups and per-user company access are managed in the application
(Settings → Users, and Settings → Companies). There are **sixteen** built-in
groups with fixed ids — `BASE_PUBLIC`, `BASE_INTERNAL`, `BASE_ADMIN`,
`SETTINGS_CONFIGURATION`, and a user/manager pair for each of accounting,
sales, purchasing, inventory, manufacturing and HR. They are declared in
`modules/auth/Groups.hpp` and listed in
[docs/reference/id-registry.md](docs/reference/id-registry.md#group-ids-res_groups).

Account creation is admin-only and password resets are admin-issued.

## Adding a module

The short version; the full checklist is in
[docs/development/conventions.md](docs/development/conventions.md).

1. `modules/<name>/XModule.hpp` — **declaration only**, no implementations and
   no heavy includes.
2. `modules/<name>/XModule.cpp` — every method body and every inner class.
3. Implement `IModule`; add DDL to `ensureSchema_()` with
   `CREATE TABLE IF NOT EXISTS` and `ALTER TABLE … ADD COLUMN IF NOT EXISTS`.
4. Seed with `ON CONFLICT (id) DO NOTHING`.
5. Take menu and action ids from
   `bash tests/integration/core/menu-ids/test.sh` — never by reading a table.
6. Register in `main.cpp` **after** every module it depends on:
   ```cpp
   container->addModule<cerp::modules::name::XModule>();
   ```
7. Re-run `cmake -B ./build` once so the glob picks the new files up.

The `.hpp`/`.cpp` split is mandatory, not stylistic: it is what keeps a
one-module change from recompiling the codebase.

## JSON-RPC

```json
POST /web/dataset/call_kw
{
  "jsonrpc": "2.0",
  "method": "call",
  "id": 1,
  "params": {
    "model":  "res.partner",
    "method": "search_read",
    "args":   [[["active", "=", true]]],
    "kwargs": { "fields": ["name", "email"], "limit": 80 }
  }
}
```

Public methods, callable without a session: `authenticate`,
`get_session_info`, `logout`, `list_db`, `server_version`. Everything else
requires a session, and model access is **deny-by-default** — a model on
neither allowlist requires `BASE_INTERNAL`, so a newly registered ViewModel is
never accidentally exposed.

Full surface: [docs/reference/http-api.md](docs/reference/http-api.md).
