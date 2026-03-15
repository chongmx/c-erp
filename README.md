# odoo-cpp

**AI assisted** C++ port of Odoo's Python backend.
Replaces the Python server while preserving the PostgreSQL schema and JSON-RPC API surface.
The OWL/JS frontend in `web/static/` talks to this backend directly.

## Architecture

```
Browser
  │  GET /  → web/static/index.html (OWL app)
  │  POST /web/session/authenticate
  │  POST /web/dataset/call_kw
  ▼
HttpServer (Drogon)
  │
  ├─ /web/session/authenticate ──► AuthService::authenticate()
  │                                  PBKDF2-SHA512 verify → SessionManager::create()
  │
  └─ /web/dataset/call_kw ────────► JsonRpcDispatcher
                                       │  routes by model name + session cookie
                                       ▼
                                   ViewModelFactory ──[Transient]──► XViewModel
                                                                          │
                                                                     REGISTER_METHOD dispatch
                                                                          │
                                   ServiceFactory   ──[Singleton]──► XService
                                                                          │
                                                                     BaseModel CRUD + search
                                                                          │
                                                                     DbConnection (libpqxx pool)
                                                                          │
                                                                      PostgreSQL
```

## Factory Lifetimes

| Lifetime | Behaviour | Use for |
|---|---|---|
| `Lifetime::Transient` | New instance on every `create()` | ViewModels (per-request state) |
| `Lifetime::Singleton` | One shared instance per key | Services, Views |

## Directory Structure

```
odoo-cpp/
├── core/
│   ├── interfaces/       IModel, IService, IViewModel, IView, IFactory, IModule
│   ├── factories/        BaseFactory, ModelFactory, ServiceFactory,
│   │                      ViewModelFactory, ViewFactory, ModuleFactory
│   ├── infrastructure/   JsonRpcDispatcher, HttpServer, WebSocketServer,
│   │                      DbConnection, SessionManager
│   └── Container.hpp
├── modules/
│   ├── base/            BaseModule — res.partner
│   └── auth/            AuthModule — res.users, res.groups, res.company, AuthService
├── 3rdparty/
│   ├── drogon/          HTTP + WebSocket server (bundled)
│   ├── json/            nlohmann/json (bundled)
│   └── libpqxx/         PostgreSQL C++ client (bundled)
├── docs/
│   ├── plan.md                   port roadmap
│   ├── progress.md               infrastructure progress
│   └── auth-module-progress.md   auth module progress
├── scripts/
│   ├── install_dep.sh
│   └── setup_db.sh
├── web/
│   └── static/
│       ├── index.html
│       ├── lib/owl.iife.js
│       └── src/
│           ├── app.js              auth gate + main shell
│           ├── app.css
│           ├── services/rpc.js     JSON-RPC + session helpers
│           └── components/
│               ├── LoginPage.js
│               ├── UserMenu.js
│               ├── Dashboard.js
│               ├── PartnerList.js
│               └── FieldsInspector.js
├── build/               out-of-source CMake build directory
├── CMakeLists.txt
├── main.cpp
└── README.md
```

## Dependencies

| Library | Source | Purpose |
|---|---|---|
| `drogon` | `3rdparty/drogon` | HTTP server, WebSocket, routing |
| `nlohmann/json` | `3rdparty/json` | JSON parsing and serialisation |
| `libpqxx` | `3rdparty/libpqxx` | PostgreSQL C++ client |
| `libpq` | system (`libpq-dev`) | PostgreSQL C client (required by libpqxx) |

> **Important:** libpqxx is built from source via `add_subdirectory`.
> Do **not** install `libpqxx-dev` from apt — the system package (7.8) has
> different exception constructor signatures than the bundled version (7.9),
> causing `undefined reference to ... std::source_location` linker errors.

## Setup

```bash
# 1. Clone with submodules
git clone --recurse-submodules <repo-url>
cd odoo-cpp

# 2. Install system dependencies
bash scripts/install_deps.sh

# 3. Build
cmake -B build
cmake --build build -j$(nproc)
```

If you already have a `build/` directory from a previous configure run:

```bash
rm -rf build   # required if CMakeCache.txt has stale paths
cmake -B build
cmake --build build -j$(nproc)
```

## Configuration

Environment variables read at startup via `AppConfig::fromEnv()`:

| Variable | Default | Description |
|---|---|---|
| `DB_HOST` | `localhost` | PostgreSQL host |
| `DB_PORT` | `5432` | PostgreSQL port |
| `DB_NAME` | `odoo` | Database name |
| `DB_USER` | `odoo` | Database user |
| `DB_PASSWORD` | `odoo` | Database password |
| `DB_POOL_SIZE` | `10` | Connection pool size |
| `HTTP_HOST` | `0.0.0.0` | HTTP bind address |
| `HTTP_PORT` | `8069` | HTTP port |
| `HTTP_THREADS` | `4` | Drogon worker threads |
| `HTTP_DOC_ROOT` | `web/static` | Static file root (serves the OWL frontend) |
| `HTTP_INDEX` | `index.html` | Root index file |

```bash
DB_NAME=odoo DB_USER=odoo DB_PASSWORD=secret ./build/c-erp
```

