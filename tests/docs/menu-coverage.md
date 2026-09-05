# Menu coverage — every page in the ERP

Every menu option, nested exactly as it appears in the interface, with the
model it opens and the tests that touch that model.

**Generated from the database, not written by hand.** Regenerate after adding
a menu or a test:

```bash
python3 tests/tools/gen_menu_doc.py
```

| | |
|---|---|
| Menu options that open a page | **115** |
| Covered by at least one test | **107** (93%) |
| No test at all | **8** |

Legend: ✅ two or more tests · 🟡 exactly one (thin — often only a
"the form opens" smoke check) · ❌ none.

> **The 93% is generous and is not page coverage.** A page counts as covered
> when ANY test mentions the model behind it. Many pages share one model —
> every entry under Accounting → Journals, Customers and Vendors is
> `account.move` — so one well-tested model marks a dozen distinct pages green.
> Credit Notes and Refunds are separate screens with separate behaviour; the
> tests that make them ✅ were written for invoices.
>
> The two audit lists at the bottom are the real backlog. Treat 🟡 as
> "probably untested" until you open the named test and check.

> **Coverage here means "a test mentions this model".** It does not mean the
> page is exercised, and it never means the page was rendered. Read
> [test-plan.md](test-plan.md) §3 for what a real test of a page has to do,
> and [browser-render-checks.md](browser-render-checks.md) for why a green
> API test can sit in front of a blank screen.

---

## The menu tree

- **Accounting**
  - **Dashboard** · `account.dashboard` — 🟡 `integration/account/bank-dashboard`
  - **Journal Entries** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
  - **Journals**
    - **Sales** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
    - **Purchases** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
    - **Bank and Cash** · `account.bank.statement` — ✅ `functional/account/money-in-and-out`, `integration/account/bank-recon`, `security/access/multicompany-hardening`
    - **Miscellaneous** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
  - **Customers**
    - **Invoices** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
    - **Credit Notes** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
    - **Payments** · `account.payment` — 🟡 `functional/account/money-in-and-out`
    - **Products** · `product.product` — ✅ `functional/base/customer-company`, `integration/core/ir-primitives`, `integration/core/precision` +9
    - **Customers** · `res.partner` — ✅ `functional/account/accounting`, `functional/base/contact-company-display`, `functional/base/customer-company` +24
  - **Vendors**
    - **Bills** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
    - **Refunds** · `account.move` — ✅ `functional/account/money-in-and-out`, `functional/account/period-close`, `functional/portal/customer-portal` +19
    - **Payments** · `account.payment` — 🟡 `functional/account/money-in-and-out`
    - **Products** · `product.product` — ✅ `functional/base/customer-company`, `integration/core/ir-primitives`, `integration/core/precision` +9
    - **Vendors** · `res.partner` — ✅ `functional/account/accounting`, `functional/base/contact-company-display`, `functional/base/customer-company` +24
  - **Expense Reports** · `hr.expense.sheet` — ✅ `integration/account/expenses`, `integration/core/attachments`, `integration/core/new-forms` +1
  - **Bank Reconciliation** · `bank.reconcile` — ❌ **no test**
  - **Assets** · `account.asset` — 🟡 `integration/account/assets`
  - **Budgets** · `account.budget` — 🟡 `integration/account/budgets`
  - **Reporting**
    - **Financial Reports** · `account.report` — ❌ **no test**
  - **Configuration**
    - **Settings** · `account.settings` — 🟡 `integration/account/lockdates-settings`
    - **Chart of Accounts** · `account.account` — ✅ `integration/account/account-config`, `integration/core/new-forms`
    - **Journals** · `account.journal` — ✅ `integration/account/account-config`, `integration/core/new-forms`, `integration/core/new-views-smoke`
    - **Analytic Accounts** · `account.analytic.account` — ✅ `functional/account/money-in-and-out`, `integration/account/analytic`, `integration/core/new-forms`
    - **Analytic Items** · `account.analytic.line` — 🟡 `functional/account/money-in-and-out`
    - **Bank Statements** · `account.bank.statement` — ✅ `functional/account/money-in-and-out`, `integration/account/bank-recon`, `security/access/multicompany-hardening`
    - **Bank Accounts** · `account.bank.account` — 🟡 `integration/account/bank-dashboard`
    - **Asset Types** · `account.asset.type` — 🟡 `integration/account/assets`
    - **Budgetary Positions** · `account.budget.post` — 🟡 `integration/account/budgets`
    - **Currencies** · `res.currency` — ✅ `integration/account/account-config`, `integration/account/currency-rate`
    - **Account Types** · `account.account.type` — 🟡 `integration/account/account-config`
    - **Fiscal Positions** · `account.fiscal.position` — 🟡 `integration/account/account-config`
    - **Incoterms** · `account.incoterms` — 🟡 `integration/account/account-config`
    - **Journal Groups** · `account.journal.group` — 🟡 `integration/account/account-config`
