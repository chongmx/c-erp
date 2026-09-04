# 044 — Production Assessment & Feature Roadmap

**Date:** 2026-08-02
**Live:** https://www.easylockerspace.com (Cloudflare → nginx → c-erp)
**Origin:** 35.208.143.63 · Ubuntu 24.04 · 964 MB RAM + 6 GB swap · 20 GB disk (63% used)
**Deployed tree:** `/home/chongmx/code/c-erp` @ `7a38d6f`, running as `chongmx` under systemd
**Method:** read-only SSH inspection; no changes made

---

## 1. The finding that matters most

**The running binary is stale. None of the security work is actually live.**

```
binary  built : 2026-08-02 02:14:25 UTC
last commit   : 2026-08-02 13:06:07 UTC   (~11 h newer)
```

The source tree has every fix. The process serving traffic does not. Proof from the live log:

```
[auth] session sync for 3354302b... uid=1 updated=1   ← JsonRpcDispatcher.hpp:342
```

Two tells: there is **no `[auth] session rotated`** line (S-42 rotation absent), and the line
numbers are pre-fix (`:342` / `:375`, versus `:360+` / `:424` in the fixed build).

So production is currently running code without S-39 (RCE), S-40/S-49 (rate limiting),
S-42/S-43/S-48 (sessions) or S-47 (audit). `audit_log` has 0 rows, consistent with this.

**Fix: rebuild and restart. Five minutes.** Everything below is secondary to this.

```bash
ssh easylockerspace
cd ~/code/c-erp && cmake --build ./build -j2 && sudo systemctl restart c-erp
# confirm:
grep 'session rotated' log/system.log      # should appear after your next login
```

> Build with `-j2`, not `-j4` — 964 MB RAM will thrash or OOM otherwise. Swap will absorb it
> but the build takes far longer.

---

## 2. What is correctly set up

Credit where due — the deployment is well built:

| Item | State |
|---|---|
| App bound to `127.0.0.1:8069` only | ✅ — the load-bearing control for S-40 |
| PostgreSQL bound to `127.0.0.1:5432` | ✅ |
| `secure_cookies = True` | ✅ |
| `trusted_proxies = 127.0.0.1,::1` | ✅ correct for the nginx→app hop |
| `dev_mode` unset (defaults false) | ✅ |
| `session_ttl_minutes = 60` | ✅ |
| systemd unit, `Restart=on-failure` | ✅ |
| HSTS via Cloudflare | ✅ `max-age=31536000` |
| nginx `http2 on;` | ✅ correct for this nginx build |
| WebSocket block repeats all proxy headers | ✅ |
| `limit_req` zones on both login endpoints | ✅ |

**Cloudflare real-IP handling is correct**, which is the part I was most concerned about:

```nginx
# /etc/nginx/conf.d/cloudflare-realip.conf   (20 CF ranges, auto-refresh script)
real_ip_header    CF-Connecting-IP;
real_ip_recursive on;
```

With `real_ip` active, `$remote_addr` is already the true client, so
`proxy_set_header X-Real-IP $remote_addr` hands the app the real address and S-40 works as
designed once the fixed binary is running. Nothing in the app needs changing for Cloudflare.

---

## 3. Production issues

### P1 — Stale binary (§1). Rebuild and restart.

### P2 — `db_password` is still the default `odoo`, and `config/system.cfg` is still tracked in git

Both confirmed on the live host. The database password is the string `odoo`, publicly known,
and committed to the repository. PostgreSQL is loopback-only, so this is not immediately
remotely exploitable — but it is one local foothold or one repo leak from full data access.

```bash
sudo -u postgres psql -c "ALTER USER odoo WITH PASSWORD '<strong>';"
# update db_password in config/system.cfg, then:
git rm --cached config/system.cfg
echo 'config/system.cfg' >> .gitignore
sudo systemctl restart c-erp
```

### P3 — The origin is reachable directly, bypassing Cloudflare

