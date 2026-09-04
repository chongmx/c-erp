# Deployment

The runbook lives beside the config it describes:
**[`deploy/README.md`](../../deploy/README.md)** — firewall, nginx, certificates,
the systemd unit, wkhtmltopdf, and the verification steps to run against the
real deployment. This page covers the topology and the cross-build, and points
there for the rest.

---

## Topology

```
internet --TLS--> nginx :443 --plain HTTP--> 127.0.0.1:8069  (c-erp)
                                             127.0.0.1:5432  (PostgreSQL)
```

nginx is the **only** ingress. That is load-bearing, not tidiness.

> Measured: 14 login attempts with rotating forged `X-Real-IP` headers were
> throttled through nginx, and **0 of 14** were throttled when sent straight to
> the app. Direct access lets a client pick its own rate-limit bucket, because
> the loopback peer is trusted and its forwarding header is honoured. If the app
> is reachable without going through nginx, the S-40 rate limiter is bypassable
> by header forgery.

So: bind loopback, and make the network agree.

```bash
ss -tlnp | grep -E '8069|5432'
#   expect 127.0.0.1:8069 and 127.0.0.1:5432 ONLY
#   a 0.0.0.0 here means http_interface is wrong and the app is exposed
curl --connect-timeout 5 http://<external-ip>:8069/healthz   # must fail
```

Open 80 and 443. Never 8069 or 5432.

## Before anything else — rotate the database password

`config/system.cfg` is tracked in git with a placeholder password in plaintext.
The old password is in git history, so rotating is not optional.

```bash
git rm --cached config/system.cfg
printf 'config/system.cfg\n' >> .gitignore
cp config/system.cfg config/system.cfg.example   # placeholders; commit this one
sudo -u postgres psql -c "ALTER USER <dbuser> WITH PASSWORD '<new-strong-password>';"
```

## The config values that matter here

```ini
http_interface      = 127.0.0.1        ; nginx is the only ingress
http_port           = 8069
secure_cookies      = True             ; nginx serves HTTPS
trusted_proxies     = 127.0.0.1,::1    ; whose X-Forwarded-For we believe
session_ttl_minutes = 60
dev_mode                               ; leave unset
```

`trusted_proxies` must list the address nginx connects **from**. Same host →
loopback. If nginx moves to another machine, put its IP here or every client
collapses back into one rate-limit bucket.

Full list: [configuration.md](configuration.md).

## Cross-building

**Do not compile on the deployment host.** Measured on `easylockerspace`
(2 cores, 964 MiB RAM, 6 GiB swap): the largest single translation unit peaks
at **1604 MiB** — more than the host's entire RAM — so it cannot compile that
file at *any* parallelism, only page it through swap. It is not OOM-killed; it
just takes hours. The same build takes 667 s on 8 cores.

`Dockerfile.build` reproduces the host's toolchain exactly so the binary built
here runs there. Every pin in it is a **measurement of the real server**, not a
guess:

```
Debian 13 (trixie), x86-64, kernel 6.12 deb13-cloud, glibc 2.41, gcc 14.2
```

Re-measure before changing it:

```bash
ssh easylockerspace 'ldd --version; gcc --version'
```

Then:

```bash
./scripts/deploy.sh                 # build both in Docker, ship them
./scripts/deploy.sh --server        # c-erp only
./scripts/deploy.sh --admin         # erp-admin only
./scripts/deploy.sh --clean         # wipe ./build first
./scripts/deploy.sh --restart       # sync, then restart via server.sh
./scripts/deploy.sh --status
./scripts/deploy.sh --host HOST     # default: easylockerspace
./scripts/deploy.sh --dry-run       # build and ABI-check, do not ship
./scripts/deploy.sh --rebuild-image
```

`deploy.sh` runs an **ABI check** before shipping — it compares the glibc and
GLIBCXX versions the new binary needs against what the host provides, and
refuses if the host is older. A binary linked against a newer glibc fails at
`exec`, on the host, with no useful message. `--no-abi-check` overrides it;
prefer fixing the image.

The cross-build gets its own directory so it never collides with a local
`./build`.

## Migrations on deploy

```bash
./build/c-erp --provision      # provision + migrate EVERY tenant, then exit
```

This runs the full boot path — `ensureSchema_()` for every module plus every
pending `MigrationRunner` migration — and exits without serving.
`tools/migrate_all_tenants.sh` wraps it.

## wkhtmltopdf — check this early

`scripts/deps/install_wkhtml.sh` handles `jammy` and `bookworm` and **silently
falls back to the jammy package elsewhere**. It can install successfully and
then fail at runtime — and the failure appears only on the first PDF request,
not at startup.

```bash
sudo ./scripts/deps/install_wkhtml.sh
wkhtmltopdf --version     # want: 0.12.6.1 (with patched qt)
```

**"with patched qt" is not optional.** Report footers use `--footer-html`, which
stock builds do not support.

## Verifying a deployment

Run the security suites against the real deployment, **individually**:

```bash
BASE=https://erp.example.com bash tests/security/hardening/security-fixes/test.sh
BASE=https://erp.example.com bash tests/security/auth/session-fixes/test.sh
BASE=https://erp.example.com bash tests/security/auth/unauthenticated/test.sh
BASE=https://erp.example.com bash tests/security/disclosure/error-masking/test.sh
BASE=https://erp.example.com bash tests/security/injection/sql-surfaces/test.sh
```

Each should end `All checks passed.`

**Not via `tests/run.sh`.** The runner snapshots and restores the database,
which is right on a development machine and emphatically wrong in production.
These five only read and probe.

For the rate-limit check, run from two **different external hosts**. From one
host with a spoofed `X-Forwarded-For` you are testing nothing, because nginx
overwrites it.

## nginx

`deploy/nginx/c-erp.conf` is the shipped config. Three placeholders to edit:
`server_name` (twice) and the two `ssl_certificate*` paths.

Check the nginx version first — `http2 on;` is an unknown directive on 1.24 and
nginx will refuse to start. 1.24 keeps `listen 443 ssl http2;`; 1.25.1+ takes
`listen 443 ssl;` plus `http2 on;`.

## Running as a service

```bash
./scripts/server.sh --install --start --enable
./scripts/server.sh --status
./scripts/server.sh --logs -f
```

`WorkingDirectory` matters: the app resolves `config/system.cfg`, `web/static`
and `log/` relative to it. `deploy/README.md` carries a hardened unit template
(`PrivateTmp`, `NoNewPrivileges`, `ProtectSystem=strict`).
