# IR ID Registry & Implementation Checklist (2026-03-23)

Every module seeds rows into `ir_act_window` and `ir_ui_menu` using **hardcoded integer IDs**.
These IDs must be globally unique across all modules. Conflicts cause silent data corruption
(wrong model is opened, duplicate menus appear, etc.).

---

## Allocated ID Map

### `ir_act_window` (window actions)

| ID | Name | Model | Module |
|----|------|-------|--------|
| 1  | Contacts | res.partner | IrModule |
| 2  | Users | res.users | IrModule |
| 3  | Companies | res.company | IrModule |
| 4  | Chart of Accounts | account.account | AccountModule |
| 5  | Journals | account.journal | AccountModule |
| 6  | Journal Entries | account.move (journal) | AccountModule |
| 7  | Payments | account.payment | AccountModule |
| 8  | Units of Measure | uom.uom | UomModule |
| 9  | Products | product.template | ProductModule |
| 10 | Product Categories | product.category | ProductModule |
| 11 | Sales Orders | sale.order | SaleModule |
| 12 | Purchase Orders | purchase.order | PurchaseModule |
| 13 | Employees | hr.employee | HrModule |
| 14 | Departments | hr.department | HrModule |
| 15 | Job Positions | hr.job | HrModule |
| 16 | Working Schedules | resource.calendar | HrModule |
| 17 | All Transfers | stock.picking | StockModule |
| 18 | Locations | stock.location | StockModule |
| 19 | Operation Types | stock.picking.type | StockModule |
| 20 | Stock Products | product.product | StockModule |
| 21 | Moves History | stock.move | StockModule |
| 22 | Receipts | stock.picking (in) | StockModule |
| 23 | Deliveries | stock.picking (out) | StockModule |
| 24 | Internal Transfers | stock.picking (internal) | StockModule |
| 25 | Warehouses | stock.warehouse | StockModule |
| 30 | Document Templates | ir.report.template | ReportModule |
| 31 | ERP Settings | ir.erp.settings | ReportModule |
| 32 | Customer Invoices | account.move (invoice) | AccountModule |
| 33 | Vendor Bills | account.move (bill) | AccountModule |
| 34 | Bills of Materials | mrp.bom | MrpModule |
| 35 | Portal Users | portal.partner | PortalModule |
| 36 | Groups | res.groups | ReportModule (seed) |

**Next safe action ID: 37**

---

### `ir_ui_menu` (navigation menu items)

| ID | Name | Parent | Module |
|----|------|--------|--------|
| 10 | Accounting (app) | NULL | AccountModule |
| 11 | Journal Entries | 10 | AccountModule |
| 12 | Customers | 10 | AccountModule |
| 13 | Vendors | 10 | AccountModule |
| 14 | Configuration | 10 | AccountModule |
| 15 | Invoices | 12 | AccountModule |
| 16 | Payments | 12 | AccountModule |
| 17 | Bills | 13 | AccountModule |
| 18 | Chart of Accounts | 14 | AccountModule |
| 19 | Journals | 14 | AccountModule |
| 20 | Contacts (app) | NULL | IrModule |
| 21 | Contacts | 20 | IrModule |
| 30 | Settings (app) | NULL | IrModule |
| 31 | Users | 30 | IrModule |
| 32 | Companies | 30 | IrModule |
| 50 | Products (app) | NULL | ProductModule |
| 51 | Products | 50 | ProductModule |
| 52 | Configuration | 50 | UomModule |
| 53 | Units of Measure | 52 | UomModule |
| 54 | Categories | 52 | ProductModule |
| 60 | Sales (app) | NULL | SaleModule |
| 61 | Orders | 60 | SaleModule |
| 62 | Sales Orders | 61 | SaleModule |
| 70 | Purchase (app) | NULL | PurchaseModule |
| 71 | Orders | 70 | PurchaseModule |
| 72 | Purchase Orders | 71 | PurchaseModule |
| 80 | Human Resources (app) | NULL | HrModule |
| 81 | Employees | 80 | HrModule |
| 82 | Departments | 80 | HrModule |
| 83 | Configuration | 80 | HrModule |
| 84 | Job Positions | 83 | HrModule |
| 85 | Working Schedules | 83 | HrModule |
| 90 | Inventory (app) | NULL | StockModule |
| 91 | Operations | 90 | StockModule |
| 92 | Configuration | 90 | StockModule |
| 93 | Locations | 92 | StockModule |
| 94 | Operation Types | 92 | StockModule |
| 95 | All Transfers | 91 | StockModule |
| 96 | Products | 90 | StockModule |
| 97 | Reporting | 90 | StockModule |
| 98 | Products (leaf) | 96 | StockModule |
| 99 | Moves History | 97 | StockModule |
| 101 | Technical | 30 | ReportModule |
| 102 | Document Templates | 101 | ReportModule |
| 103 | ERP Settings | 30 | ReportModule |
| 104 | Bills of Materials | 96 | MrpModule |
| 105 | Groups | 101 | ReportModule |
| 110 | Manufacturing (app) | NULL | MrpModule |
| 111 | Products | 110 | MrpModule |
| 112 | Bills of Materials | 111 | MrpModule |
| 113 | Bills of Materials | 50 | MrpModule |
| 120 | Portal Users | 30 | PortalModule |
| 200 | Receipts | 91 | StockModule |
| 201 | Deliveries | 91 | StockModule |
| 202 | Internal Transfers | 91 | StockModule |
| 203 | Warehouses | 92 | StockModule |