Verified from outside:

```
origin :443 with spoofed Host -> 200
origin :80                    -> 301
```

Anyone who knows `35.208.143.63` skips Cloudflare's WAF, DDoS absorption and bot rules
entirely. Rate limiting still works (a direct client's IP is not in the CF ranges, so
`real_ip` correctly leaves `$remote_addr` alone, and a forged `CF-Connecting-IP` is ignored) —
but every other Cloudflare protection is opt-out.

Fix — restrict origin ingress to Cloudflare:

```bash
gcloud compute firewall-rules create allow-cloudflare-https \
  --allow tcp:80,tcp:443 \
  --source-ranges="$(curl -s https://www.cloudflare.com/ips-v4 | paste -sd,)" \
  --target-tags=<vm-tag>
gcloud compute firewall-rules delete <the-current-0.0.0.0/0-web-rule>
```

Keep SSH on a separate rule. Cloudflare's ranges change — reuse the existing
`update-cloudflare-ips.sh` cron to refresh the firewall too.

### P4 — `wkhtmltopdf` is not installed → every PDF route is dead

```
wkhtmltopdf: command not found
```

All invoice/order/delivery PDF downloads return 503, in both the backoffice and the portal.
This is S-46 materialising exactly as predicted. `scripts/install_wkhtml.sh` has no `noble`
branch and silently targets the jammy build.

Verify the **patched-Qt** build specifically — the report footers depend on `--footer-html`,
which stock builds do not support:

```bash
sudo ./scripts/install_wkhtml.sh
wkhtmltopdf --version     # must say: 0.12.6.1 (with patched qt)
```

If the jammy package won't resolve on noble, the options are a jammy container for the
renderer or a different engine (and the footer work in `docs/030` would need redoing).

### P5 — No backups

Nothing found. Before real customer data exists:

```bash
# /etc/cron.daily/c-erp-backup
pg_dump -U odoo odoo | gzip > /var/backups/c-erp-$(date +%F).sql.gz
find /var/backups -name 'c-erp-*.sql.gz' -mtime +30 -delete
```

Restore one into a scratch database before relying on it.

### P6 — 964 MB RAM

Fine for one instance with 6 GB swap, but it constrains two things: build with `-j2`, and the
process-per-tenant multi-company design in `040` §2 needs re-costing before committing —
measure one instance's RSS first.

---

## 4. Yet-to-be-completed features

The domain name says what this system is for: **locker rental**. That reframes the backlog —
the ERP is scaffolding, the rental module is the product.

### 4.1 Blocking a real rental business

| # | Feature | Why it blocks | Effort |
|---|---|---|---|
| F1 | **Rental module** | The actual product. No units, contracts, recurring invoicing, dashboard or event log exist | ~7 w |
| F2 | **`ir.cron`** | No scheduler at all → no recurring invoicing, no recurring expenses | 3–4 d |
| F3 | **`ir.sequence`** | Numbering is hardcoded PG sequences. Gapless, per-company invoice numbering is a legal requirement in most jurisdictions | 3–4 d |
| F4 | **Tax computation** | `account.tax` exists as a model; nothing computes tax lines. **Every invoice total is currently untaxed** | 2 w |
| F5 | **Payment allocation** | "Pays in advance / pays late" *is* allocation. Needs `account.partial.reconcile` + residuals | 1 w |
| F6 | **`ir.mail_server`** | No SMTP → no invoice delivery, no overdue reminders, no password reset (S-31) | 3–5 d |
| F7 | **`ir.attachment`** | No file storage → no signed contracts, no expense receipts | 3–4 d |

**F2–F5 are hard prerequisites for F1.** A rental module built first would ship its own
scheduler, its own numbering and its own tax arithmetic — three pieces of core infrastructure
buried in a vertical module, to be torn out later.

### 4.2 Needed soon after launch

