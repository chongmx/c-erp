# HTTP API

One server, five surfaces:

| Prefix | Audience | Auth |
|---|---|---|
| `/web/...` | the ERP application | `session_id` cookie |
| `/portal/...` | customers | `portal_sid` cookie (separate store, 8 h) |
| `/kiosk/...` | a shared HR tablet | none — device-scoped |
| `/site/...` | the public | none |
| `/healthz`, `/robots.txt`, `/sitemap.xml` | infrastructure | none |

---

## 1. JSON-RPC — `POST /web/dataset/call_kw`

Everything the ERP application does goes through this one endpoint.

```jsonc
{
  "jsonrpc": "2.0",
  "method": "call",
  "params": {
    "model":  "sale.order",
    "method": "search_read",
    "args":   [[["state","=","draft"]], ["id","name","amount_total"]],
    "kwargs": { "limit": 80, "order": "date_order desc", "context": {} }
  }
}
```

`POST /web/dataset/call` is the same dispatcher with positional arguments.

### What the dispatcher does, in order

1. **Resolve the tenant** — explicit `db` argument → `Host` subdomain → login
   email domain → default. The request's DB connections are bound to it for its
   duration.
2. **Rate-limit `authenticate`** — 10 failures per client IP per 5-minute
   window, keyed on the real client IP (`X-Forwarded-For` from a trusted proxy
   only). Over the limit → `429`.
3. **Require a session** unless the method is public.
4. **Model-level access check** (below).
5. **Overwrite the context** with the caller's identity — `uid`, `session_id`,
   `company_id`, `partner_id`, `is_admin`, `group_ids`, `allowed_company_ids`.
   **Assigned, not merged**: a client that puts its own `allowed_company_ids`
   in the request body has it overwritten before any model sees it.
6. **Publish the ambient `UserContext`**, so a model reached through a
   hand-written ViewModel that never calls `setUserContext` is still
   company-scoped.
7. **Dispatch** — `get_views`, `read_group` and `fields_get` are answered
   centrally; everything else goes to the model's ViewModel.

### Methods answered centrally

| Method | Served by | Why |
|---|---|---|
| `get_views` | `ViewFactory` | when the ViewModel does not implement it |
| `read_group` / `web_read_group` | the **model**, not the ViewModel | so every model with a field registry gets grouping, including hand-written ones |
| `fields_get` | cached, 300 s TTL | pure metadata |

### Methods a ViewModel registers

Standard, via `REGISTER_METHOD` / `REGISTER_MUTATOR`:

```
search_read   web_search_read   search   search_count
read          web_read          fields_get
create        write             unlink
default_get   onchange
```

`search_read` and `web_search_read` map to the same handler, as do `read` and
`web_read`. They return **plain record arrays** — `[{id, field, …}]` — never a
`{arch, fields, record}` wrapper; that shape belongs to `get_views` alone.

Document-specific actions are registered the same way:
`action_confirm`, `action_cancel`, `action_post`, `action_approve`,
`action_refuse`, `action_reset_draft`, `action_register_payment`,
`button_validate`, `traceability`, `unpack`, and so on.

### Public methods

Callable without a session:

```
authenticate   get_session_info   logout   list_db   server_version
```

### Model-level access

`checkModelAccess_()` is **deny-by-default**:

1. Admins bypass every check.
2. Models on the `kAllowed` list need only a logged-in internal user
   (`BASE_INTERNAL`, group 2) — menus, actions, currencies, partners, users,
   UoM, products and categories, the parts tables, chatter, the audit log and
   the decimal-precision table.
3. Models in `kRequired` need a specific group — accounting models want
   `ACCOUNT_BILLING` (5), stock wants `INVENTORY_USER` (11), sale wants
   `SALES_USER` (7), purchase `PURCHASE_USER` (9), MRP `MRP_USER` (13), HR
   `HR_EMPLOYEE` (15), `res.company` / `res.groups`
   `SETTINGS_CONFIGURATION` (4), `ir.config.parameter` `BASE_ADMIN` (3).
4. **Anything on neither list requires `BASE_INTERNAL`.** A newly registered
   ViewModel is therefore never accidentally exposed.

A failure raises `AccessDeniedError`, which is the one exception whose message
is always passed through to the client — the user has to know why.

