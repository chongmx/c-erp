# HR Module Progress

## Summary
Implemented Phase 12 — Human Resources module. 4 models covering working schedules, departments, job positions, and employees. Independent of sale/purchase — only depends on base and auth. Uses `GenericViewModel<T>` for all CRUD operations (no custom actions needed). Schema handles the circular dependency between `hr_department.manager_id` and `hr_employee` via deferred `ALTER TABLE`.

---

## Build Status
- Build: ✅ clean (`[100%] Built target c-erp`)

---

## New Files

| File | Purpose |
|------|---------|
| `modules/hr/HrModule.hpp` | ResourceCalendar + HrDepartment + HrJob + HrEmployee models, views, module class |

---

## Models

### `resource.calendar` → table `resource_calendar`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | Working schedule name, required |
| hours_per_day | Float | DEFAULT 8.0 |
| company_id | Many2one | → res.company |
| active | Boolean | |

**Seed:** id=1 — "Standard 40 hours/week", 8h/day

### `hr.department` → table `hr_department`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| parent_id | Many2one | → hr.department (hierarchical) |
| manager_id | Many2one | → hr.employee (added via ALTER TABLE after hr_employee created) |
| company_id | Many2one | → res.company |
| active | Boolean | |

### `hr.job` → table `hr_job`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | Job position name, required |
| description | Text | |
| department_id | Many2one | → hr.department |
| company_id | Many2one | → res.company |
| active | Boolean | |

### `hr.employee` → table `hr_employee`

| Field | Type | Notes |
|-------|------|-------|
| name | Char | required |
| job_id | Many2one | → hr.job |
| department_id | Many2one | → hr.department |
| parent_id | Many2one | → hr.employee (manager) |
| coach_id | Many2one | → hr.employee (coach) |
| work_email | Char | |
| work_phone | Char | |
| mobile_phone | Char | |
| resource_calendar_id | Many2one | → resource.calendar |
| company_id | Many2one | → res.company |
| user_id | Many2one | → res.users |
| address_id | Many2one | → res.partner (work address) |
| gender | Selection | male / female / other |
| marital | Selection | single / married / other |
| birthday | Date | |
| identification_id | Char | National ID |
| private_email | Char | |
| active | Boolean | |

---

## Circular FK Resolution

`hr_department.manager_id → hr_employee` and `hr_employee.department_id → hr_department` would be a circular dependency at table creation time. Solved with:
1. Create `hr_department` without `manager_id`
2. Create `hr_employee` (which references `hr_department`)
3. `ALTER TABLE hr_department ADD COLUMN IF NOT EXISTS manager_id INTEGER REFERENCES hr_employee(id)`

This is idempotent on every boot via `ADD COLUMN IF NOT EXISTS`.

---

## Views

- `resource.calendar.list` / `.form`
- `hr.department.list` / `.form`
- `hr.job.list` / `.form`
- `hr.employee.list` — name, job, department, work_email, work_phone
- `hr.employee.form` — all 18 fields

---

## IR

| Type | id | name | target |
|------|----|------|--------|
| ir_act_window | 13 | Employees | hr.employee |
| ir_act_window | 14 | Departments | hr.department |
| ir_act_window | 15 | Job Positions | hr.job |
| ir_act_window | 16 | Working Schedules | resource.calendar |
| ir_ui_menu | 80 | Employees (app tile) | parent=NULL |
| ir_ui_menu | 81 | Employees (leaf) | parent=80, action_id=13 |
| ir_ui_menu | 82 | Departments (leaf) | parent=80, action_id=14 |
| ir_ui_menu | 83 | Configuration (section) | parent=80 |
| ir_ui_menu | 84 | Job Positions (leaf) | parent=83, action_id=15 |
| ir_ui_menu | 85 | Working Schedules (leaf) | parent=83, action_id=16 |

---

## CMakeLists.txt / main.cpp Changes
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/modules/hr
```
```cpp
#include "modules/hr/HrModule.hpp"
g_container->addModule<odoo::modules::hr::HrModule>();
```

---

## Module & Table Inventory (cumulative after Phase 12)

| Module | Tables |
|--------|--------|
| base | `res_lang`, `res_currency`, `res_country`, `res_country_state`, `res_partner` |
| auth | `res_company`, `res_groups`, `res_users`, `res_groups_users_rel` |
| ir | `ir_act_window`, `ir_ui_menu` |
| account | `account_account`, `account_journal`, `account_tax`, `account_move`, `account_move_line`, `account_payment`, `account_payment_term` |
| uom | `uom_uom` |
| product | `product_category`, `product_product` |
| sale | `sale_order`, `sale_order_line` |
| purchase | `purchase_order`, `purchase_order_line` |
| hr | `resource_calendar`, `hr_department`, `hr_job`, `hr_employee` |

**Total: 26 tables** (plus `sale_order_seq`, `purchase_order_seq` sequences)

---

## Next Steps
- Phase 11: Stock module — deferred (high complexity, 26 models in Odoo)
- auth_signup — deferred (requires email infrastructure)
- All core ERP modules now complete for MVP