| Feature | Note |
|---|---|
| **Test suite** | Last open P0. S-49 (rate limiter counting nothing) is exactly the bug class that survives review and dies to a runtime test |
| `stock.quant` / `qty_available` | No on-hand quantity exists anywhere. Not needed for lockers; blocking if you ever sell goods |
| Password policy + 2FA (`auth_totp`) | No length or complexity rules today |
| Persistent sessions (PERF-B) | Every restart logs everyone out; blocks running >1 instance |
| `product.supplierinfo` | Phase A3b — **allowlisted in the dispatcher but not implemented** |
| `mrp.production` | Same: allowlisted, not implemented |

### 4.3 Deferred — not needed for lockers

`product.pricelist`, `product.template`/variants, `account.fiscal.position`,
`res.currency.rate`/FX, bank reconciliation, `account.analytic.account`,
`stock.production.lot`, reordering rules, PartKeepr PK2–PK7, multi-company.

Multi-company (`040` §2) is deferred deliberately: with 964 MB RAM the process-per-tenant
design needs re-costing, and one company is enough to launch.

### 4.4 Small Stage 1 leftovers

`S-41` (domain field allowlist), `C-1` (recursive `product_count`), `S-45` (ORDER BY rebuild),
dead ACL entries. ~10 h total. Do them opportunistically, not as a blocking phase.

---

## 5. Plan

### Now — today, ~2 hours

| # | Task | Time |
|---|---|---|
| 1 | **Rebuild + restart** — makes all existing security work live | 15 m |
| 2 | Rotate `db_password`, untrack `config/system.cfg` | 20 m |
| 3 | Install wkhtmltopdf, verify patched-Qt, test a PDF end to end | 30 m |
| 4 | Lock the GCP firewall to Cloudflare ranges | 20 m |
| 5 | Daily `pg_dump` cron + one restore drill | 30 m |

After this the deployment is sound and every fix already written is actually running.

### Phase 1 — rental foundations (~4 weeks)

Build in dependency order. Each is genuinely needed by the rental module, so none is
speculative:

```
F2 ir.cron          3–4 d   ── scheduler (reuse the S-43 eviction timer pattern)
F3 ir.sequence      3–4 d   ── contract + invoice numbering
F5 payment alloc    1 w     ── advance/late payments, residuals
F4 tax engine       2 w     ── correct invoice totals   ← highest-risk item, prototype first
```

Ship the test-suite harness alongside these (`040` Stage 3) rather than as a separate phase —
`scripts/test_sessionmanager.cpp` is already close to production shape and ports with a
`main()` swap.

### Phase 2 — rental module (~7 weeks)

Per `040` §3, which has the full schema and billing design:

```
models + schema + migrations        1.5 w
contract lifecycle, unit states     1 w
billing engine + idempotency        1.5 w   ← the UNIQUE(contract_line_id, period_start)
payments, deposits, overdue         1 w        constraint is the critical piece
expenses (one-off + recurring)      1 w
dashboard                           1 w
event log + portal + record rules   1.5 w
```

### Phase 3 — after first customers

Test-suite depth, password policy + 2FA, `ir.attachment` (F7), `ir.mail_server` (F6) if not
already pulled forward for invoice delivery, persistent sessions.

> F6/F7 move into Phase 1 the moment you need to **email** an invoice or **attach** a signed
> contract — both are likely as soon as you have a real tenant. They are cheap (3–5 d each);
> the sequencing above assumes manual PDF handover at first.

### Critical path

**~11 weeks to a working locker-rental business**, assuming one developer and no scope growth.
The tax engine (F4) is the item most likely to overrun and the one I would prototype first —
if it slips, everything after it slips.

---

## 6. What I did not change

This was a read-only investigation. Nothing on the server was modified — no rebuild, no config
edit, no service restart. Every command in §3 and §5 is for you to run or to approve.

The single highest-value action is §5 item 1: the security work is written, reviewed, tested
and committed, and none of it is running.
