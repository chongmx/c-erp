# 070 — Fresh-database provisioning fix (the v809 boot crash)

**Date:** 2026-08-09
**Status:** ✅ Fixed and verified on a genuinely empty database.
**Trigger:** deploying the current code to a clean server (`instance-easylockerspace-24-04`)
died on boot:

```
[c-erp] Fatal: [migrations] v809 'rental_ir_sequence' FAILED:
           ERROR:  relation "ir_sequence" does not exist
```

---

## 1. Root cause — migrations ran *before* the schema they depend on

Boot had two schema mechanisms in the wrong order (`core/Container.hpp::boot()`):

- **Stage 2b — `runMigrations_()`** — the numbered `registerMigration({…})` list, applied in
  ascending version order.
- **Stage 2c — `initializeModules_()`** — each module's `ensureSchema_()` (DDL) + seeds.

Migrations ran **first**. But the migrations are written as **ALTERs on / reads of the tables
that `ensureSchema_()` creates**:

| Migration | Needs (created in `ensureSchema_`, stage 2c) |
|---|---|
| 200 `add_sale_note` | `sale_order` |
| 809 `rental_ir_sequence` | `ir_sequence` |
| 810 `rental_ir_cron` | `ir_cron` |
| 901 `money_reference_data` | `res_currency`, `res_company`, `account_account` |
| 940 `money_stock_move` | `stock_move` |
| 980 `create_ir_sequence` seed | `sale_order_seq`, `purchase_order_seq`, `stock_*_seq` |

On the long-lived dev database none of this mattered — every table already existed and every
migration was already recorded in `schema_migrations`, so stage 2b was a no-op. **The ordering
had simply never been exercised against an empty database.** The first genuinely fresh boot hit
migration 809 trying to `INSERT INTO ir_sequence` — a table not created until migration 980, and
in practice by `ensureSchema_` which had not run yet — and aborted.

## 2. The fix — schema first, migrations second

**`core/Container.hpp`** — swap the two stages:

```
initializeModules_();   // 2b now: ensureSchema_() (DDL) + seeds
runMigrations_();       // 2c now: ALTER / type-convert / seed on top
```

This is the correct dependency direction: the migrations are *upgrades* to a base schema, so the
base schema must exist first. On an already-migrated database it changes nothing (every migration
is skipped via `schema_migrations`; every `CREATE … IF NOT EXISTS` / `ON CONFLICT` seed is a
no-op).

**`modules/auth/AuthModule.cpp::ensureSchema_()`** — create `ir_sequence` and `ir_cron` here,
right after `res_company` (which `ir_sequence.company_id` FKs). Auth is module #2, so these exist
before any later module seeds into them (mrp `mrp.production`, account `INV`) **and** before the
rental migrations 809/810 populate them. Migrations 980/990 keep the identical
`CREATE … IF NOT EXISTS` as their canonical definition; on a fresh DB `ensureSchema_` wins and
those migrations become no-ops. The two definitions must stay in sync (noted in both files).

## 3. Secondary bug this exposed — base currency was USD, not MYR

With schema/seed now running before migrations, a latent defect surfaced: `AuthModule` seeded the
default company with `currency_id = 1` (USD), and migration 901 only assigns MYR
`WHERE currency_id IS NULL`. A fresh install therefore came up with **USD** as the base currency.

**Fix:** seed the company with `currency_id = NULL` and let migration 901 (the canonical base-
currency step) set it to MYR. Verified no module seed reads company currency during init, so the
brief NULL window (until 901 runs, same boot) is safe. Fresh boot now yields base currency **MYR**.

## 4. Preflight tool — `tools/db_preflight.sh`

A **read-only** pre-start check the operator runs first (it never writes to the DB). It parses the
same `config/system.cfg` (env overrides `DB_HOST/PORT/NAME/USER/PASSWORD`, as `setup_db.sh`) and:

1. tests connectivity, distinguishing *DB missing* / *auth failed* / *unreachable*;
2. reports empty vs provisioned;
3. checks the critical infrastructure tables (`ir_sequence`, `ir_cron`, `res_company`, …);
4. cross-checks **migrations declared in code vs applied in the DB**, listing any pending ones.

Exit codes: `0` ready (provisioned & up to date, or empty and ready to provision), `1` connection/
config problem, `2` pending migrations. Usage: `./tools/db_preflight.sh`.

## 5. No demo data on the live server

Confirmed: `RentalDemo::seed` is reachable **only** through the opt-in `/rental/demo/seed` HTTP
route — never from any module `initialize()`. A fresh boot seeds only reference data (menus,
groups, chart of accounts, currencies, countries, sequences/crons). The fresh provision below has
`rental_contract = 0`, `rental_unit = 0`, `product_product = 0`.

## 6. Verification — from a genuinely empty `public` schema

```
DROP SCHEMA public CASCADE; CREATE SCHEMA public;   → 0 tables
./build/c-erp                                        → "Listening on :8069", no Fatal
```

- **81 tables** created; **37 migrations** applied, gap-free v1 → v1040 (incl. the former crashers
  809, 810, 980, 990).
- `ir_sequence`: `rental.contract` (the v809 insert), `sale.order`, `purchase.order`,
  `stock.picking.{in,out,int}`, `mrp.production`, `account.move.INV`.
- `ir_cron`: `session.gc`, `rental.billing`, `rental.expenses`, `stock.reorder`.
- Base currency **MYR**; **no demo data**.
- `tools/db_preflight.sh` → "provisioned and up to date" (exit 0).

## 7. Test suite — 38/44, and what the 6 non-passing are

Running the full suite against the freshly-provisioned DB (the currency fix cleared
`verify_currency_rate` and `verify_ir_cron`): **38 passed, 6 failed.** None of the 6 is caused by
this change — they are **pre-existing non-hermetic tests** that the empty DB exposed:

| Failing script | Why (not a regression) |
|---|---|
| `verify_money_display` / `verify_money_recompute` / `verify_money_roundtrip` | Hardcode `product_product WHERE id=3` / `account_move WHERE id=4` — records they never create. The suite leaves `product_product = 0` after a run, so id=3 only ever existed as **hand-seeded baseline** on the long-lived dev DB. |
| `verify_tax_engine` | Same: builds a sale line from a baseline product that isn't there → the request body interpolates empty → server "Invalid JSON". Its invoice-tax checks (no product needed) pass. |
| `verify_no_double_audit` | Its audit checks all pass; it fails a **meta-check** ("probed 3 models, need ≥4") that depends on DB state. |
| `verify_security_fixes` | Audit checks pass; fails **S-40 X-Forwarded-For bucketing** (HTTP proxy-IP parsing) — unrelated to schema/provisioning. |

**Takeaway for reliability:** the integration suite assumes a database carried forward across runs
(baseline products id=1–3, specific `account_move` ids). A from-scratch provision — now possible
for the first time — does not reproduce that. Making these four scripts create their own products
(hermetic) is the clean follow-up; it is a test-quality change, separate from this deployment fix.

## 8. Files touched

- `core/Container.hpp` — swap stages 2b/2c (+ comments).
- `modules/auth/AuthModule.cpp` — `ir_sequence` + `ir_cron` in `ensureSchema_`; company
  `currency_id = NULL` (base currency fix).
- `tools/db_preflight.sh` — new read-only preflight check.
