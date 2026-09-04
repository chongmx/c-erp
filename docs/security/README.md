# Security

What is enforced, where it is enforced, and what a new file has to do to
inherit it. Rules are numbered because the code and the tests refer to them by
number.

The penetration tests under `tests/security/` assert that these hold. They are
written to **fail** — a green run means the attack was refused.

```
auth/        session-fixes, unauthenticated
access/      multicompany-hardening, multicompany-isolation, multitenant,
             partner-tenant-isolation
injection/   domain-field-allowlist, sql-surfaces
disclosure/  error-masking
hardening/   nginx-forge, nginx-proxy, security-fixes
website/     site-hardening
```

```bash
./tests/run.sh --group security
```

---

## SQL injection

Values are always bound. It is the **column name** that needs guarding.

### S-49 — a column name reaching SQL must be allowlisted, not charset-checked

Any user-supplied identifier interpolated into SQL — a domain filter field, an
`ORDER BY`, a `GROUP BY` — is checked against the model's **registered fields**,
not merely validated as `[A-Za-z0-9_]`.

A charset check stops injection but still lets an authenticated user *name any
real column* — `password`, say — and read it blind through a `like` filter, one
substring at a time. Restricting the SELECT list does not help: the leak is in
the `WHERE`.

```cpp
domainFromJson(merged).toSql(&filterableColumns_());   // rejects unregistered columns
```

`FieldRegistry` is populated only from C++ `registerFields()` code and enforces
`[a-zA-Z_][a-zA-Z0-9_]*` on every name at boot, so the allowlist itself cannot
be poisoned.

### Order clauses

Sanitised at two layers:

1. **Syntax** — `CallKwArgs::order()` (`core/interfaces/IViewModel.hpp`)
   hand-parses the string and rejects anything that is not `col [ASC|DESC]`
   separated by commas.
2. **Existence** — `BaseModel::validateOrder_()` checks every column against the
   model's `FieldRegistry`.

Never pass a raw `call.order()` to SQL. Route it through `BaseModel::search()`
or `searchRead()`, which call `validateOrder_()` for you.

### Domain clauses

`Domain::toSql()` (`modules/base/Domain.hpp`) binds every value as a pqxx
parameter. Never build a `WHERE` by concatenation — use `domainFromJson()` +
`toSql()`.

### write() and the SELECT list

`write()` and `buildSelectCols_()` both guard field-name concatenation with
`fieldRegistry_.has()`. Never trust a JSON object key as a column name without
that check.

## SEC-28 — never expose `ex.what()` unconditionally

The one rule most likely to be broken by a new file. It has its own page:
**[error-handling.md](error-handling.md)**.

In short: every catch block that writes to an HTTP response gates the detail
behind `devMode`, and logs the real thing with `LOG_ERROR` regardless.
`AccessDeniedError` is the only exception always passed through — the user has
to know why they were denied.

## SEC-31 / S-39 — no `std::system()`

`std::system()` hands its string to `/bin/sh`, so any user-derived substring is
a shell-injection sink — including substrings that *look* pre-validated.
`std::stoi("12$(id)")` returns `12` without throwing, so an id that passed
validation still carried a command substitution into the shell. Quoting is not
a fix: `$(...)` and backticks expand inside double quotes.

`infrastructure::runProcess()` (`core/infrastructure/ProcessRunner.hpp`) passes
an **argv array** straight to `execvp()`. There is no shell and no parsing stage
that could reinterpret an argument. Both external-process call sites — the PDF
renderer and the portal's — use it, and `DbBackup` invokes `pg_dump` /
`pg_restore` the same way.

### SEC-29 — allowlist any DB value that reaches an external command

Even with argv, a value read from the database and passed as an argument is
validated first:

```cpp
static const std::set<std::string> kAllowed = {"A3","A4","A5","Letter","Legal"};
const std::string safe = kAllowed.count(dbValue) ? dbValue : "A4";
```

## Authorization

Three layers, all enforced below the ViewModel.

### Model level — `checkModelAccess_()`

