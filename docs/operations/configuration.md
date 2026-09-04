# Configuration

One INI file, `config/system.cfg`, read by `AppConfig::fromFileOrEnv()`
(`core/Container.hpp`). Pass a different path with `--config`. If the file does
not exist, configuration falls back to environment variables.

```bash
./build/c-erp --config /etc/c-erp/system.cfg
```

Everything lives under a single `[options]` section.

---

## Database

| Key | Default | |
|---|---|---|
| `db_host` | `localhost` | |
| `db_port` | `5432` | |
| `db_name` | | the database |
| `db_user` | | |
| `db_password` | | |
| `db_maxconn` | `10` | connection pool size |

> The shipped `db_name` and `db_user` are both the literal string `odoo` — a
> historical identifier, not a product reference. It appears in
> `deploy/README.md`, `tools/admin/README.md`, `tests/docs/` and
> `backups/odoo/` for the same reason: those are real database, role and path
> names, so they are quoted verbatim.
> Renaming the database is a migration, not a documentation change; if you do
> it, those commands need updating with it.

## HTTP

| Key | Default | |
|---|---|---|
| `http_interface` | `127.0.0.1` | **Bind loopback in production** — nginx is the sole ingress |
| `http_port` | `8069` | |
| `workers` | `4` | |
| `http_doc_root` | `web/static` | |
| `http_index` | `index.html` | |
| `cors_origin` | *(empty)* | no `Access-Control-Allow-Origin` is sent. Set it **only** for a separate dev-server origin, exactly (`http://localhost:3000`). Never `*` in production |

## Security

| Key | Default | |
|---|---|---|
| `secure_cookies` | | `True` sets `Secure` on the session cookie. **Required** whenever nginx serves HTTPS — i.e. always, in production |
| `trusted_proxies` | `127.0.0.1,::1` | proxies whose `X-Forwarded-For` / `X-Real-IP` are believed for per-client rate limiting (S-40). Only the immediate peer is checked. `False` disables header trust entirely, for direct-exposure setups |
| `session_ttl_minutes` | `60` | idle session lifetime. Lower = a smaller window for a stolen session id; higher = fewer surprise logouts |
| `dev_mode` | *unset (false)* | exposes `ex.what()` in HTTP responses. **Local developer machines only** — it is deliberately absent from the production template. See [../security/error-handling.md](../security/error-handling.md) |

`trusted_proxies` must list the address nginx connects **from**. Same host →
loopback.

## Logging

| Key | Default | |
|---|---|---|
| `log_level` | `warn` | the template ships `info` |
| `logfile` | `log/system.log` | |
| `log_size_limit_mb` | `20` | the logger rolls by size |
| `log_max_files` | `30` | retention; `0` = unlimited |

Both retention keys matter. Without them the logger keeps every roll forever —
1,807 files had accumulated before they were set.

## Mail

| Key | Default |
|---|---|
| `smtp_server` | `localhost` |
| `smtp_port` | `25` |
| `smtp_ssl` | `False` |
| `smtp_user` | `False` |
| `smtp_password` | `False` |
| `email_from` | `False` |

## Multi-company

| Key | |
|---|---|
| `control_db` | the control-plane identity database. Empty = the cross-tenant company switcher is off |

Tenant databases are configured separately, in `config/tenants.json` — copy
`config/tenants.json.example`. See
[../architecture/multi-company.md](../architecture/multi-company.md).

## Environment variables

Used only when the config file is absent:

```
DB_HOST  DB_PORT  DB_NAME  DB_USER  DB_PASSWORD  DB_POOL_SIZE
HTTP_HOST  HTTP_PORT  HTTP_THREADS
```

## Config is read once, at the top

`ServiceFactory` is the configuration bus. Modules read `db()`, `devMode()`,
`secureCookies()` and `sessions()` from it. **Never read an environment
variable or a file inside a module constructor** — add the value to
`ServiceFactory` instead.

## A note on secrets

`config/system.cfg` ships with placeholder credentials. Before a real
deployment, untrack it and rotate the password — the shipped one is in git
history:

```bash
git rm --cached config/system.cfg
printf 'config/system.cfg\n' >> .gitignore
cp config/system.cfg config/system.cfg.example      # placeholders; commit this one
sudo -u postgres psql -c "ALTER USER <dbuser> WITH PASSWORD '<new-strong-password>';"
```

See [deployment.md](deployment.md).