Record-level filtering happens below this, in `RuleEngine`; see
[../architecture/multi-company.md](../architecture/multi-company.md#record-rules).

## 2. Session and company

| Route | Purpose |
|---|---|
| `POST /web/session/authenticate` | log in; sets the `session_id` cookie |
| `POST /web/session/get_session_info` | who am I |
| `POST /web/session/my_companies` | companies I may switch to (one database) |
| `POST /web/session/set_active_company` | switch within one database |
| `POST /web/session/consolidated` | read across all allowed companies |
| `POST /web/session/companies` | tenants I belong to (control plane) |
| `POST /web/session/lookup_companies` | the same, **before** login |
| `POST /web/session/switch_company` | mint a new session in another tenant |
| `POST /web/session/import_shared_products` | pull the shared catalogue in |
| `POST /web/company/access` | admin: `list` / `grant` / `revoke` |

## 3. Actions, content and data

| Route | Purpose |
|---|---|
| `POST /web/action/load` | load an `ir_act_window` |
| `POST /web/action/load_breadcrumbs` | breadcrumb resolution |
| `POST /web/dataset/fields_get` | field metadata |
| `GET /web/content/{id}` | serve an attachment |
| `POST /web/attachment/upload` | upload one |
| `GET /web/export/{model}` | CSV export |
| `POST /web/import/{model}` | CSV import |
| `GET /web/account/dashboard` | accounting dashboard data |
| `GET /web/account/report` · `/print` | financial statements |
| `GET /web/account/settings` | accounting settings |
| `GET /websocket` | the bus; the session cookie is validated before the connection is accepted |

## 4. Database tools

Admin-gated, and only ever against the caller's **own** tenant.

```
POST /web/dbtool          the Database Tools screen (read-only introspection)
POST /web/db/list
POST /web/db/backup       POST /web/db/restore     POST /web/db/delete
GET  /web/db/download     POST /web/db/upload
POST /web/control/admin   the loopback-only operator console
```

`DbExplorer` runs every query inside a `pqxx::read_transaction`, so a crafted
data-modifying CTE is refused by PostgreSQL itself, not by a string check.
`DbBackup` shells out via **argv arrays**, never a shell string.

## 5. Auth pages

```
GET/POST  /web/signup            account creation (admin-issued in practice)
GET/POST  /web/reset_password    admin-issued password reset
```

## 6. Portal

Cookie `portal_sid`, its own session store, its own per-IP login limiter
(10 attempts / 5 minutes), PBKDF2-SHA512 passwords.

```
GET   /portal                                 the page
POST  /portal/api/login  · /logout · /change-password
GET   /portal/api/home   · /invoices · /orders · /deliveries · /units · /products
GET   /portal/api/invoice/{id}/detail   · /print · /pdf
GET   /portal/api/order/{id}/detail     · /print · /pdf
GET   /portal/api/delivery/{id}/detail  · /print · /pdf
POST  /portal/api/invoice/{id}/proof          upload a payment proof
GET   /portal/api/statement  · /print · /pdf
POST  /portal/api/quote  · /request
GET   /portal/doc/{token}/{id}                token-granted document access
```

Portal routes are registered directly with `drogon::app().registerHandler()`
and so **do not** inherit the `HttpServer` helper behaviour. Each lambda applies
security headers, the `devMode` error gate and the `Secure` cookie flag itself —
see [../development/conventions.md](../development/conventions.md).

## 7. Kiosk, site and health

```
GET   /kiosk                 the punch page
POST  /kiosk/api/punch       check in / check out

GET   /site  · /site/{page} · /site/blog
POST  /site/form/{id}        a website form submission
GET   /site/media/{id}       the media library
GET   /site/api/health
GET   /robots.txt · /sitemap.xml     generated from the published page set

GET   /healthz
GET   /rental/dashboard  ·  /rental/cashflow
GET   /rental/calendar?month=YYYY-MM[&type_id=N]   day-level occupancy
POST  /rental/booking/create                       let a unit for a period
POST  /rental/billing/run?date=YYYY-MM-DD          generate invoices now
GET   /rental/demo/status                     what demo data exists
POST  /rental/demo/seed  ·  /rental/demo/clear
```

The two rental-demo mutations are `POST`, deliberately: a `GET` that changes
data can be triggered by a link, a prefetch or a crawler. All three
authenticate.

Every rental route authenticates — occupancy, tenant names and receivables are
commercially sensitive, and the first cut of these routes had no auth at all.
Parameters travel as query or form values rather than a JSON body, which is the
shape the whole module uses; one idiom is worth more than a tidier payload.

`POST /rental/booking/create` takes `unit_id`, `partner_id`, `date_start` and
optionally `date_end` (empty = open-ended), `unit_price` (default: the unit
type's rate), `contract_id` (default: open a new contract) and `billing_mode`.
It answers **400 with a sentence** when the operator's request cannot stand —
overlapping dates naming the clashing let, a retired unit, no customer — rather
than a masked 500.

## Errors

```jsonc
{ "jsonrpc": "2.0", "id": 1,
  "error": { "code": 100, "message": "Session expired",
             "data": { "message": "Please authenticate first." } } }
```

Exception detail is gated on `dev_mode`. In production the client gets a
generic message and `ex.what()` goes to the log only — `ex.what()` from pqxx
carries full SQL text, table names and schema details. See
[../security/error-handling.md](../security/error-handling.md).