- **Contacts**
  - **Contacts** · `res.partner` — ✅ `functional/account/accounting`, `functional/base/contact-company-display`, `functional/base/customer-company` +24
- **Settings**
  - **ERP Settings** · `ir.erp.settings` — 🟡 `integration/core/new-forms`
  - **Users & Access**
    - **Users** · `res.users` — ✅ `functional/account/accounting`, `integration/account/no-double-audit`, `integration/portal/web-features` +11
    - **Groups** · `res.groups` — 🟡 `integration/core/new-forms`
    - **Portal Users** · `portal.partner` — ✅ `functional/portal/customer-portal`, `integration/core/new-forms`, `integration/portal/web-features` +2
    - **Companies** · `res.company` — ✅ `integration/core/contact-company-link`, `security/access/partner-tenant-isolation`
    - **Companies & Access** · `company.admin` — ❌ **no test**
  - **Website**
    - **Website Pages** · `website.page` — ✅ `integration/website/blocks`, `integration/website/cms`, `integration/website/editor` +3
    - **Website Menu** · `website.menu` — 🟡 `integration/website/cms`
    - **Website Forms** · `website.form` — ✅ `integration/website/cms`, `integration/website/forms`, `security/website/site-hardening`
    - **Form Submissions** · `website.form.submission` — 🟡 `integration/website/forms`
  - **Database**
    - **Database & Backups** · `db.backups` — ❌ **no test**
    - **Database Tools** · `db.studio` — 🟡 `integration/core/db-tools`
    - **Demo Data** · `rental.demo.data` — 🟡 `integration/rental/rental-demo`
  - **Technical**
    - **Document Templates** · `ir.report.template` — ✅ `integration/core/new-forms`, `integration/core/new-views-smoke`, `integration/core/template-reset`
    - **AI Agent** · `ir.ai.settings` — ✅ `integration/core/ai-settings`, `integration/mrp/bom-formats`, `integration/product/part-lookup-review`
- **Sales**
  - **Orders**
    - **Sales Orders** · `sale.order` — ✅ `functional/base/customer-company`, `functional/portal/customer-portal`, `functional/sale/order-to-invoice` +13
- **Rental**
  - **Operations**
    - **Dashboard** · `rental.dashboard` — 🟡 `integration/rental/rental-dashboard`
    - **Units** · `rental.unit` — ✅ `functional/base/customer-company`, `functional/rental/unit-picker`, `integration/account/no-double-audit` +5
    - **Contracts** · `rental.contract` — ✅ `functional/base/customer-company`, `functional/core/form-pickers`, `functional/rental/lifecycle` +5
    - **Expenses** · `rental.expense` — ✅ `integration/account/no-double-audit`, `integration/core/new-forms`, `integration/rental/rental-schema`
    - **Events** · `rental.event` — 🟡 `integration/rental/rental-schema`
  - **Configuration**
    - **Unit Types** · `rental.unit.type` — ✅ `integration/core/new-forms`, `integration/money/money-string-write`, `integration/rental/rental-schema`
    - **Expense Categories** · `rental.expense.category` — ✅ `integration/account/no-double-audit`, `integration/core/new-forms`, `integration/rental/rental-schema`
- **Products**
  - **Products** · `product.product` — ✅ `functional/base/customer-company`, `integration/core/ir-primitives`, `integration/core/precision` +9
  - **Product Templates** · `product.template` — ✅ `integration/product/product-variants`, `integration/sale/pricelists`
  - **Parts Catalogue** · `part.catalog` — ✅ `functional/product/parts-catalogue`, `integration/product/part-catalog`, `security/injection/sql-surfaces`
  - **Part Lookup** · `part.lookup` — ✅ `integration/core/ai-settings`, `integration/mrp/bom-import`, `integration/product/part-lookup` +2
  - **Configuration**
    - **Units of Measure** · `uom.uom` — 🟡 `integration/core/new-forms`
    - **Categories** · `product.category` — ✅ `integration/core/new-forms`, `integration/product/category-tree`, `integration/sale/pricelists` +2
    - **Attributes** · `product.attribute` — 🟡 `integration/product/product-variants`
    - **Pricelists** · `product.pricelist` — 🟡 `integration/sale/pricelists`
    - **Price Rules** · `product.pricelist.item` — 🟡 `integration/sale/pricelists`
    - **Vendor Pricelists** · `product.supplierinfo` — ✅ `integration/core/new-forms`, `integration/purchase/supplierinfo`
    - **Footprints** · `part.footprint` — ✅ `integration/core/new-forms`, `integration/product/partkeepr`
    - **Part Units** · `part.unit` — 🟡 `integration/core/new-forms`
  - **Bills of Materials** · `mrp.bom` — 🟡 `integration/account/no-double-audit`
