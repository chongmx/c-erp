# Phase 13–14 Progress

## Summary
Phase 13 adds `ir_config_parameter` (system key-value store) to `IrModule`.
Phase 14 adds `AuthSignupModule` — self-service user registration and password reset HTTP endpoints.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## Phase 13 — ir.config.parameter

### Changes to `modules/ir/IrModule.hpp`
- Added `#include "GenericViewModel.hpp"`
- New `IrConfigParameter` model class: `key` (UNIQUE), `value` fields
- `ensureSchema_()` now creates `ir_config_parameter` table:
  ```sql
  CREATE TABLE IF NOT EXISTS ir_config_parameter (
      id         SERIAL  PRIMARY KEY,
      key        VARCHAR NOT NULL UNIQUE,
      value      TEXT    NOT NULL DEFAULT '',
      create_date TIMESTAMP DEFAULT now(),
      write_date  TIMESTAMP DEFAULT now()
  )
  ```
- `initialize()` calls new `seedConfigParams_()` which inserts:
  - `web.base.url` = `http://localhost:8069`
  - `auth_signup.allow` = `True`
  - `auth_signup.reset_pwd` = `True`
  - `database.uuid` = `gen_random_uuid()::text`
  (all `ON CONFLICT (key) DO NOTHING`)
- Model registered in `registerModels()` as `"ir.config.parameter"`
- ViewModel registered in `registerViewModels()` as `GenericViewModel<IrConfigParameter>` (full CRUD: search_read, read, create, write, unlink, fields_get)

---

## Phase 14 — auth_signup

### New file: `modules/auth/AuthSignupModule.hpp`

**Module metadata**
- `moduleName()` = `"auth_signup"`
- `dependencies()` = `{"auth", "ir"}`

**`initialize()`**
```sql
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS signup_token      VARCHAR;
ALTER TABLE res_partner ADD COLUMN IF NOT EXISTS signup_expiration TIMESTAMP;
```

**Routes registered in `registerRoutes()`**

#### `POST /web/signup`
Body: `{ "login": "...", "password": "...", "name": "..." }`
- Checks `ir_config_parameter.auth_signup.allow` — returns 403 if disabled
- Creates `res_partner` + `res_users` rows (company_id=1)
- Hashes password via `AuthService::hashPassword()` (PBKDF2-SHA512)
- Returns `{"result": {"login": "..."}}`
- Returns 400 if login/password missing, 500 on DB error (e.g. duplicate login)

#### `POST /web/reset_password`
Body for token request: `{ "login": "..." }`
Body for completion: `{ "login": "...", "token": "...", "password": "..." }`
- Checks `ir_config_parameter.auth_signup.reset_pwd` — returns 403 if disabled
- **Token request**: generates 48-char hex token (OpenSSL RAND_bytes), stores in `res_partner.signup_token` with 24h expiry, returns `{"result": {"token": "...", "login": "..."}}` (no email — stub for dev)
- **Completion**: verifies token + expiry, updates `res_users.password`, clears token
- OPTIONS preflight registered for both routes (CORS)

### `main.cpp` changes
```cpp
#include "modules/auth/AuthSignupModule.hpp"
g_container->addModule<odoo::modules::auth::AuthSignupModule>();
```

---

## Table Inventory (cumulative after Phase 14)

| Module         | Tables |
|----------------|--------|
| base           | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth           | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir             | `ir_act_window`, `ir_ui_menu`, `ir_config_parameter` |
| account        | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |
| uom            | `uom_uom` |
| product        | `product_category`, `product_product` |
| sale           | `sale_order`, `sale_order_line` |
| purchase       | `purchase_order`, `purchase_order_line` |
| hr             | `resource_calendar`, `hr_department`, `hr_job`, `hr_employee` |
| auth_signup    | *(columns on res_partner: signup_token, signup_expiration)* |

**Total: 27 tables** (+2 sequences)

---

## Notes
- No email infrastructure — password reset tokens are returned in the HTTP response for dev/testing. In production, swap `storeResetToken_` return with an SMTP call.
- `configBool_()` catches all exceptions and defaults to `true` so signup works even if `ir_config_parameter` is not yet seeded (startup race-safe).
