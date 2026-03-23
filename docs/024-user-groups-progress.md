# User Groups & Authentication Levels — Progress Log (2026-03-23)

Build clean after all changes.

---

## Overview

The system uses **res_groups** (same as Odoo) to define permission levels. Users are assigned to
one or more groups via the `res_groups_users_rel` junction table. The session carries all group IDs
so any code can check membership without extra DB queries.

---

## Group Hierarchy (16 groups)

| ID | Name | Full Name | share | Access Level |
|----|------|-----------|-------|-------------|
| 1  | Public | Base / Public | TRUE | Anonymous / portal visitors. No login. Read-only public docs. |
| 2  | Internal User | Base / Internal User | FALSE | Every logged-in employee. Required for all internal access. |
| 3  | Administrator | Base / Administrator | FALSE | Full system access. Implied by all below at the broadest level. |
| 4  | Configuration | Settings / Configuration | FALSE | Can access Settings app and change system parameters. |
| 5  | Billing | Accounting / Billing | FALSE | Create/edit/validate invoices and bills. No journal entries. |
| 6  | Accountant | Accounting / Accountant | FALSE | Full accounting: journals, reconciliation, tax, reports. |
| 7  | User | Sales / User | FALSE | Own quotations and sales orders only. |
| 8  | Manager | Sales / Manager | FALSE | All sales orders, teams, discounts, reports. |
| 9  | User | Purchase / User | FALSE | Own RFQs and purchase orders only. |
| 10 | Manager | Purchase / Manager | FALSE | All POs, vendor pricelists, approval, reports. |
| 11 | User | Inventory / User | FALSE | Process receipts, deliveries, transfers. No adjustments. |
| 12 | Manager | Inventory / Manager | FALSE | Adjustments, valuation, warehouse config, reorder rules. |
| 13 | User | Manufacturing / User | FALSE | Create and process manufacturing orders. |
| 14 | Manager | Manufacturing / Manager | FALSE | BoM, work centres, routings, planning, reports. |
| 15 | Employee | Human Resources / Employee | FALSE | Own employee profile only. |
| 16 | Manager | Human Resources / Manager | FALSE | All employees, contracts, departments, HR reports. |

**`share = TRUE`:** Portal/external users — not counted as internal seats.
**`share = FALSE`:** Internal employees — require a named user licence.

---

## Typical User Configuration

| Role | Recommended groups |
|------|--------------------|
| System Administrator | 2 (Internal), 3 (Admin) |
| Accountant (full) | 2, 4 (Settings), 5, 6 |
| Billing Clerk | 2, 5 |
| Sales Executive | 2, 7 |
| Sales Director | 2, 8 |
| Purchasing Officer | 2, 9 |
| Purchasing Manager | 2, 10 |
| Warehouse Staff | 2, 11 |
| Warehouse Manager | 2, 12 |
| Production Worker | 2, 13 |
| Production Manager | 2, 14 |
| HR Staff | 2, 15 |
| HR Manager | 2, 16 |
| Customer (portal) | 1 — handled by PortalSessionManager, not res_users login |

> Every internal user should be in group **2 (Internal User)**. Without it,
> `session_info.is_internal_user` is `false` and the frontend will treat them as public.

---

## Files Changed

### `modules/auth/AuthModule.hpp` — `seedGroups_()`

Changed from an early-return-if-count>0 pattern to fully idempotent individual inserts:

```cpp
txn.exec(R"(
    INSERT INTO res_groups (id, name, full_name, share) VALUES
        (1,  'Public',        'Base / Public',                TRUE),
        (2,  'Internal User', 'Base / Internal User',         FALSE),
        (3,  'Administrator', 'Base / Administrator',         FALSE),
        (4,  'Configuration', 'Settings / Configuration',     FALSE),
        (5,  'Billing',       'Accounting / Billing',         FALSE),
        (6,  'Accountant',    'Accounting / Accountant',      FALSE),
        (7,  'User',          'Sales / User',                 FALSE),
        (8,  'Manager',       'Sales / Manager',              FALSE),
        (9,  'User',          'Purchase / User',              FALSE),
        (10, 'Manager',       'Purchase / Manager',           FALSE),
        (11, 'User',          'Inventory / User',             FALSE),
        (12, 'Manager',       'Inventory / Manager',          FALSE),
        (13, 'User',          'Manufacturing / User',         FALSE),
        (14, 'Manager',       'Manufacturing / Manager',      FALSE),
        (15, 'Employee',      'Human Resources / Employee',   FALSE),
        (16, 'Manager',       'Human Resources / Manager',    FALSE)
    ON CONFLICT (id) DO NOTHING
)");
txn.exec("SELECT setval('res_groups_id_seq', 16, true)");
```

