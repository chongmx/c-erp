# ERP Admin Console (`erp-admin`)

A **separate, loopback-only** administrative console for the IT operator — *not* the
Odoo/OWL frontend, and *never* exposed to the public site. It manages the c-erp service,
PostgreSQL databases (backup/restore), multi-company tenants, nginx and SSL, by shelling
out to the system tools after allowlist-validating every identifier.

## Security model

- **Loopback only.** It binds `127.0.0.1` and refuses any other bind. It is never put behind
  nginx. Reach it over an **SSH tunnel**:

  ```bash
  ssh -L 8072:localhost:8072 <user>@<server>
  ```

- **Per-launch token.** On start it prints a one-time URL containing a random token; only
  whoever launched it over SSH sees it. Every `/api/*` call requires that token **and** a
  loopback peer address. Close the tool when you're done — it's meant to be run on demand,
  not left running.

- **No shell injection.** Every action runs via `execvp` with an argv array (never a shell
  string), and database names / domains / backup filenames are allowlist-validated
  (`[A-Za-z0-9_]`, no `/` or `..`) before use.

## Build & run

```bash
cmake --build ./build --target erp-admin      # separate target, not built by default

# RECOMMENDED — run elevated so nginx / SSL / systemctl actions work:
sudo ./build/erp-admin

# or as your user (databases/backups/tenants work; privileged actions fall back
# to `sudo -n` and, if that needs a password, tell you exactly what to grant):
./build/erp-admin

# options:
./build/erp-admin --port 8072 --config config/system.cfg \
                  --service c-erp --nginx-conf /etc/nginx/sites-available/c-erp.conf
```

It **auto-resolves its working directory** to the project root (the folder above
`build/`), so `config/`, `tools/` and the web UI are found no matter where you launch it
from. The startup banner prints the resolved app root, the DB it will use, and — if you're
not root — a reminder to use `sudo`.

Then, from your laptop with the tunnel open, browse to the printed
`http://127.0.0.1:8072/?token=…` URL.

## Privileges

The console runs as **your local user**. Actions that need more:

| Action | Needs |
|---|---|
| Create company / restore into a new DB | a PostgreSQL role with **CREATEDB** (`ALTER ROLE odoo CREATEDB;`) |
| nginx test / reload | write access to nginx + `systemctl reload nginx` (root / sudoers) |
| SSL (certbot) | root / sudoers |
| Service start/stop/restart, journalctl | root / sudoers, and a systemd unit (see below) |

Backup, restore-into-existing, DB listing and tenant registration work with just the
configured DB user. Anything requiring root shows the command's error output in the UI, so
you can see exactly what to grant. To run the whole console elevated: `sudo ./build/erp-admin`.

## What it does

- **Overview** — c-erp service state, DB reachability, disk, uptime.
- **Companies** — list tenants (`config/tenants.json`); **create** a company (calls
  `tools/provision_tenant.sh`: createdb → register → provision); **link** an existing DB;
  enable/disable.
- **Databases** — list DBs + sizes; **backup** (`pg_dump -Fc`); **restore** (`pg_restore`);
  download a dump.
- **nginx** — view the config, `nginx -t`, test-then-reload.
- **SSL** — `certbot certificates`, renew (dry-run by default).
- **Logs** — c-erp log / `journalctl` / nginx error log.
- **Service** — start / stop / restart / status of the c-erp systemd unit.

## Suggested systemd unit (so Service/logs work on the live server)

```ini
# /etc/systemd/system/c-erp.service
[Unit]
Description=c-erp
After=network.target postgresql.service
[Service]
WorkingDirectory=/opt/c-erp
ExecStart=/opt/c-erp/build/c-erp
Restart=on-failure
[Install]
WantedBy=multi-user.target
```

## Test

```bash
tools/test_erp_admin.sh     # starts it, checks the auth gate + DB/backup/tenant endpoints
```
