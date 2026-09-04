# SEC-28 — never expose `ex.what()` in an HTTP response

**Never pass `ex.what()` into an HTTP response body without gating it behind
`devMode`.**

```cpp
// WRONG — leaks SQL text, table names and query values to the caller
cb(htmlError(500, std::string("Internal error: ") + ex.what()));
return errorResponse_(id, 500, "Server Error", e.what());

// CORRECT — generic in production, full detail in dev, always logged
LOG_ERROR << "[module/route] " << ex.what();
cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
```

---

## Why

`ex.what()` from a pqxx exception carries the **complete PostgreSQL error
text**: the failed statement, table and column names, constraint names, the
values that caused the failure, and internal schema detail.

```
ERROR: duplicate key value violates unique constraint "product_product_barcode_key"
DETAIL: Key (barcode)=(12345678) already exists.
```

From one bad request an attacker learns the table, the column, that there is a
unique constraint on it, and that their probe value already exists. That is the
schema, for free, and it is the expensive half of preparing a targeted attack.

`tests/security/disclosure/error-masking` asserts this holds.

## Where `devMode` comes from

| Context | How to get it |
|---|---|
| JSON-RPC dispatcher | the `devMode_` member, set in the constructor from `ServiceFactory` |
| HTTP route lambdas | `bool devMode = services_.devMode();` **before** the lambda, then capture it |
| `HttpServer::addJsonPost` and friends | already captured from `cfg_.devMode` |

Capture it **once, by value, outside** the lambda. Calling `services_.devMode()`
inside the lambda risks a dangling reference to a factory that has gone out of
scope.

```cpp
void MyModule::registerRoutes() {
    auto db       = db_;
    auto sessions = services_.sessions();
    bool devMode  = services_.devMode();          // once, here

    drogon::app().registerHandler("/my/route/{1}",
        [db, sessions, devMode](const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                const std::string& param)
        {
            try {
                // ... business logic ...
            } catch (const AccessDeniedError& ex) {
                // always shown — the user has to know why
                cb(htmlError(403, ex.what()));
            } catch (const PoolExhaustedException& ex) {
                LOG_ERROR << "[my/route] pool: " << ex.what();
                cb(htmlError(503, "The server is temporarily overloaded. Please retry."));
            } catch (const std::runtime_error& ex) {
                // application-level: record not found, validation.
                // Still gated — even a benign message is an oracle.
                cb(htmlError(404, devMode ? ex.what() : "Record not found"));
            } catch (const std::exception& ex) {
                // could be pqxx, JSON parse, anything
                LOG_ERROR << "[my/route] " << ex.what();
                cb(htmlError(500, devMode ? ex.what() : "An internal error occurred"));
            }
        },
        {drogon::Get});
}
```

Note the order: `PoolExhaustedException` must be caught **above**
`catch (const std::exception&)`, or it is swallowed as a generic 500 and the
client retries against a server that is merely busy.

## The one exception: `AccessDeniedError`

`AccessDeniedError` (`core/infrastructure/Errors.hpp`) is **always** passed
through unconditionally. The user must know why their request was denied. This
is by design and is not a disclosure risk — the message names a permission, not
a schema.

```cpp
} catch (const AccessDeniedError& e) {
    return errorResponse_(id, 403, "Access Denied", e.what());     // always shown
} catch (const std::exception& e) {
    LOG_ERROR << "[rpc] " << e.what();
    return errorResponse_(id, 200, "Server Error",
                          devMode_ ? e.what() : "An internal error occurred");
}
```

## Configuration

`dev_mode` in `config/system.cfg` defaults to **false**. Set it true only on a
developer machine. It is deliberately absent from the production config
template — see [../operations/configuration.md](../operations/configuration.md).

## Checklist

- [ ] Every `catch (const std::exception&)` that writes to a response uses
      `devMode ? ex.what() : "An internal error occurred"`
- [ ] Every `catch (const std::runtime_error&)` that writes to a response uses
      `devMode ? ex.what() : "<safe user-facing message>"`
- [ ] The real detail is logged with `LOG_ERROR` regardless of `devMode`
- [ ] `PoolExhaustedException` → 503, caught above `std::exception`
- [ ] `devMode` captured by value at the top of `registerRoutes()`, not
      resolved inside the lambda
