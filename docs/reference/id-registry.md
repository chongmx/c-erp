# ID registry

`ir_ui_menu`, `ir_act_window` and `res_groups` rows are seeded with
**hardcoded ids**. Two modules choosing the same id is silent: whichever seeds
last wins, the loser's menu opens the winner's screen — and seeding on top of an
**app root** deletes a whole app from the home screen. Eight such collisions
shipped before anything checked for them.

## Do not pick an id by reading this page

```bash
bash tests/integration/core/menu-ids/test.sh
```

It reads the source, not the database, so it fires at authoring time whether or
not anyone has run the server. It fails on any id claimed by two modules, fails
if an app root has more than one owner, and **prints the next free id in each
space** — that is the number to use.

At the time of writing: next free `ir_ui_menu` **87**, next free
`ir_act_window` **127**. Run the test rather than trusting those numbers.

The test is also in the default suite, so a collision fails CI.

---

## Reserved app roots (`ir_ui_menu`)

Seeding over one of these removes an app from the home screen. Each may be
seeded by exactly one module.

```
10 20 30 50 60 70 80 90 110 130 300 400      (+ the Settings children 31, 32)
```

## Menu id allocation (`ir_ui_menu`)

| Range | Owner | Area |
|---|---|---|
| 10 | `ir` | **Accounting** app root |
| 11–19, 22–29, 33–49, 59, 63–66 | `account` | the accounting tree |
| 20–21 | `ir` | **Contacts** |
| 30–32 | `ir` | **Settings** and its children |
| 50, 52–53 | `uom` | **Products** root, units of measure |
| 51, 54–57, 75–79, 86 | `product` | products, categories, parts catalogue |
| 60–62 | `sale` | **Sales** |
| 67–68, 80–85 | `hr` | **Employees** |
| 69, 90–99, 200–210 | `stock` | **Inventory** |
| 70–72 | `purchase` | **Purchase** |
| 73 | `portal` | portal users |
| 74, 101–103, 105, 131–132 | `report` | documents and reporting |
| 104, 110–120 | `mrp` | **Manufacturing** |
| 130, 137–142 | `project` | **Project** |
| 150 | `bom` | BOM editor |
| 300, 309–315, 320–322, 330 | `rental` | **Rental** |
| 400–402 | `help` | **Help Centre** |
| 403, 413–415 | `ir` | technical menus, AI settings |
| 404–408 | `hr` | attendance, leave, holidays |
| 409–412 | `website` | the CMS |

## Action id allocation (`ir_act_window`)

| Range | Owner |
|---|---|
| 1–3, 117 | `ir` |
| 4–7, 32–33, 60–63, 73–93 | `account` |
| 8 | `uom` |
| 9–13, 102–107 | `product` |
| 14, 16, 50–51, 97–98, 118–122 | `hr` |
| 17–29, 31, 47, 94, 99 | `stock` |
| 30, 71–72, 95–96, 101 | `report` |
| 34–38 | `mrp` |
| 39–46, 127 | `rental` |
| 48 | `sale` |
| 49 | `purchase` |
| 100 | `portal` |
| 108–113 | `project` |
| 114–115 | `help` |
| 116 | `bom` |
| 123–126 | `website` |

Both spaces have gaps. **Do not fill them by hand** — take the next free id the
test prints. A gap usually means an id was retired, and reusing it resurrects a
stale reference.

## Group ids (`res_groups`)

Declared in `modules/auth/Groups.hpp`, seeded by `AuthModule::seedGroups_()`.
The two must stay in sync.

| id | Constant | What it grants |
|---:|---|---|
| 1 | `BASE_PUBLIC` | portal visitors, `share = TRUE`; no login, public documents only |
| 2 | `BASE_INTERNAL` | standard employee — can log in. **Every non-admin employee needs this** |
| 3 | `BASE_ADMIN` | system administrator; bypasses every access and record-rule check |
| 4 | `SETTINGS_CONFIGURATION` | the Settings app and company/system parameters — *not* full admin |
| 5 | `ACCOUNT_BILLING` | customer invoices and vendor bills |
| 6 | `ACCOUNT_MANAGER` | journal entries, reconciliation, closing, tax and accounting reports |
| 7 | `SALES_USER` | own quotations and sales orders |
| 8 | `SALES_MANAGER` | all sales orders, teams, manual discounts, sales reports |
| 9 | `PURCHASE_USER` | own RFQs and purchase orders |
| 10 | `PURCHASE_MANAGER` | all POs, vendor pricelists, approvals, purchase reports |
| 11 | `INVENTORY_USER` | receipts, deliveries, internal transfers |
| 12 | `INVENTORY_MANAGER` | adjustments, valuation, warehouse config, reordering rules |
| 13 | `MRP_USER` | create and process manufacturing orders |
| 14 | `MRP_MANAGER` | BOMs, work centres, routings, planning, MRP reports |
| 15 | `HR_EMPLOYEE` | own employee profile |
| 16 | `HR_MANAGER` | all employees, contracts, departments, HR reports |

Use the `Groups::` constants in C++, not bare integers. The one deliberate
exception is `JsonRpcDispatcher::checkModelAccess_()`, whose map carries the
numbers with inline comments — see
[http-api.md](http-api.md#model-level-access).

## External ids

For anything that needs a stable name rather than a stable number, use
`ir_model_data` (`core/IrModelData.hpp`): it maps a `module.name` xml_id to a
`(model, res_id)` pair, so a seeded record can be referenced by a name that
never changes even as serial ids do.

All `IrModelData` operations take the **caller's** transaction — an xml_id and
the record it names must be created or destroyed together, or the mapping
dangles.

## Menu coverage

`tests/docs/menu-coverage.md` lists every menu option, nested as it appears in
the interface, with the model behind it and the tests that touch that model.
It is **generated from the database**:

```bash
python3 tests/tools/gen_menu_doc.py
```

Regenerate it after adding a menu or a test.
