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

- C++20, single translation unit (all headers included from `main.cpp`)
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