**Next safe menu ID: 121** (skip 114–119 as buffer; next batch: 121+)

---

## Lessons Learned — What Went Wrong

### Bug 1: Reusing an existing action ID
I added `ir_act_window id=4` for `res.groups` but id=4 was already "Chart of Accounts"
(`account.account`) in AccountModule. Result: clicking "Groups" loaded chart of accounts data.

### Bug 2: Duplicate menu section header
I added `ir_ui_menu id=33` "Technical" under Settings, but AccountModule already uses
id=33 for "Vendor Bills". Additionally, ReportModule already seeds id=101 "Technical" as
the proper Settings → Technical section. Result: two "Technical" dropdowns in Settings.

### Bug 3: Menu ID conflict with different module
I chose id=104 for the Groups menu, not knowing MrpModule uses id=104 for
"Bills of Materials" (Inventory → Products). Since both used `ON CONFLICT DO UPDATE`,
the winner was whichever module ran last. The row name stayed "Bills of Materials"
but the action pointed at `res.groups`. Result: "Bill of Materials" link opened Groups.

### Bug 4: Stale DB rows after code fix
Removing a hardcoded seed from code does NOT remove the row from the database —
`ON CONFLICT DO NOTHING` won't clean up rows that already exist. Fix: explicitly
`DELETE FROM ir_ui_menu WHERE id=<stale_id>` in the seed function.

---

## Checklist for Adding a New Module Menu

Before writing any seed code, follow these steps:

- [ ] **1. Consult the ID registry** (this file) — find the next unused `ir_act_window` ID
- [ ] **2. Pick an action ID** — increment "Next safe action ID" at the top of this file
- [ ] **3. Consult the menu registry** — find the next unused `ir_ui_menu` ID
- [ ] **4. Pick a menu ID** — increment "Next safe menu ID" and note your allocation here
- [ ] **5. Update this registry file** with your new allocations before committing
- [ ] **6. Check parent IDs** — verify the parent menu ID exists and belongs to the right app
      - Settings children go under: id=30 (direct) or id=101 (Technical section)
      - Accounting children go under: id=10 (app), id=12 (Customers), id=13 (Vendors), id=14 (Config)
      - Inventory children go under: id=90 (app), id=91 (Operations), id=92 (Config), id=96 (Products)
- [ ] **7. Use `ON CONFLICT (id) DO UPDATE SET name=…, parent_id=…, sequence=…, action_id=…`**
      for menus you own — always update ALL columns, not just `action_id`. A partial update
      will leave a stale `parent_id` in the DB, causing the item to appear under the wrong menu.
- [ ] **8. If removing a menu from code**, add an explicit `DELETE FROM ir_ui_menu WHERE id=<n>`
      to the seed function to remove the stale DB row
- [ ] **9. Build and start the server**, then manually click every affected menu item to
      verify the correct view opens

---

## Module Seed Execution Order

Modules are seeded in registration order. Later modules can overwrite earlier ones if
both use `ON CONFLICT DO UPDATE`. Keep this in mind when the same ID might be touched
by two modules (it shouldn't happen — use the registry to prevent it).

Approximate order: Ir → Auth → Uom → Product → Account → Sale → Purchase → Hr → Stock → Mrp → Report → Portal
