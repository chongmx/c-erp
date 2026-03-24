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
