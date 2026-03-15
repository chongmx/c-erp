# Base Module Progress

## Summary
Extended the base module with four new lookup models, seeded reference data, and wired richer session_info. Also hardened the auth module seed to include a default company.

---

## Work Completed

### New Models (all in `modules/base/BaseModule.hpp`)

| Model | C++ Class | Table | Fields |
|-------|-----------|-------|--------|
| `res.lang` | `ResLang` | `res_lang` | name, code, iso_code, url_code, active, direction, date_format, time_format |
| `res.currency` | `ResCurrency` | `res_currency` | name, symbol, position, rounding, decimal_places, active |
| `res.country` | `ResCountry` | `res_country` | name, code, currency_id→res_currency, phone_code |
| `res.country.state` | `ResCountryState` | `res_country_state` | name, code, country_id→res_country |

All four models inherit `BaseModel<T>` with `registerFields`, `serializeFields`, `deserializeFields`, `validate`.

### Extended `res.partner`
Added columns (with `ALTER TABLE … ADD COLUMN IF NOT EXISTS` — safe on existing DBs):
- `street`, `city`, `zip`, `lang`
- `country_id` → `res_country`
- `state_id` → `res_country_state`

### `LookupViewModel<TModel>` template
Reusable read-only viewmodel in `BaseModule.hpp`. Handles `search_read`, `read`, `web_read`, `fields_get`, `search_count` for any `BaseModel<T>`. Registered for all four new models.

### Seed Data
Seeded on first boot (each seed checks row count first — idempotent):

| Table | Seeds |
|-------|-------|
| `res_lang` | `en_US` (id=1) |
| `res_currency` | USD (1), EUR (2), GBP (3), JPY (4), CNY (5) |
| `res_country` | 15 countries, each linked to a currency |

### `modules/auth/ResCompany.hpp`
Added `partnerId` (→ `res_partner`) and `currencyId` (→ `res_currency`) fields with `registerFields`, `serializeFields`, `deserializeFields` entries.

### `modules/auth/AuthModule.hpp` — schema + seed
- `res_company` DDL extended with `ALTER TABLE … ADD COLUMN IF NOT EXISTS partner_id` and `currency_id`
- `seedAdminUser_()` now seeds in correct order:
  1. Company partner ("My Company")
  2. Default company (id=1, currency_id=1 = USD)
  3. Admin partner ("Administrator")
  4. Admin user (company_id=1)

### `core/infrastructure/SessionManager.hpp` — Session enrichment
New fields on `Session` struct:

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | display name from `res_partner.name` |
| `partnerId` | `int` | `res_partner.id` |
| `companyId` | `int` | `res_company.id` |
| `companyName` | `string` | `res_company.name` |
| `isAdmin` | `bool` | member of Administrator group (gid=3) |

`toJson()` now includes all these fields plus `is_admin` and `is_system`.

### `modules/auth/AuthViewModel.hpp` — session enrichment after login
After `AuthService::authenticate()` succeeds, a JOIN query populates the new session fields:
```sql
SELECT u.partner_id, u.company_id,
       COALESCE(p.name, u.login) AS uname,
       COALESCE(c.name, '')      AS cname,
       EXISTS(SELECT 1 FROM res_groups_users_rel r
              WHERE r.uid = u.id AND r.gid = 3) AS is_admin
FROM res_users u
LEFT JOIN res_partner p ON p.id = u.partner_id
LEFT JOIN res_company c ON c.id = u.company_id
WHERE u.id = $1
```
The `authenticate` response now includes `name`, `partner_id`, `company_id`, `is_admin`.

### `core/infrastructure/JsonRpcDispatcher.hpp` — richer session_info
`GET /web/session/get_session_info` now returns the full Odoo-compatible shape:

```json
{
  "uid": 1,
  "login": "admin",
  "name": "Administrator",
  "partner_id": 2,
  "company_id": 1,
  "company_name": "My Company",
  "is_admin": true,
  "is_system": true,
  "is_public": false,
  "is_internal_user": false,
  "username": "admin",
  "db": "odoo",
  "server_version": "19.0+e (odoo-cpp)",
  "session_id": "...",
  "context": { "uid": 1, "lang": "en_US", "tz": "UTC" },
  "user_context": { "uid": 1, "lang": "en_US", "tz": "UTC" },
  "user_companies": {
    "current_company": 1,
    "allowed_companies": {
      "1": { "id": 1, "name": "My Company", "sequence": 1,
             "child_ids": [], "parent_id": false }
    }
  }
}
```

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)
- All changes are additive / idempotent — safe on fresh and existing databases

---

## Current Module & Table Inventory

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |

## Next Steps
Refer to `plan.md` Phase 4 → 5 → 6:
- Phase 4 is now complete (session_info hardened)
- Phase 5: IR stubs (`ir.ui.menu`, `ir.ui.view`, `ir.actions`) for full Odoo webclient compatibility
- Phase 6: Account module (`account.account`, `account.journal`, `account.move`, etc.)