**Why `ON CONFLICT (id) DO NOTHING` (not early return):**
The old approach skipped seeding entirely if any groups existed. This meant upgrading an existing
installation would never add the new groups 4–16. The new approach is safe to run on every boot.

### `core/infrastructure/SessionManager.hpp` — `Session` struct

Added:

```cpp
std::vector<int> groupIds;  // all res_groups IDs this user belongs to

bool hasGroup(int gid) const;
bool hasAnyGroup(std::initializer_list<int> gids) const;
```

`toJson()` now includes `"group_ids": [2, 7, ...]`.

### `modules/auth/AuthViewModel.hpp` — `handleAuthenticate`

After successful login, loads all group IDs with a second query:

```cpp
auto gRows = etxn.exec(
    "SELECT gid FROM res_groups_users_rel WHERE uid = $1 ORDER BY gid",
    pqxx::params{session.uid});
for (const auto& gr : gRows)
    session.groupIds.push_back(gr["gid"].as<int>());
```

`group_ids` is included in the authenticate response JSON and stored in the session.

### `core/infrastructure/JsonRpcDispatcher.hpp`

- `handleAuthenticate` result sync: copies `group_ids` array from result into `session.groupIds`
- `session_info`: `is_internal_user` now checks `session.hasGroup(2)` instead of `!session.isAdmin`

### `modules/auth/Groups.hpp` *(new file)*

Named integer constants so code uses `Groups::SALES_MANAGER` instead of bare `8`:

```cpp
namespace Groups {
    constexpr int BASE_PUBLIC            = 1;
    constexpr int BASE_INTERNAL          = 2;
    constexpr int BASE_ADMIN             = 3;
    constexpr int SETTINGS_CONFIGURATION = 4;
    constexpr int ACCOUNT_BILLING        = 5;
    constexpr int ACCOUNT_MANAGER        = 6;
    constexpr int SALES_USER             = 7;
    constexpr int SALES_MANAGER          = 8;
    constexpr int PURCHASE_USER          = 9;
    constexpr int PURCHASE_MANAGER       = 10;
    constexpr int INVENTORY_USER         = 11;
    constexpr int INVENTORY_MANAGER      = 12;
    constexpr int MRP_USER               = 13;
    constexpr int MRP_MANAGER            = 14;
    constexpr int HR_EMPLOYEE            = 15;
    constexpr int HR_MANAGER             = 16;
}
```

---

## How to Check Groups in ViewModel/Route Code

```cpp
#include "modules/auth/Groups.hpp"

// In any ViewModel method that receives a Session:
if (!session.hasGroup(Groups::ACCOUNT_MANAGER))
    throw std::runtime_error("Access denied: Accounting / Accountant required");

if (!session.hasAnyGroup({Groups::SALES_USER, Groups::SALES_MANAGER}))
    throw std::runtime_error("Access denied: Sales access required");

if (!session.isAdmin)   // shortcut — same as hasGroup(Groups::BASE_ADMIN)
    throw std::runtime_error("Administrators only");
```

---

## How to Assign a User to Groups (SQL)

```sql
-- Assign user id=5 to Internal User + Sales Manager + Accounting Billing
INSERT INTO res_groups_users_rel (gid, uid) VALUES
    (2, 5),   -- Base / Internal User
    (8, 5),   -- Sales / Manager
    (5, 5)    -- Accounting / Billing
ON CONFLICT DO NOTHING;
```

The frontend Settings → Users form will expose group checkboxes in a future UI phase.

---

## What Is NOT Yet Enforced (Future Phase — SEC-04)

Groups are defined and stored in the session, but actual enforcement is manual:
- **No `ir.model.access` table** — ViewModels do not auto-check CRUD permissions per group
- **No `ir.rule` table** — no automatic domain filtering per group
- **No field-level visibility** by group

Until RBAC enforcement is implemented, group membership is available for code to check via
`session.hasGroup()` but is not automatically enforced on every RPC call.
