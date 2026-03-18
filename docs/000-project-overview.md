# Progress Summary

## Project Overview
This repository is an AI-assisted C++ port of Odoo backend, implementing JSON-RPC API via Drogon and libpqxx.

## Folder Structure (as of now)

- `CMakeLists.txt` – root CMake build script.
- `README.md` – project documentation and setup instructions.
- `main.cpp` – application entry point: loads config, builds container, registers modules, boots, runs.
- `core/` – core DI, infrastructure, factories, interfaces.
  - `Container.hpp` – AppConfig, Container class, boot/run/shutdown flow, `AppConfig::fromEnv()`.
  - `infrastructure/` 
    - `DbConnection.hpp` – `DbConfig`, thread-safe connection pool, health check.
    - `HttpServer.hpp` – `HttpConfig`, Drogon wrappers for route registration and static file handling.
    - `JsonRpcDispatcher.hpp` – JSON-RPC route dispatching engine.
    - `SessionManager.hpp` – session storage and handling.
    - `WebSocketServer.hpp` – websocket route wiring.
  - `interfaces/` – interfaces for modules/services/models/views/factories.
  - `factories/` – factories for models/services/views/viewmodels/modules.
- `modules/` – module implementations.
  - `base/` – base domain module with models/services/views/viewmodels and BaseModule.
- `3rdparty/` – vendored dependencies:
  - `drogon/`, `json/`, `libpqxx/`.
- `scripts/` – helper scripts (`install_dep.sh`, `setup_db.sh`).
- `uploads/` – runtime file upload temporary directory.
- `web/` – static web assets (for local static serving with `HTTP_DOC_ROOT`).
- `build/` – out-of-source CMake build artifacts.

## Key Files and Responsibilities

### `main.cpp`
- Instantiates `AppConfig` from env via `AppConfig::fromEnv()`.
- Constructs `Container` with config.
- Registers modules (`BaseModule` currently).
- Boots container and starts HTTP server.
- Handles SIGINT/SIGTERM for graceful shutdown.

### `core/Container.hpp`
- Defines `AppConfig` with `DbConfig` and `HttpConfig`.
- Implements `AppConfig::fromEnv()` environment variable mapping:
  - DB: `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, `DB_PASSWORD`, `DB_POOL_SIZE`.
  - HTTP: `HTTP_HOST`, `HTTP_PORT`, `HTTP_THREADS`, `HTTP_DOC_ROOT`, `HTTP_INDEX`.
- Defines `Container` class with:
  - Infrastructure singletons: `DbConnection`, `HttpServer`, `WebSocketServer`, `SessionManager`, `JsonRpcDispatcher`.
  - Factories: models, services, viewmodels, views, modules.
  - `addModule<T>()`, `addModuleInstance(...)` for module registration.
  - `boot()` for module boot and route wiring.
  - `run()` for DB health check and app startup.
  - `shutdown()`.

### `core/infrastructure/DbConnection.hpp`
- `DbConfig` default values and connection string builder.
- Thread-safe connection pool class (`DbConnection`) with lease object `PooledConnection`.
- `acquire(timeoutMs)` blocking connection leasing.
- Health info JSON for diagnostics.

### `core/infrastructure/HttpServer.hpp`
- `HttpConfig` defaults include host, port, threads, logRequests, corsOrigin, docRoot, indexFile.
- Wraps Drogon app initialization with listeners and static file routing.
- Provides JSON helper route registration (`addJsonPost`, `addJsonGet`).
- Adds built-in `/healthz` endpoint.

### `core/infrastructure/JsonRpcDispatcher.hpp` (existing)
- Provides JSON-RPC routing integration with HTTP server.

### `core/infrastructure/SessionManager.hpp` (existing)
- Session creation/storage; used by JSON-RPC endpoints.

### `core/infrastructure/WebSocketServer.hpp` (existing)
- Provides websocket endpoint wiring to Drogon.

## README Updates Made
- Updated `Directory Structure` to match live repository tree.
- Updated `Configuration` section to exactly match `AppConfig::fromEnv()` defaults and recognized env vars.
  - `DB_PASSWORD` default is `odoo`.
  - `HTTP_DOC_ROOT` default is `web/static`.
  - `HTTP_INDEX` default is `index.html`.
- Added clarity that `logRequests` and `corsOrigin` are currently code defaults, not env mapped.

## Function List (Main Runtime Path)

- `AppConfig::fromEnv()` — load env vars into `DbConfig` + `HttpConfig` with defaults.
- `Container::Container(const AppConfig&)` — create infrastructure and factory instances.
- `Container::addModule<TModule>()` — register module creator for boot.
- `Container::boot()` — boot modules, initialize services, wire RPC/WebSocket routes.
- `Container::run()` — check DB health, start HTTP server, block.
- `Container::shutdown()` — stop HTTP server and shutdown services.
- `DbConnection::acquire(int)` — get pooled connection.
- `DbConnection::isHealthy()` / `healthInfo()` — health diagnostics.
- `HttpServer::addJsonPost` / `addJsonGet` — register JSON endpoints.

## Current Build/Run Status (as of this snapshot)
- Builds with CMake using included submodules.
- Runs a Drogon HTTP server with JSON-RPC and optional static file serving.
- Uses Postgres connection pool via libpqxx.
- Uses modular registration pattern with `addModule` and boot ordering.

## Next Suggested Steps
1. Add per-module docs for module conventions and data model wiring.
2. Add `config.example.env` with recommended environment variables.
3. Add integration test harness for JSON-RPC routes, DB fixtures, and health checks.
