# Logging — Progress Log (2026-03-23)

Build clean after all changes.

---

## Problem

`logfile` and `log_level` keys were documented as supported in `system.cfg` but were never
actually read into `AppConfig` or applied anywhere. The log directory was never created and
no file was ever opened.

---

## Implementation

### `core/infrastructure/HttpServer.hpp`

**New `HttpConfig` fields:**

```cpp
std::string logFile  = "";      // path to log file, e.g. "log/system.log"
std::string logLevel = "warn";  // trace | debug | info | warn | error | fatal
```

**`HttpServer` constructor — log level:**

Replaced the old boolean `logRequests` check with a string-to-enum mapping:

```cpp
trantor::Logger::LogLevel level = trantor::Logger::kWarn;
if      (lvl == "trace") level = trantor::Logger::kTrace;
else if (lvl == "debug") level = trantor::Logger::kDebug;
else if (lvl == "info")  level = trantor::Logger::kInfo;
else if (lvl == "warn")  level = trantor::Logger::kWarn;
else if (lvl == "error") level = trantor::Logger::kError;
else if (lvl == "fatal") level = trantor::Logger::kFatal;
app.setLogLevel(level);
```

**`HttpServer` constructor — file logger:**

Uses `trantor::AsyncFileLogger` (non-blocking async I/O — no impact on request latency):

```cpp
if (!cfg_.logFile.empty()) {
    // Create log directory if it doesn't exist
    std::filesystem::create_directories(logPath.parent_path());

    // Strip ".log" extension — trantor appends it automatically
    // ("log/system.log" → base "log/system" → file "log/system.log")
    asyncLogger_.setFileName(baseName);
    asyncLogger_.startLogging();
    trantor::Logger::setOutputFunction(
        [this](const char* msg, const uint64_t len) { asyncLogger_.output(msg, len); },
        [this]()                                    { asyncLogger_.flush(); }
    );
}
```

**New private member:**

```cpp
trantor::AsyncFileLogger asyncLogger_;
```

**New includes:**

```cpp
#include <trantor/utils/AsyncFileLogger.h>
#include <filesystem>
```

---

### `core/Container.hpp` — `AppConfig::fromFile`

```cpp
cfg.http.logFile  = get("logfile", "");

// Normalise to lowercase before storing
std::string lvl = get("log_level", "warn");
for (auto& c : lvl) c = std::tolower(c);
cfg.http.logLevel = lvl;
```

---

## Log Levels

| Level | When to use |
|-------|-------------|
| `trace` | Extremely verbose — every internal step. Development only. |
| `debug` | Detailed diagnostic info. Development only. |
| `info`  | Normal operational events (startup, shutdown, connections). |
| `warn`  | Recoverable problems (config fallbacks, retries). **Default.** |
| `error` | Failures that affect a single request but not the whole server. |
| `fatal` | Failures that prevent the server from continuing. |

Each level includes all levels above it (e.g. `info` also emits `warn`, `error`, `fatal`).

---

## `system.cfg` example

```ini
[options]
; ...other settings...

; Log file path — directory is created automatically
logfile = log/system.log

; Minimum log level: trace | debug | info | warn | error | fatal
log_level = info
```

---

## Notes

- **`trantor::AsyncFileLogger`** writes on a background thread — file I/O does not block request handling.
- **Log rotation:** trantor does not rotate by date. It rotates by file size (default ~20 MB). For date-based rotation, use `logrotate` at the OS level.
- **Both stdout and file:** When `logfile` is set, `trantor::Logger::setOutputFunction` replaces the default stdout writer. Logs go to the file only. If you also want stdout, wrap the output function to write both.
- **`.log` extension:** trantor appends `.log` to the base name. The config strips `.log` before passing to `setFileName` to avoid `system.log.log`.
- **Log directory:** created via `std::filesystem::create_directories` — no need to manually `mkdir log/` before starting the server.