Deny-by-default. Admins bypass; `kAllowed` models need only `BASE_INTERNAL`;
`kRequired` models need a named group; **anything on neither list requires
`BASE_INTERNAL`**, so a newly registered ViewModel is never accidentally
exposed. See [../reference/http-api.md](../reference/http-api.md#model-level-access).

When you add a model with sensitive data, add it to the map in
`JsonRpcDispatcher::checkModelAccess_()`.

### S-30 — record level (`ir.rule`)

**Implemented.** `core/RuleEngine` loads `ir_rule` at startup and `BaseModel`
calls it before every CRUD operation. Global rules are subtractive, group rules
additive; admins bypass; a model with no rule is unrestricted.

Hand-written SQL does not get this for free. A custom `search_read` that never
passes through `BaseModel` would be silently unfiltered — call
`modules/base/RecordRuleSql.hpp` once per custom read. Details in
[../architecture/multi-company.md](../architecture/multi-company.md#record-rules).

### ViewModel write handlers

1. **create / unlink** on a sensitive model: require
   `isAdmin || hasGroup(SETTINGS_CONFIGURATION)`.
2. **write**: a non-admin may only write their own record (`ids == [session.uid]`).
3. **password**: never accept `password` in a generic `write()`. Require a
   dedicated `change_password` handler that validates the old password first.
4. **self-deletion guard** in any `handleUnlink` for user records:

```cpp
for (int id : call.ids())
    if (id == session.uid)
        throw std::runtime_error("Cannot delete your own user account");
```

### Account access policy

Account creation is **admin-only** and password resets are **admin-issued**.
There is no self-service signup path in normal operation.

## Sessions

- 128-bit ids from `RAND_bytes()` (`core/infrastructure/SessionManager.hpp`).
  Never `std::rand`, `mt19937`, or any other PRNG.
- Cookies are `HttpOnly`, `SameSite=Lax`, `Path=/`. Do not relax these.
  `SameSite=Lax` is load-bearing CSRF protection on same-site deployments — it
  keeps the cookie off cross-origin POSTs.
- `Secure` is set when `secure_cookies = True`. **Always set it** in any
  deployment served over HTTPS.
- **S-43** — idle sessions expire after `session_ttl_minutes` (default 60) and
  are reclaimed by the eviction timer.
- The portal has its own store (`portal_sid`, 8-hour TTL) following the same
  patterns.
- Never construct a `SessionManager` inside a ViewModel factory lambda. That
  creates an empty per-request store and every lookup returns `nullopt`. Take
  the shared one from `services_.sessions()`.

## Rate limiting

`authenticate` is limited to **10 failed attempts per client IP per 5-minute
window** (`LoginRateLimiter`). The portal login has its own limiter with the
same shape.

Apply the same pattern to any new credential-accepting endpoint: check
`rateLimiter_.allow(ip)` before doing work, then `recordFailure` /
`recordSuccess`.

> A failure counted only on the success path is not a limiter. `AuthViewModel`
> **throws** on bad credentials, which once made `recordFailure()` unreachable
> on exactly the path that matters — the call now sits in a `catch (...)`.

The limiter is in-memory. A multi-process deployment needs it moved to
PostgreSQL or Redis.

### S-40 — client IP behind a proxy

Behind nginx, `getPeerAddr()` is always `127.0.0.1`, which would make one global
bucket shared by every user. `clientIp_()` trusts `X-Forwarded-For` /
`X-Real-IP` **only** when the immediate peer is listed in `trusted_proxies`.

This is why the app must not be reachable except through nginx: measured,
14 login attempts with rotating forged `X-Real-IP` headers were throttled
through nginx and **0 of 14** were throttled when sent straight to the app.
Direct access lets a client pick its own rate-limit bucket. Bind loopback and
firewall 8069 and 5432 shut.

## Response headers

Every JSON response carries:

```
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
Referrer-Policy: strict-origin-when-cross-origin
Content-Security-Policy: default-src 'none'
```

Applied by `HttpServer::applySecurityHeaders_()` inside every handler helper.
A handler registered via `addJsonPost` / `addJsonPostWithResponse` /
`addJsonGet` inherits them — but those helpers carry only the `/web/...`
JSON-RPC surface, registered by `JsonRpcDispatcher`.

**Every route a module registers bypasses them**, going straight to
`drogon::app().registerHandler()`, and must write the headers itself. The
portal and the public site both do; a new module route will not unless you make
it.

- **HSTS** belongs at the reverse proxy, not here: the application cannot know
  whether it is behind TLS termination.
- **Static files** served by Drogon's `setDocumentRoot()` bypass the helper.
  Add the headers at the proxy for production.

## CORS

`cors_origin` defaults to empty — no `Access-Control-Allow-Origin` is sent,
which is correct when the frontend is served from the same origin. Set it only
for a separate dev server, and to an exact origin
(`http://localhost:3000`). **Never `*` in production.**

## File uploads (SEC-16 / SEC-19)

Before saving an uploaded file:

1. Size — reject over 10 MB.
2. Basename — strip every `/` and `\` path component.
3. Extension — lowercase, allowlisted (`.pdf`, `.jpg`, `.jpeg`, `.png`).
4. Path — `data/upload_dir/{id}_{timestamp}_{baseName}`.
5. Store the basename in the database, never the raw filename.

Attachment **bytes** go through `core/Filestore`, which is content-addressed:
the path is `data/filestore/<sha256[:2]>/<sha256>`. The hash *is* the name, so
a filename from a request never touches the path and traversal is impossible by
construction — and identical content stored twice costs one file.

## Database tools

`DbExplorer` runs every query inside a `pqxx::read_transaction`. A crafted
`WITH … INSERT` data-modifying CTE is refused by PostgreSQL itself rather than
by a string check we wrote — the parser-level checks are a second line, not the
first. Table and column names arriving from the request are allowlisted (S-49).

`DbBackup` is admin-gated, requires password re-confirmation, and is passed only
the caller's own tenant `DbConfig`, so one company can never dump or restore
another's database.

## The public website

Page content is **typed blocks rendered by the server**, not author markup, so
the ordinary block set has no XSS surface at all. Two consequences worth
knowing:

- The `html` block is the one that carries markup. It is sanitised on render,
  and the caller is responsible for only letting an administrator author it.
- A block type the renderer does not understand is **skipped**, not passed
  through — content it cannot vouch for is not emitted.

`tests/security/website/site-hardening` covers this surface.

## Checklist for a new endpoint or ViewModel method

- [ ] Domain / `WHERE`: `domainFromJson()` + `toSql()` with the column allowlist
- [ ] `ORDER BY`: routed through `BaseModel::search()` / `searchRead()`
- [ ] Field names in `write()`: guarded with `fieldRegistry_.has()`
- [ ] New model fields: ASCII identifiers only (enforced at boot)
- [ ] String values: bound as pqxx params, never concatenated
- [ ] New credential endpoint: `LoginRateLimiter` pattern, failure counted on the throw path
- [ ] Every new `catch`: the `devMode` ternary, plus `LOG_ERROR`
- [ ] `PoolExhaustedException` → 503, **above** `catch (const std::exception&)`
- [ ] Handler registered directly with drogon: apply security headers yourself
- [ ] External process: `runProcess()` with an argv array; DB values allowlisted
- [ ] Custom SQL read: `RecordRuleSql` so `ir.rule` is still enforced
- [ ] Sensitive model: added to `checkModelAccess_()`
