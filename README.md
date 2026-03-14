# odoo-cpp

**AI assisted** C++ port of Odoo's Python backend.  
The OWL/JS/HTML frontend is untouched — this replaces only the Python server while preserving the PostgreSQL schema and JSON-RPC API surface.

## Architecture

```
HTTP Request (JSON-RPC)
        │
        ▼
  HttpServer (Drogon)
        │
        ▼
JsonRpcDispatcher
        │  routes by model name
        ▼
ViewModelFactory ──[Transient]──► XViewModel      (BaseViewModel)
                                       │
                                  REGISTER_METHOD dispatch table
                                       │
ServiceFactory   ──[Singleton]──► XService         (BaseService)
                                       │
                                  BaseModel CRUD + search
                                       │
                                       ▼
                                  DbConnection     (libpqxx pool)
                                       │
                                       ▼
                                   PostgreSQL

ViewFactory      ──[Singleton]──► XView            (BaseView)
                                       │
                                  render(json) → JSON Response
```

## Factory Lifetimes

| Lifetime | Behaviour | Use for |
|---|---|---|
| `Lifetime::Transient` | New instance on every `create()` | ViewModels (per-request state) |
| `Lifetime::Singleton` | One shared instance per key | Services, Views |

```cpp
// Transient — fresh ViewModel per request (default)
auto vm = viewModels->create("res.partner");

// Singleton — shared service instance
auto svc = services->create("partner", Lifetime::Singleton);
```

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
│   └── base/            BaseModule, services, views, viewmodels, models
├── 3rdparty/
│   ├── drogon/          HTTP + WebSocket server (bundled)
│   ├── json/            nlohmann/json (bundled)
│   └── libpqxx/         PostgreSQL C++ client (bundled)
├── scripts/
│   ├── install_dep.sh
│   └── setup_db.sh
├── uploads/
│   └── tmp/             temporary upload storage
├── web/                 static web assets
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

# 2. Install system dependencies and set up libpqxx submodule
bash install_deps.sh

# 3. Build
cmake -B build
cmake --build build -j$(nproc)
```

If you already have a `build/` directory from a previous configure run:

```bash
rm -rf build   # required if CMakeCache.txt has stale libpqxx paths
cmake -B build
cmake --build build -j$(nproc)
```

## Configuration

The server reads configuration from environment variables at startup using `AppConfig::fromEnv()` in `core/Container.hpp`.

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
| `HTTP_DOC_ROOT` | `web/static` | Static file root directory (enable serving static files) |
| `HTTP_INDEX` | `index.html` | Root index file when static serving is enabled |

Note: `HttpConfig` has additional defaults in code: `logRequests = true`, `corsOrigin = "*"`.

```bash
DB_NAME=odoo DB_USER=odoo DB_PASSWORD=secret ./build/c-erp
```

> Tip: set `HTTP_DOC_ROOT=web/static` to serve local frontend files from `web/static`.

## Adding a New Module

1. Create `modules/<name>/` containing `XModel.hpp`, `XService.hpp`, `XView.hpp`, `XViewModel.hpp`, `XModule.hpp`
2. Implement `XModule` inheriting `IModule` with `staticModuleName()`, and the five `register*()` methods
3. In `main.cpp`, add:
   ```cpp
   container->addModule<odoo::modules::name::XModule>();
   ```

### Minimal module skeleton

```cpp
class XModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "name"; }

    XModule(core::ModelFactory& m, core::ServiceFactory& s,
            core::ViewModelFactory& vm, core::ViewFactory& v)
        : models_(m), services_(s), viewModels_(vm), views_(v) {}

    std::string moduleName() const override { return "name"; }

    void registerModels()     override { /* models_.registerCreator(...) */ }
    void registerServices()   override { /* services_.registerCreator(...) */ }
    void registerViews()      override { /* views_.registerView<XView>(...) */ }
    void registerViewModels() override { /* viewModels_.registerCreator(...) */ }
    void registerRoutes()     override {}
private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;
};
```

## JSON-RPC Wire Format

Standard Odoo JSON-RPC 2.0 — the OWL frontend sends requests unchanged:

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

Public methods that bypass session authentication: `authenticate`, `get_session_info`, `logout`, `list_db`, `server_version`.

## Build (WSL / Windows)

Use VS Code tasks for a deterministic shell target.

**WSL (recommended):**  
`Ctrl+Shift+P` → `Terminal: Run Task` → `CMake Build (WSL)`

**Windows (PowerShell):**  
`Ctrl+Shift+P` → `Terminal: Run Task` → `CMake Build (Windows)`

Keep WSL as the default in `.vscode/settings.json` for most development. Update the `build` path in `.vscode/tasks.json` if you change the build directory.