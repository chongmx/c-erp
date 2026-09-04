# 073 — ERP Admin Console (loopback-only IT operator tool)

**Date:** 2026-08-10
**Status:** ✅ Built + tested (`tools/test_erp_admin.sh`, all green; UI rendered in headless Chrome).
**What:** a **separate, separately-compiled** administrative UI for the IT operator — the
equivalent of the reference ERP's `/web/database/manager` but hardened: SSH/loopback-only, and covering
databases, multi-company setup, nginx and SSL. It is **not** the reference ERP/OWL frontend and is never
served on the public site.

---

## 1. Why a separate tool

the reference ERP ships a database manager on the public webserver, gated only by a master password — a
known attack surface. The requirement here was the opposite: an admin console that the public site
**cannot reach at all**, usable only by a local user who has SSH'd into the box. So it is a
distinct binary (`erp-admin`), built as its own CMake target (`EXCLUDE_FROM_ALL`), bound to
loopback, and reached over an SSH tunnel — never behind nginx.

## 2. Security model

- **Loopback bind, enforced.** Binds `127.0.0.1`; refuses to start on any non-loopback bind. Every
  request is checked for a loopback peer address as well.
- **Per-launch token.** A 160-bit random token is minted at startup and printed in the access URL.
  Only whoever launched the tool over SSH sees it; every `/api/*` call must present it
  (`X-Admin-Token` header, or `?token=` for downloads). The UI scrubs the token from the address
  bar into `sessionStorage`.
- **No shell.** Every system action runs via `fork`+`execvp` with an **argv array** — never a shell
  string — so there is no shell to inject into. Database names, subdomains, and backup filenames
  are **allowlist-validated** (`[A-Za-z0-9_]`; domains add `.`/`-`; filenames reject `/` and `..`)
  before they reach any command.
- **Least privilege by default.** Runs as the local user. Root-only actions (nginx/SSL/systemctl)
  surface the command's stderr in the UI so the operator sees exactly what to grant, rather than
  the tool silently running as root.

## 3. Architecture

- `tools/admin/erp_admin.cpp` — a ~450-line single file: a tiny Drogon server (1 thread, loopback),
  the token gate, a safe `runCmd(argv, env, cwd)`, config parsing (reuses `config/system.cfg`'s
  `db_*`), and the JSON endpoints.
- `tools/admin/web/index.html` — a single-file vanilla-HTML/JS/CSS SPA (dark theme, tabbed). No
  build step, no framework, no external assets.
- Reuses the vendored **Drogon** only; DB work shells out to `psql`/`pg_dump`/`pg_restore` (no
  libpqxx link).

## 4. Capabilities

| Tab | Endpoints | Backing command |
|---|---|---|
| Overview | `/api/overview` | `systemctl is-active`, `psql`, `df`, `uptime` |
| Companies | `/api/tenants[/create|/link|/toggle]` | `tools/provision_tenant.sh`, `tenants.json` edits |
| Databases | `/api/databases`, `/api/backup`, `/api/restore`, `/api/backup/download` | `pg_dump -Fc`, `pg_restore` |
| nginx | `/api/nginx`, `/api/nginx/reload` | `nginx -t`, `systemctl reload nginx` |
| SSL | `/api/ssl`, `/api/ssl/renew` | `certbot certificates`, `certbot renew` |
| Logs | `/api/logs` | `tail`, `journalctl` |
| Service | `/api/service` | `systemctl status/start/stop/restart` |

The **Companies** tab is the bridge to the multi-company work (docs/072): "Create a company" runs
the same `provision_tenant.sh` (createdb → register → provision) the CLI does, and "Link" registers
an existing database as a tenant.

## 5. Testing

- `tools/test_erp_admin.sh` — starts `erp-admin` on a loopback port, captures the token, and
  verifies: **no token → 401**, **wrong token → 401**, real token works; the UI is served;
  overview/databases return data; `pg_dump` backup creates a `.dump` and appears in the list; a
  **malicious db name is rejected** (injection guard); the service-action allowlist holds. All
  green.
- Headless-Chrome render confirms the SPA loads (all 7 tabs, Overview + Companies) with no JS
  errors (bar the browser's own favicon probe, now answered with 204).

Usage, SSH-tunnel instructions, and the privilege matrix are in `tools/admin/README.md`.
