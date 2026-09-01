# Deployment — GCP VM, Ubuntu 24.04, nginx

Runbook for the topology in `docs/040`:

```
internet --TLS--> nginx :443 --plain HTTP--> 127.0.0.1:8069 (c-erp)
                                             127.0.0.1:5432 (PostgreSQL)
```

---

## 1. Before anything else

`config/system.cfg` is **tracked in git with `db_password = odoo` in plaintext**. Fix this
first — it is Stage 1 item 4 and nothing below makes it safe:

```bash
git rm --cached config/system.cfg
printf 'config/system.cfg\n' >> .gitignore
cp config/system.cfg config/system.cfg.example   # placeholders, commit this one
sudo -u postgres psql -c "ALTER USER odoo WITH PASSWORD '<new-strong-password>';"
# then update db_password in the real, untracked config/system.cfg
```

The old password is in git history, so rotating is not optional.

---

## 2. Firewall

The app binds loopback, so nginx is the only ingress — but make the network agree:

```bash
# GCP firewall: allow 80/443 only. Never open 8069 or 5432.
gcloud compute firewall-rules create c-erp-web \
    --allow tcp:80,tcp:443 --target-tags=c-erp --direction=INGRESS
```

Verify after the app starts — this is the check that matters:

```bash
ss -tlnp | grep -E '8069|5432'
#   expect 127.0.0.1:8069 and 127.0.0.1:5432 ONLY
#   a 0.0.0.0 here means http_interface is wrong and the app is exposed
curl --connect-timeout 5 http://<external-ip>:8069/healthz   # must fail
```

> **This is load-bearing, not tidiness.** Measured (docs/042 §6a): 14 login attempts with
> rotating forged `X-Real-IP` headers were throttled through nginx, and **0 of 14** were
> throttled when sent straight to the app. Direct access lets a client pick its own
> rate-limit bucket, because the loopback peer is trusted and its forwarding header is
> honoured. If the app is reachable without going through nginx, S-40 is bypassable by
> header forgery.

---

## 3. Application config

`config/system.cfg` — the values that matter for this topology:

```ini
http_interface      = 127.0.0.1        ; nginx is the only ingress
http_port           = 8069
secure_cookies      = True             ; nginx serves HTTPS
trusted_proxies     = 127.0.0.1,::1    ; S-40 — whose XFF we believe
session_ttl_minutes = 60
dev_mode                               ; leave unset (defaults false)
```

`trusted_proxies` must list the address nginx connects *from*. Same host → loopback. If nginx
ever moves to another machine, put its IP here or every client collapses into one rate-limit
bucket again.

---

## 4. nginx

```bash
sudo apt install -y nginx
sudo cp deploy/nginx/c-erp.conf /etc/nginx/sites-available/c-erp
sudo ln -sf /etc/nginx/sites-available/c-erp /etc/nginx/sites-enabled/c-erp
sudo rm -f /etc/nginx/sites-enabled/default      # else it wins on port 80
```

Edit the three placeholders: `server_name` (×2) and the two `ssl_certificate*` paths.

**Check your nginx version first** — this bites on Ubuntu 24.04:

```bash
nginx -v
```

- **1.24** (Ubuntu 24.04 default) → keep `listen 443 ssl http2;` as shipped.
- **1.25.1+** → switch to the commented alternative (`listen 443 ssl;` + `http2 on;`).

`http2 on;` on 1.24 is an unknown directive and nginx will refuse to start.

Certificates:

```bash
sudo apt install -y certbot python3-certbot-nginx
sudo mkdir -p /var/www/certbot
sudo certbot --nginx -d erp.example.com
```

Then:

```bash
sudo nginx -t && sudo systemctl reload nginx
```

---

## 5. Running the app

No systemd unit is committed yet (Stage 4 adds a template for multi-tenant). Minimal single
-tenant unit:

```ini
# /etc/systemd/system/c-erp.service
[Unit]
Description=c-erp
After=network.target postgresql.service

[Service]
Type=simple
User=cerp
WorkingDirectory=/opt/c-erp
ExecStart=/opt/c-erp/build/c-erp
Restart=on-failure
RestartSec=5

# PDF rendering writes to a temp dir; PrivateTmp keeps it off the shared /tmp.
PrivateTmp=true
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/c-erp/log /opt/c-erp/uploads

[Install]
WantedBy=multi-user.target
```

`WorkingDirectory` matters — the app resolves `config/system.cfg`, `web/static` and `log/`
relative to it.

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now c-erp
```

---

## 6. wkhtmltopdf — check this early (S-46)

`scripts/deps/install_wkhtml.sh` handles only `jammy` and `bookworm` and **silently falls back to
the jammy package on noble**. It may install and then fail at runtime, and the failure only
appears on the first PDF request — not at startup.

```bash
sudo ./scripts/deps/install_wkhtml.sh
wkhtmltopdf --version     # want: 0.12.6.1 (with patched qt)
```

The **patched qt** part is not optional — the report footers use `--footer-html`, which stock
builds do not support. If this fails, the fallbacks are a jammy container for the renderer or a
different engine, and the footer layout work would need redoing.

---

## 7. Verify the deployment

```bash
# S-40: per-client rate limiting through the proxy.
# Run from two DIFFERENT external hosts — from one host with a spoofed
# X-Forwarded-For you are testing nothing, because nginx overwrites it.
for i in $(seq 1 12); do
  curl -s -o /dev/null -X POST https://erp.example.com/web/session/authenticate \
    -H 'Content-Type: application/json' \
    --data '{"jsonrpc":"2.0","params":{"db":"odoo","login":"x","password":"y"}}'
done
# host A must be blocked by ~attempt 11; host B must still be served.
```

Then the committed suites, against the real deployment:

```bash
BASE=https://erp.example.com bash tests/security/hardening/security-fixes/test.sh
BASE=https://erp.example.com bash tests/security/auth/session-fixes/test.sh
BASE=https://erp.example.com bash tests/security/auth/unauthenticated/test.sh
BASE=https://erp.example.com bash tests/security/disclosure/error-masking/test.sh
BASE=https://erp.example.com bash tests/security/injection/sql-surfaces/test.sh
```

Each should end `All checks passed.`

Run them individually like this, **not** via `tests/run.sh` — the runner
snapshots and restores the database, which is right for a development machine
and emphatically wrong for production. These five only read and probe.

Two of them make assertions that a real deployment should tighten beyond what
a local run can prove:

- `unauthenticated` reports `Secure` on the session cookie as a NOTE over
  plain HTTP. Against `https://` it must be present — if it is not, fix
  `secureCookies` in the config before going further.
- `error-masking` only means anything with `dev_mode=false`. Confirm that
  first; with devMode on, the server returns `ex.what()` by design and the
  test is measuring nothing.

---

## 8. Backups — before real data exists

```bash
# /etc/cron.daily/c-erp-backup
pg_dump -U odoo odoo | gzip > /var/backups/c-erp-$(date +%F).sql.gz
find /var/backups -name 'c-erp-*.sql.gz' -mtime +30 -delete
```

**Restore it once, into a scratch database, before you rely on it.** An untested backup is not
a backup.

---

## 9. Multi-tenant

See `docs/040` §2. One process per company, each with its own config, port and database, one
nginx `server` block per subdomain. The template is at the bottom of
`deploy/nginx/c-erp.conf`.

Cookies scope per subdomain, so tenant sessions isolate for free. **Do not** widen the session
cookie to the parent domain to smooth admin switching — that recouples the tenants.