- **Purchase**
  - **Orders**
    - **Purchase Orders** · `purchase.order` — ✅ `functional/purchase/order-to-bill`, `integration/core/new-forms`, `integration/core/read-group`
- **Inventory**
  - **Operations**
    - **Receipts** · `stock.picking` — ✅ `functional/portal/customer-portal`, `functional/purchase/order-to-bill`, `functional/sale/order-to-invoice` +13
    - **Deliveries** · `stock.picking` — ✅ `functional/portal/customer-portal`, `functional/purchase/order-to-bill`, `functional/sale/order-to-invoice` +13
    - **Internal Transfers** · `stock.picking` — ✅ `functional/portal/customer-portal`, `functional/purchase/order-to-bill`, `functional/sale/order-to-invoice` +13
    - **All Transfers** · `stock.picking` — ✅ `functional/portal/customer-portal`, `functional/purchase/order-to-bill`, `functional/sale/order-to-invoice` +13
    - **Landed Costs** · `stock.landed.cost` — ✅ `integration/core/new-forms`, `integration/stock/landed-cost`
    - **Barcode** · `barcode.scan` — ❌ **no test**
  - **Products**
    - **Products** · `product.product` — ✅ `functional/base/customer-company`, `integration/core/ir-primitives`, `integration/core/precision` +9
    - **Bills of Materials** · `mrp.bom` — 🟡 `integration/account/no-double-audit`
    - **Lots/Serial Numbers** · `stock.production.lot` — ✅ `functional/stock/warehouse`, `integration/core/new-forms`, `integration/stock/lot-serial` +1
    - **Packages** · `stock.quant.package` — ✅ `functional/stock/warehouse`, `integration/core/new-forms`, `integration/core/new-views-smoke` +1
  - **Reporting**
    - **On Hand** · `stock.quant` — ✅ `functional/mrp/manufacture`, `functional/portal/customer-portal`, `functional/sale/order-to-invoice` +13
    - **Inventory Valuation** · `stock.valuation.layer` — 🟡 `integration/stock/stock-valuation`
    - **Moves History** · `stock.move` — ✅ `functional/stock/warehouse`, `integration/core/new-views-smoke`, `integration/stock/lots-packages`
  - **Configuration**
    - **Warehouses** · `stock.warehouse` — ✅ `integration/core/new-forms`, `integration/core/new-views-smoke`, `integration/purchase/supplierinfo` +2
    - **Locations** · `stock.location` — ✅ `functional/stock/warehouse`, `integration/core/new-forms`
    - **Operation Types** · `stock.picking.type` — 🟡 `integration/core/new-forms`
    - **Reordering Rules** · `stock.warehouse.orderpoint` — ✅ `integration/core/new-forms`, `integration/core/new-views-smoke`, `integration/purchase/supplierinfo` +2
    - **Putaway Rules** · `stock.putaway.rule` — ✅ `integration/core/new-forms`, `integration/core/new-views-smoke`, `integration/stock/product-inventory` +1
- **Project**
  - **Task Board** · `project.board` — 🟡 `integration/project/project`
  - **Projects** · `project.project` — ✅ `functional/project/project`, `integration/project/project`, `integration/website/forms`
  - **Tasks** · `project.task` — ✅ `functional/project/project`, `integration/project/project`, `integration/website/forms`
  - **Timesheets** · `project.timegrid` — 🟡 `integration/project/project`
  - **Timesheet Entries** · `project.timesheet` — ✅ `functional/project/project`, `integration/project/project`
  - **Task Stages** · `project.task.type` — ❌ **no test**