## Authentication

### Default credentials

Seeded automatically on first boot (only when `res_users` is empty):

| Login | Password |
|-------|----------|
| `admin` | `admin` |

The admin user is assigned to the `Administrator` group and has an associated `res_partner` record (`Administrator` / `admin@example.com`).

### Login flow

1. Open `http://localhost:8069` — the OWL app shows a login page.
2. Enter database (`odoo`), login, and password, then click **Sign In**.
3. The frontend POSTs to `/web/session/authenticate`; on success a `session_id` cookie is set.
4. All subsequent `call_kw` requests include the cookie — the server validates it via `SessionManager`.
5. **Sign out** clears the cookie and returns to the login page.

Session survives page reload via `GET /web/session/get_session_info` called on mount.

### Change a user's password

```sql
-- Connect to the database
psql -U odoo -d odoo

-- Generate a new hash first (run the binary with a helper, or use Python):
-- python3 -c "
--   from passlib.hash import pbkdf2_sha512
--   print(pbkdf2_sha512.hash('newpassword'))
-- "

-- Then update:
UPDATE res_users
SET password = '$pbkdf2-sha512$600000$<salt>$<hash>'
WHERE login = 'admin';
```

Or use the C++ `AuthService::hashPassword()` utility directly in a small tool:

```cpp
#include "modules/auth/AuthService.hpp"
#include <iostream>
int main(int argc, char** argv) {
    std::cout << odoo::modules::auth::AuthService::hashPassword(argv[1]) << "\n";
}
```

### Add a new user

```sql
-- 1. Create a partner record
INSERT INTO res_partner (name, email)
VALUES ('Jane Doe', 'jane@example.com')
RETURNING id;   -- note the returned id, e.g. 2

-- 2. Insert the user (replace <hash> with output of hashPassword())
INSERT INTO res_users (login, password, partner_id, lang, tz, active, share)
VALUES ('jane', '<hash>', 2, 'en_US', 'UTC', TRUE, FALSE);

-- 3. Assign to a group (1=Public, 2=Internal User, 3=Administrator)
INSERT INTO res_groups_users_rel (gid, uid)
VALUES (2, (SELECT id FROM res_users WHERE login = 'jane'));
```

### Disable a user (soft-delete)

```sql
UPDATE res_users SET active = FALSE WHERE login = 'jane';
```

Inactive users are rejected at login even with a correct password (`AuthService::authenticate()` checks `active = TRUE`).

### Delete a user permanently

```sql
-- Remove group memberships first (FK constraint)
DELETE FROM res_groups_users_rel
WHERE uid = (SELECT id FROM res_users WHERE login = 'jane');

-- Then delete the user
DELETE FROM res_users WHERE login = 'jane';

-- Optionally remove the partner record too
DELETE FROM res_partner WHERE email = 'jane@example.com';
```

### Groups

| id | name | full_name | Purpose |
|----|------|-----------|---------|
| 1 | Public | Base / Public | Unauthenticated / portal users |
| 2 | Internal User | Base / Internal | Standard employees |
| 3 | Administrator | Base / Admin | Full access |

Seeded by `AuthModule::seedGroups_()` on first boot.

## Adding a New Module

1. Create `modules/<name>/` with `XModel.hpp`, `XService.hpp`, `XView.hpp`, `XViewModel.hpp`, `XModule.hpp`.
2. Implement `XModule` inheriting `IModule` — provide `staticModuleName()` and the five `register*()` methods.
3. Add DDL to `initialize()` using `CREATE TABLE IF NOT EXISTS` (idempotent).
4. Register in `main.cpp` **after** all modules it depends on:
   ```cpp
   container->addModule<odoo::modules::name::XModule>();
   ```

> Boot order matters: modules are initialized in registration order.
> A module that depends on `base` must be registered after `BaseModule`.

### Minimal module skeleton

```cpp
class XModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "name"; }

    XModule(core::ModelFactory& m, core::ServiceFactory& s,
            core::ViewModelFactory& vm, core::ViewFactory& v)
        : models_(m), services_(s), viewModels_(vm), views_(v) {}

    std::string              moduleName()   const override { return "name"; }
    std::vector<std::string> dependencies() const override { return {"base"}; }

    void registerModels()     override { /* models_.registerCreator(...) */ }
    void registerServices()   override { /* services_.registerCreator(...) */ }
    void registerViews()      override { /* views_.registerView<XView>(...) */ }
    void registerViewModels() override { /* viewModels_.registerCreator(...) */ }
    void initialize()         override { /* CREATE TABLE IF NOT EXISTS ... */ }

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;
};
```

## JSON-RPC Wire Format

Standard Odoo JSON-RPC 2.0:

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

Public endpoints (no session required): `/web/session/authenticate`, `/web/session/get_session_info`, `/healthz`.

## Build (WSL / Windows)

**WSL (recommended):**
`Ctrl+Shift+P` → `Terminal: Run Task` → `CMake Build (WSL)`

**Windows (PowerShell):**
`Ctrl+Shift+P` → `Terminal: Run Task` → `CMake Build (Windows)`
