# odoo-cpp

C++ port of Odoo's Python backend using MVVM architecture.
Frontend (OWL/JS/HTML) is untouched — this replaces only the Python server.

## Architecture

```
HTTP Request (JSON-RPC)
        │
        ▼
  HttpService (Crow)
        │
        ▼
JsonRpcDispatcher
        │  routes by model name
        ▼
ViewModelFactory ──[Transient/Singleton]──► XViewModel  (BaseViewModel)
        │                                        │
        │                               REGISTER_METHOD dispatch table
        │                                        │
        ▼                                        ▼
ServiceFactory  ──[Singleton]──────────► XService       (BaseService)
                                                 │
                                         BaseModel::search/create/write
                                                 │
                                                 ▼
                                          DbConnection (libpqxx pool)
                                                 │
                                                 ▼
                                            PostgreSQL
        │
        ▼
  ViewFactory  ──[Singleton]──────────► XJsonView      (BaseView<T>)
        │
        ▼
  JSON Response
```

## Factory Lifetimes

All factories support two lifetimes:

| Lifetime | Behavior | Use for |
|---|---|---|
| `Lifetime::Transient` | New instance every `create()` call | ViewModels (per-request state) |
| `Lifetime::Singleton` | Single shared instance | Services, Views, DB connections |

```cpp
// Transient — new ViewModel per request (default)
auto vm = viewModelFactory->create("account.move");

// Singleton — shared service instance
auto svc = serviceFactory->create("accounting", Lifetime::Singleton);
```

## Adding a New Module

1. Create `modules/<name>/` with: `XModel.hpp`, `XService.hpp`, `XViewModel.hpp`, `XView.hpp`, `XModule.hpp`
2. In `XModule::register*()` methods, call `container->*Factory->registerCreator(...)`
3. In `main.cpp`, add: `moduleFactory->registerCreator("<name>", [&]{ return make_shared<XModule>(container); })`

## Directory Structure

```
odoo-cpp/
├── core/
│   ├── interfaces/     IModel, IService, IViewModel, IView, IFactory, IModule
│   ├── base/           BaseModel, BaseService, BaseViewModel, BaseView
│   ├── orm/            Domain, FieldRegistry, RecordSet
│   └── factories/      BaseFactory, ModelFactory, ServiceFactory,
│                       ViewModelFactory, ViewFactory, ModuleFactory
├── infrastructure/
│   ├── db/             DbConnection (libpqxx pool)
│   ├── http/           HttpService, JsonRpcDispatcher, SessionManager
│   ├── websocket/      WebSocketService (bus.Bus notifications)
│   └── di/             Container (wires everything)
├── modules/
│   ├── base/           ResPartner, PartnerService, PartnerViewModel, BaseModule
│   ├── account/        (next to port)
│   └── sale/           (after account)
└── main.cpp
```

## Dependencies

| Library | Purpose |
|---|---|
| `nlohmann/json` | JSON parsing and serialization |
| `Crow` | HTTP server and WebSocket |
| `libpqxx` | PostgreSQL client |
| `GTest` | Unit testing |

Install via vcpkg: `vcpkg install nlohmann-json crow libpqxx gtest`
