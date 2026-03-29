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

## Project structure

- `main.cpp` — entry point, server bootstrap
- `core/` — infrastructure: HTTP server, JSON-RPC dispatcher, session manager, DB connection, container
- `modules/` — feature modules (account, auth, base, hr, ir, mail, mrp, portal, product, purchase, report, sale, stock, uom)
- `web/static/src/app.js` — single-file OWL frontend (no build step; served statically)
- `docs/` — progress and architecture docs
- `3rdparty/` — vendored: drogon, libpqxx, nlohmann/json

## Key notes

- C++20, **split translation units** (PERF-E): each module has a slim `.hpp` (declaration only) and a `.cpp` (implementation + inner classes). `CMakeLists.txt` picks up `*.cpp` from `main.cpp`, `core/`, `modules/`, and `factories/`.
- Frontend is plain JS/OWL — edit `web/static/src/app.js` directly, no npm/webpack needed
- Database: PostgreSQL; schema is created/migrated automatically on startup via `ensureSchema_()` in each module
- Config: `config.json` at project root (DB credentials, HTTP port, devMode flag)

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
Exposing it enables information-disclosure attacks. See `docs/security-error-handling.md`.

`AccessDeniedError` is the **only** exception that is always passed through (user must know why access was denied).

### SEC-29: Allowlist-validate any DB value used in shell commands

Any field read from the database that is interpolated into a shell command must be
validated against a fixed allowlist first:

```cpp
static const std::set<std::string> kAllowed = {"A3","A4","A5","Letter","Legal"};
const std::string safe = kAllowed.count(dbValue) ? dbValue : "default";
```

Never use raw DB values in `std::system()` calls via string concatenation.

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

namespace odoo::modules::xxx {

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

} // namespace odoo::modules::xxx
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

namespace odoo::modules::xxx {
using namespace odoo::infrastructure;
using namespace odoo::core;

// Inner model/viewmodel classes defined here
class XxxFoo : public BaseModel<XxxFoo> { ... };

// All XxxModule:: method implementations here
XxxModule::XxxModule(...) : ... {}
void XxxModule::registerModels() { ... }
// ...
} // namespace odoo::modules::xxx
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