- **Employees**
  - **Employees** · `hr.employee` — ✅ `integration/account/expenses`, `integration/core/new-forms`, `integration/core/new-views-smoke` +3
  - **Attendance** · `hr.attendance` — 🟡 `integration/hr/attendance`
  - **Time Off** · `hr.leave` — 🟡 `integration/hr/holidays`
  - **Allocations** · `hr.leave.allocation` — 🟡 `integration/hr/holidays`
  - **Departments** · `hr.department` — 🟡 `integration/core/new-forms`
  - **Employee Expenses** · `hr.expense` — ✅ `integration/account/expenses`, `integration/core/attachments`, `integration/core/new-forms` +1
  - **Configuration**
    - **Job Positions** · `hr.job` — 🟡 `integration/core/new-forms`
    - **Working Schedules** · `resource.calendar` — 🟡 `integration/core/new-forms`
    - **Leave Types** · `hr.leave.type` — ❌ **no test**
    - **Public Holidays** · `hr.public.holiday` — ❌ **no test**
- **Manufacturing**
  - **Operations**
    - **Manufacturing Orders** · `mrp.production` — ✅ `functional/mrp/manufacture`, `integration/core/new-forms`, `integration/mrp/mrp-mps` +2
    - **Work Orders** · `mrp.workorder` — 🟡 `integration/mrp/mrp-workorder`
  - **BOM Editor** · `bom.editor` — ✅ `functional/product/parts-catalogue`, `integration/mrp/bom-formats`, `integration/mrp/bom-import`
  - **Planning**
    - **Master Production Schedule** · `mrp.production.schedule` — ✅ `integration/core/new-forms`, `integration/mrp/mrp-mps`
  - **Products**
    - **Bills of Materials** · `mrp.bom` — 🟡 `integration/account/no-double-audit`
  - **Configuration**
    - **Work Centers** · `mrp.workcenter` — ✅ `integration/core/new-forms`, `integration/mrp/mrp-production`, `integration/mrp/mrp-workorder`
- **Help**
  - **Help Centre** · `help.center` — 🟡 `integration/core/help`
  - **Help Articles** · `help.article` — 🟡 `integration/core/help`

---

## Audit list — no test at all (8)

| Model | Page |
|---|---|
| `account.report` | Financial Reports |
| `bank.reconcile` | Bank Reconciliation |
| `barcode.scan` | Barcode |
| `company.admin` | Companies & Access |
| `db.backups` | Database & Backups |
| `hr.leave.type` | Leave Types |
| `hr.public.holiday` | Public Holidays |
| `project.task.type` | Task Stages |

## Audit list — thin, one test only (40)

Each of these is touched by a single test, and several only incidentally.
Check whether that test actually exercises the page or merely opens its form.

| Model | Page |
|---|---|
| `account.account.type` | Account Types |
| `account.analytic.line` | Analytic Items |
| `account.asset` | Assets |
| `account.asset.type` | Asset Types |
| `account.bank.account` | Bank Accounts |
| `account.budget` | Budgets |
| `account.budget.post` | Budgetary Positions |
| `account.dashboard` | Dashboard |
| `account.fiscal.position` | Fiscal Positions |
| `account.incoterms` | Incoterms |
| `account.journal.group` | Journal Groups |
| `account.payment` | Payments |
| `account.settings` | Settings |
| `db.studio` | Database Tools |
| `help.article` | Help Articles |
| `help.center` | Help Centre |
| `hr.attendance` | Attendance |
| `hr.department` | Departments |
| `hr.job` | Job Positions |
| `hr.leave` | Time Off |
| `hr.leave.allocation` | Allocations |
| `ir.erp.settings` | ERP Settings |
| `mrp.bom` | Bills of Materials |
| `mrp.workorder` | Work Orders |
| `part.unit` | Part Units |
| `product.attribute` | Attributes |
| `product.pricelist` | Pricelists |
| `product.pricelist.item` | Price Rules |
| `project.board` | Task Board |
| `project.timegrid` | Timesheets |
| `rental.dashboard` | Dashboard |
| `rental.demo.data` | Demo Data |
| `rental.event` | Events |
| `res.groups` | Groups |
| `resource.calendar` | Working Schedules |
| `stock.picking.type` | Operation Types |
| `stock.valuation.layer` | Inventory Valuation |
| `uom.uom` | Units of Measure |
| `website.form.submission` | Form Submissions |
| `website.menu` | Website Menu |

## Pages with a browser render check

Only these have been proven to draw: `integration/product/category-tree`.

Every other page in this document is unverified visually, including every one
marked ✅.

---

_Generated 2026-09-04 from `ir_ui_menu` × `ir_act_window`._
