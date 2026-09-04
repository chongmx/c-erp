# Multi-company

There are two independent mechanisms, and they answer different questions.
Both can be on at once.

| | Several companies **in one database** | Several companies **in separate databases** |
|---|---|---|
| Isolation | `company_id` + `ir.rule` record rules | separate PostgreSQL databases |
| Config | none — it is always on | `config/tenants.json` |
| Switching | `/web/session/set_active_company` | `/web/session/switch_company` |
| Users | one `res_users` row, several companies | one row per tenant, linked by the control plane |
| Use when | one operator, several legal entities | separate customers on one deployment |

---

## One database, several companies

Every company-scoped table carries `company_id`. Isolation is enforced by
**record rules**, not by the query author remembering to filter.

`res_company_users_rel` lists which companies a user may reach.
`res_users.company_id` is the currently active one.

```
POST /web/session/my_companies          which companies can I see?
POST /web/session/set_active_company    switch (company_id must be allowed)
POST /web/company/access                admin: list / grant / revoke
```

`/web/session/consolidated` reads across the allowed set instead of the active
one, for group-level reporting.

### Record rules

`core/RuleEngine` loads `ir_rule` once at startup (`IrModule::initialize()`)
and is called by `BaseModel` before **every** CRUD operation.

Semantics follow the reference ERP:

```
global rules (global = true)   subtractive — ALL must match
group rules  (global = false)  additive    — the user needs ≥1 group match

final filter = AND(global rules) AND OR(matching group rules)
```

- Admin users (`isAdmin`) bypass all rules.
- A model with no active rule is unrestricted.
- Variables substituted into `domain_force`: `user.id`, `user.company_id`,
  `user.partner_id`.

**Custom SQL does not get this for free.** A ViewModel that hand-writes a
`search_read` never passes through `BaseModel`, so record rules would be
silently unenforced. `modules/base/RecordRuleSql.hpp` closes that: call it once
per custom read and it appends

```sql
AND <idExpr> IN (SELECT id FROM <table> WHERE <ir.rule domain>)
```

as an **id-membership subquery** rather than splicing leaves into the outer
`WHERE` — deliberately, because the outer query joins other tables that also
have a `company_id`, and a bare column there would be ambiguous.

It is a no-op when the rule engine is not ready, the user is an admin, or the
model has no rule for that operation. Safe to call unconditionally.

## Several databases (tenants)

Copy `config/tenants.json.example` to `config/tenants.json`:

```json
{
  "tenants": [
    { "name": "acme",   "subdomain": "acme",   "email_domains": ["acme.com"],   "active": true },
    { "name": "globex", "subdomain": "globex", "email_domains": ["globex.com"], "active": true }
  ]
}
```

`name` is the database and the tenant key. Connection details inherit from the
primary `db_*` values in `config/system.cfg` unless a tenant overrides `host`,
`port`, `user`, `password` or `pool_size`.

### How a request finds its tenant

In order:

1. an explicit `db` argument (only `authenticate` has one);
2. the `Host` subdomain — `acme.example.com` → tenant `acme`;
3. the login's email domain — `someone@acme.com` → tenant `acme`;
4. the default database.

### Provisioning and migrating

```bash
tools/provision_tenant.sh <name>      # create + provision one tenant
tools/migrate_all_tenants.sh          # migrate every tenant
./build/c-erp --provision             # boot, provision + migrate all, exit
```

`--provision` (alias `--migrate`) is the one used by deployment: it runs the
full boot path, so every tenant gets `ensureSchema_()` and every pending
`MigrationRunner` migration, then exits without serving.

## The control plane

Optional, and only needed for the cross-tenant company switcher.

Set `control_db = <database>` in `config/system.cfg` and provision that
database. It is **not** an ERP tenant and never gets the ERP schema — it holds
two tables:

| Table | What it holds |
|---|---|
| `mc_membership` | identity → (tenant database, local login in that tenant) |
| `mc_shared_product` | a shared product catalogue tenants may opt into |

Once someone has proved their identity in one company, the control plane
vouches for them in the others: the login page can offer a company chooser, and
the top bar can offer a switcher.

```
POST /web/session/lookup_companies       pre-login: which companies is this identity in?
POST /web/session/companies              post-login: the same, authenticated
POST /web/session/switch_company         issue a new session in another tenant
POST /web/session/import_shared_products pull the shared catalogue into this tenant
```

`switch_company` mints a **new session id** in the target tenant. It does not
reuse the old one.

## Company identity

`core/CompanyIdentity` is the single answer to "what is this company called,
where is it, what is its bank account". Before it existed, the invoice PDF, the
template preview and the portal each answered differently — the preview showed
different details from the document it was previewing. Read company details
from here, never from `res_company` or `ir_config_parameter` directly.
