# Database schema

**128 tables, 1,466 columns** in a tenant database, plus two in the control
plane (see the last section). Every one is created and evolved from C++ — there
is no `.sql` schema file to read and no external migration directory.

This page is the *catalogue*: which tables exist, which module owns each one,
and what columns it carries. For column **types**, defaults and constraints,
read the `CREATE TABLE` in the source file named at the head of each section —
that is the only authoritative copy, and duplicating it here would only rot.

```bash
# what the running database actually has
./scripts/db_snapshot.sh restore db/snapshots/baseline.dump
psql -d "$DB" -c '\d account_move'
```

The in-app **Database Tools** screen (Settings → Technical → Database Tools)
browses the live schema, and is the fastest way to answer a question about one
table.

### Column order here is not the database's column order

Each row lists columns in the order the **source declares** them — the
`CREATE TABLE` first, then each `ALTER TABLE … ADD COLUMN` where it reads. The
database orders them by `ordinal_position`, which is the order they were
physically *added*, and the two diverge wherever a migration appended a column
that appears earlier in the source.

`res_partner` is the clearest case: the page keeps the portal and signup
columns next to the other identity columns, while the database has them after
`company_name`, because they arrived in a later `ALTER`. Five tables differ
this way — `res_partner`, `account_move_line`, `sale_order_line`, `stock_move`
and `mrp_bom` — and in every one the *set* of columns is identical.

This is deliberate: grouped-by-purpose reads better than
ordered-by-migration-accident, and nothing depends on the position. So
`./tests/tools/audit_schema_doc.sh` compares the **set** of columns and the
count, and ignores the sequence. If you ever need true physical order, ask the
database: `psql -d odoo -c '\d res_partner'`.

---

## How the schema is created

Two mechanisms, both idempotent, both run at boot:

1. **`ensureSchema_()`** in each module's `initialize()` —
   `CREATE TABLE IF NOT EXISTS` plus `ALTER TABLE … ADD COLUMN IF NOT EXISTS`.
   This is the normal path. Booting against an existing database is a no-op;
   booting against an empty one provisions it in full.
2. **`MigrationRunner`** (`core/infrastructure/MigrationRunner.cpp`) — numbered
   migrations recorded in `schema_migrations`, each applied once in its own
   transaction. Use this for data transformations rather than shape changes;
   the Money scale-6 conversion (`core/MoneyMigrations.cpp`) is the worked
   example.

Reference data is seeded with `ON CONFLICT (id) DO NOTHING` — never with a
`COUNT(*) > 0` guard, which would block later seeds from being added.

## Conventions that hold everywhere

| | |
|---|---|
| Primary key | `id SERIAL PRIMARY KEY` |
| Timestamps | `create_date`, `write_date`, both `DEFAULT now()` |
| Soft delete | `active BOOLEAN DEFAULT TRUE` where a record is archivable |
| Company scope | `company_id` — isolation is enforced by `ir.rule`, not by callers |
| Money & quantity | `BIGINT` micro-units, **scale 6** (`core/Money.hpp`). Never `double`, never `NUMERIC` for amounts |
| Model ↔ table | `stock.picking` ↔ `stock_picking` |

**Scale 6 is the single most important convention here.** An amount column is
an int64 holding millionths. `1.50` is stored as `1500000`. Display precision is
a separate, configurable concern (`core/DecimalPrecision.hpp`) and never changes
what is stored.

---

## The tables

Columns below are those declared in `CREATE TABLE` plus every
`ALTER TABLE … ADD COLUMN` found in the same sources, so a column added long
after the table appears here too.

### base — partners and world data

`modules/base/BaseModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `res_country` | 8 | `id`, `name`, `code`, `currency_id`, `phone_code`, `active`, `create_date`, `write_date` |
| `res_country_state` | 6 | `id`, `country_id`, `name`, `code`, `create_date`, `write_date` |
| `res_currency` | 10 | `id`, `name`, `symbol`, `position`, `rounding`, `decimal_places`, `active`, `create_date`, `write_date`, `rate` |
| `res_lang` | 11 | `id`, `name`, `code`, `iso_code`, `url_code`, `active`, `direction`, `date_format`, `time_format`, `create_date`, `write_date` |
| `res_partner` | 35 | `id`, `name`, `email`, `phone`, `is_company`, `company_id`, `active`, `create_date`, `write_date`, `signup_token`, `signup_expiration`, `portal_password_hash`, `portal_active`, `property_product_pricelist_id`, `street`, `city`, `zip`, `lang`, `country_id`, `state_id`, `mobile`, `website`, `comment`, `job_position`, `customer_rank`, `vendor_rank`, `is_contractor`, `is_individual`, `company_name`, `parent_id`, `commercial_partner_id`, `type`, `street2`, `commercial_company_name`, `display_name` |

### auth — users, groups, companies

`modules/auth/AuthModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `ir_cron` | 11 | `id`, `code`, `name`, `interval_minutes`, `next_run`, `last_run`, `active`, `failure_count`, `last_error`, `create_date`, `write_date` |
| `res_company` | 23 | `id`, `name`, `email`, `phone`, `website`, `vat`, `parent_id`, `active`, `create_date`, `write_date`, `partner_id`, `currency_id`, `reg_number`, `street`, `street2`, `street3`, `city_country`, `bank_name`, `bank_account_name`, `bank_account_no`, `bank_address`, `bank_swift`, `payment_term_days` |
| `res_company_users_rel` | 2 | `company_id`, `user_id` |
| `res_groups` | 7 | `id`, `name`, `full_name`, `share`, `permissions`, `create_date`, `write_date` |
| `res_groups_users_rel` | 2 | `gid`, `uid` |
| `res_users` | 11 | `id`, `login`, `password`, `partner_id`, `company_id`, `lang`, `tz`, `active`, `share`, `create_date`, `write_date` |

### ir — the technical registry

`modules/ir/IrModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `audit_log` | 6 | `id`, `model`, `operation`, `record_ids`, `uid`, `created_at` |
| `ir_act_window` | 12 | `id`, `name`, `res_model`, `view_mode`, `domain`, `context`, `target`, `path`, `help`, `active`, `create_date`, `write_date` |
| `ir_ai_prompt` | 4 | `task`, `body`, `updated_by`, `write_date` |
| `ir_ai_provider` | 12 | `name`, `label`, `api_key`, `base_url`, `path`, `model`, `auth_style`, `workspace_id`, `write_date`, `search_style`, `search_tool`, `search_path` |
| `ir_ai_settings` | 17 | `id`, `enabled`, `provider`, `api_key`, `model`, `max_output_tokens`, `daily_call_cap`, `calls_today`, `calls_date`, `last_ok_at`, `last_error`, `create_date`, `write_date`, `api_base_url`, `workspace_id`, `web_search`, `max_candidates` |
| `ir_attachment` | 18 | `id`, `name`, `description`, `res_model`, `res_id`, `res_field`, `type`, `url`, `mimetype`, `file_size`, `checksum`, `store_fname`, `public`, `company_id`, `create_uid`, `create_date`, `write_date`, `document_type` |
| `ir_config_parameter` | 5 | `id`, `key`, `value`, `create_date`, `write_date` |
| `ir_model_data` | 8 | `id`, `module`, `name`, `model`, `res_id`, `noupdate`, `create_date`, `write_date` |
| `ir_rule` | 12 | `id`, `name`, `model_name`, `domain_force`, `perm_read`, `perm_write`, `perm_create`, `perm_unlink`, `global`, `active`, `create_date`, `write_date` |
| `ir_rule_group_rel` | 2 | `rule_id`, `group_id` |
| `ir_ui_menu` | 9 | `id`, `name`, `parent_id`, `sequence`, `action_id`, `web_icon`, `active`, `create_date`, `write_date` |

### mail — chatter

`modules/mail/MailModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `mail_message` | 7 | `id`, `res_model`, `res_id`, `author_id`, `body`, `subtype`, `date` |

### account — accounting

`modules/account/AccountModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `account_account` | 12 | `id`, `name`, `code`, `account_type`, `internal_group`, `currency_id`, `company_id`, `reconcile`, `active`, `note`, `create_date`, `write_date` |
| `account_account_type` | 6 | `id`, `name`, `code`, `internal_group`, `create_date`, `write_date` |
| `account_analytic_account` | 8 | `id`, `name`, `code`, `partner_id`, `company_id`, `active`, `create_date`, `write_date` |
| `account_analytic_line` | 10 | `id`, `name`, `date`, `amount`, `account_id`, `general_account_id`, `move_line_id`, `company_id`, `create_date`, `write_date` |
| `account_asset` | 16 | `id`, `name`, `asset_type_id`, `value`, `value_residual`, `acquisition_date`, `number`, `period_months`, `account_asset_id`, `account_depreciation_id`, `account_expense_id`, `journal_id`, `state`, `company_id`, `create_date`, `write_date` |
| `account_asset_depreciation_line` | 11 | `id`, `asset_id`, `sequence`, `depreciation_date`, `amount`, `remaining_value`, `depreciated_value`, `move_id`, `posted`, `create_date`, `write_date` |
| `account_asset_type` | 11 | `id`, `name`, `number`, `period_months`, `account_asset_id`, `account_depreciation_id`, `account_expense_id`, `journal_id`, `company_id`, `create_date`, `write_date` |
| `account_bank_account` | 10 | `id`, `name`, `bank_name`, `account_number`, `journal_id`, `currency_id`, `company_id`, `active`, `create_date`, `write_date` |
| `account_bank_account_line` | 9 | `id`, `bank_account_id`, `sequence`, `date`, `name`, `debit`, `credit`, `create_date`, `write_date` |
| `account_bank_statement` | 10 | `id`, `name`, `date`, `journal_id`, `balance_start`, `balance_end`, `state`, `company_id`, `create_date`, `write_date` |
| `account_bank_statement_line` | 12 | `id`, `statement_id`, `date`, `name`, `payment_ref`, `partner_id`, `amount`, `is_reconciled`, `reconciled_move_id`, `company_id`, `create_date`, `write_date` |
| `account_budget` | 8 | `id`, `name`, `date_from`, `date_to`, `state`, `company_id`, `create_date`, `write_date` |
| `account_budget_line` | 7 | `id`, `budget_id`, `post_id`, `planned_amount`, `practical_amount`, `create_date`, `write_date` |
| `account_budget_post` | 6 | `id`, `name`, `account_ids_json`, `company_id`, `create_date`, `write_date` |
| `account_fiscal_position` | 9 | `id`, `name`, `note`, `country`, `auto_apply`, `active`, `company_id`, `create_date`, `write_date` |
| `account_fiscal_position_tax` | 6 | `id`, `position_id`, `tax_src_id`, `tax_dest_id`, `create_date`, `write_date` |
| `account_incoterms` | 6 | `id`, `code`, `name`, `active`, `create_date`, `write_date` |
| `account_journal` | 11 | `id`, `name`, `code`, `type`, `currency_id`, `company_id`, `default_account_id`, `sequence`, `active`, `create_date`, `write_date` |
| `account_journal_group` | 6 | `id`, `name`, `journal_ids_json`, `company_id`, `create_date`, `write_date` |
| `account_move` | 30 | `id`, `name`, `ref`, `narration`, `move_type`, `state`, `date`, `invoice_date`, `due_date`, `journal_id`, `partner_id`, `company_id`, `currency_id`, `payment_term_id`, `invoice_origin`, `payment_state`, `amount_untaxed`, `amount_tax`, `amount_total`, `amount_residual`, `create_date`, `write_date`, `sale_id`, `purchase_id`, `rental_contract_id`, `line_precision`, `currency_rate`, `amount_total_base`, `amount_residual_base`, `reversed_entry_id` |
| `account_move_line` | 22 | `id`, `move_id`, `account_id`, `journal_id`, `company_id`, `date`, `name`, `ref`, `partner_id`, `debit`, `credit`, `balance`, `amount_currency`, `quantity`, `price_unit`, `display_type`, `tax_line_id`, `reconciled`, `create_date`, `write_date`, `tax_ids_json`, `analytic_account_id` |
| `account_payment` | 17 | `id`, `name`, `date`, `journal_id`, `partner_id`, `company_id`, `currency_id`, `amount`, `payment_type`, `partner_type`, `state`, `move_id`, `memo`, `create_date`, `write_date`, `amount_base`, `currency_rate` |
| `account_payment_term` | 7 | `id`, `name`, `note`, `lines_json`, `active`, `create_date`, `write_date` |
| `account_tax` | 13 | `id`, `name`, `amount`, `amount_type`, `type_tax_use`, `price_include`, `company_id`, `active`, `description`, `create_date`, `write_date`, `account_id`, `tax_group` |

### uom — units of measure

`modules/uom/UomModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `uom_uom` | 9 | `id`, `name`, `category`, `uom_type`, `factor`, `rounding`, `active`, `create_date`, `write_date` |

### product — catalogue and electronic parts

`modules/product/ProductModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `part_footprint` | 5 | `id`, `name`, `description`, `create_date`, `write_date` |
| `part_lookup_result` | 14 | `id`, `query`, `mpn`, `manufacturer`, `state`, `payload`, `issues`, `product_id`, `categ_id`, `source`, `confidence`, `company_id`, `create_date`, `write_date` |
| `part_manufacturer_info` | 7 | `id`, `product_id`, `manufacturer_id`, `part_number`, `notes`, `create_date`, `write_date` |
| `part_parameter` | 10 | `id`, `product_id`, `name`, `value_numeric`, `unit_id`, `value_text`, `create_date`, `write_date`, `value_base`, `quantity_kind` |
| `part_unit` | 8 | `id`, `name`, `symbol`, `create_date`, `write_date`, `quantity_kind`, `factor`, `is_base` |
| `product_attribute` | 5 | `id`, `name`, `sequence`, `create_date`, `write_date` |
| `product_attribute_value` | 6 | `id`, `attribute_id`, `name`, `sequence`, `create_date`, `write_date` |
| `product_category` | 10 | `id`, `name`, `parent_id`, `active`, `create_date`, `write_date`, `property_stock_valuation_account_id`, `property_stock_journal_id`, `property_stock_account_input_id`, `property_stock_account_output_id` |
| `product_pricelist` | 8 | `id`, `name`, `currency_id`, `company_id`, `sequence`, `active`, `create_date`, `write_date` |
| `product_pricelist_item` | 18 | `id`, `pricelist_id`, `applied_on`, `product_id`, `product_tmpl_id`, `categ_id`, `min_quantity`, `date_start`, `date_end`, `compute_price`, `fixed_price`, `percent_price`, `base`, `price_discount`, `price_surcharge`, `sequence`, `create_date`, `write_date` |
| `product_product` | 39 | `id`, `name`, `default_code`, `barcode`, `description`, `type`, `categ_id`, `uom_id`, `uom_po_id`, `list_price`, `standard_price`, `volume`, `weight`, `sale_ok`, `purchase_ok`, `company_id`, `active`, `create_date`, `write_date`, `qty_available`, `cost_method`, `quantity_svl`, `value_svl`, `tracking`, `expense_ok`, `image_1920`, `description_sale`, `description_purchase`, `income_account_id`, `expense_account_id`, `invoice_policy`, `sale_line_warn`, `sale_line_warn_msg`, `purchase_method`, `purchase_lead_time`, `purchase_line_warn`, `purchase_line_warn_msg`, `footprint_id`, `product_tmpl_id` |
| `product_supplierinfo` | 12 | `id`, `product_id`, `partner_id`, `product_name`, `product_code`, `min_qty`, `price`, `delay`, `sequence`, `company_id`, `create_date`, `write_date` |
| `product_template` | 24 | `id`, `name`, `default_code`, `description`, `description_sale`, `description_purchase`, `type`, `categ_id`, `uom_id`, `uom_po_id`, `list_price`, `standard_price`, `tracking`, `invoice_policy`, `purchase_method`, `income_account_id`, `expense_account_id`, `sale_ok`, `purchase_ok`, `image_1920`, `company_id`, `active`, `create_date`, `write_date` |
| `product_template_attribute_line` | 6 | `id`, `product_tmpl_id`, `attribute_id`, `sequence`, `create_date`, `write_date` |
| `product_template_attribute_value` | 7 | `id`, `line_id`, `value_id`, `price_extra`, `create_date`, `write_date`, `active` |
| `product_variant_combination` | 2 | `product_id`, `ptav_id` |

### sale

`modules/sale/SaleModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `sale_order` | 24 | `id`, `name`, `state`, `partner_id`, `partner_invoice_id`, `partner_shipping_id`, `date_order`, `validity_date`, `payment_term_id`, `note`, `currency_id`, `company_id`, `user_id`, `client_order_ref`, `origin`, `invoice_status`, `amount_untaxed`, `amount_tax`, `amount_total`, `create_date`, `write_date`, `line_precision`, `currency_rate`, `pricelist_id` |
| `sale_order_line` | 20 | `id`, `order_id`, `sequence`, `product_id`, `name`, `product_uom_qty`, `product_uom_id`, `price_unit`, `discount`, `tax_ids_json`, `price_subtotal`, `price_tax`, `price_total`, `qty_invoiced`, `qty_delivered`, `company_id`, `currency_id`, `display_type`, `create_date`, `write_date` |

### purchase

`modules/purchase/PurchaseModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `purchase_order` | 20 | `id`, `name`, `state`, `partner_id`, `date_order`, `date_planned`, `payment_term_id`, `note`, `currency_id`, `company_id`, `user_id`, `origin`, `invoice_status`, `amount_untaxed`, `amount_tax`, `amount_total`, `create_date`, `write_date`, `line_precision`, `currency_rate` |
| `purchase_order_line` | 20 | `id`, `order_id`, `sequence`, `product_id`, `name`, `product_qty`, `product_uom_id`, `price_unit`, `discount`, `tax_ids_json`, `price_subtotal`, `price_tax`, `price_total`, `date_planned`, `qty_invoiced`, `qty_received`, `company_id`, `currency_id`, `create_date`, `write_date` |

### hr — employees, expenses, attendance, leave

`modules/hr/HrAttendance.cpp`, `modules/hr/HrLeave.cpp`, `modules/hr/HrModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `hr_attendance` | 8 | `id`, `employee_id`, `check_in`, `check_out`, `worked_hours`, `company_id`, `create_date`, `write_date` |
| `hr_department` | 8 | `id`, `name`, `parent_id`, `company_id`, `active`, `create_date`, `write_date`, `manager_id` |
| `hr_employee` | 22 | `id`, `name`, `job_id`, `department_id`, `parent_id`, `coach_id`, `work_email`, `work_phone`, `mobile_phone`, `resource_calendar_id`, `company_id`, `user_id`, `address_id`, `gender`, `marital`, `birthday`, `identification_id`, `private_email`, `active`, `create_date`, `write_date`, `pin_hash` |
| `hr_expense` | 18 | `id`, `name`, `employee_id`, `sheet_id`, `date`, `product_id`, `account_id`, `quantity`, `unit_amount`, `total_amount`, `tax_id`, `tax_amount`, `payment_mode`, `reference`, `state`, `company_id`, `create_date`, `write_date` |
| `hr_expense_sheet` | 14 | `id`, `name`, `employee_id`, `date`, `total_amount`, `payment_mode`, `state`, `journal_id`, `move_id`, `payment_move_id`, `note`, `company_id`, `create_date`, `write_date` |
| `hr_job` | 8 | `id`, `name`, `description`, `department_id`, `company_id`, `active`, `create_date`, `write_date` |
| `hr_leave` | 11 | `id`, `employee_id`, `leave_type_id`, `date_from`, `date_to`, `number_of_days`, `state`, `reason`, `company_id`, `create_date`, `write_date` |
| `hr_leave_allocation` | 11 | `id`, `employee_id`, `leave_type_id`, `number_of_days`, `date_from`, `date_to`, `state`, `notes`, `company_id`, `create_date`, `write_date` |
| `hr_leave_type` | 11 | `id`, `name`, `code`, `requires_allocation`, `is_paid`, `max_days_per_request`, `color`, `company_id`, `active`, `create_date`, `write_date` |
| `hr_public_holiday` | 6 | `id`, `name`, `date`, `company_id`, `create_date`, `write_date` |
| `resource_calendar` | 7 | `id`, `name`, `hours_per_day`, `company_id`, `active`, `create_date`, `write_date` |

### stock — warehouse

`modules/stock/StockModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `stock_landed_cost` | 8 | `id`, `name`, `date`, `picking_id`, `state`, `company_id`, `create_date`, `write_date` |
| `stock_landed_cost_line` | 9 | `id`, `landed_cost_id`, `name`, `product_id`, `price`, `split_method`, `account_id`, `create_date`, `write_date` |
| `stock_location` | 10 | `id`, `name`, `complete_name`, `location_id`, `usage`, `company_id`, `active`, `create_date`, `write_date`, `barcode` |
| `stock_move` | 19 | `id`, `picking_id`, `product_id`, `product_uom_id`, `name`, `product_uom_qty`, `quantity`, `state`, `location_id`, `location_dest_id`, `company_id`, `origin`, `create_date`, `write_date`, `reserved_qty`, `lot_id`, `result_package_id`, `production_id`, `is_production_raw` |
| `stock_picking` | 15 | `id`, `name`, `picking_type_id`, `state`, `partner_id`, `location_id`, `location_dest_id`, `scheduled_date`, `origin`, `company_id`, `sale_id`, `purchase_id`, `create_date`, `write_date`, `user_id` |
| `stock_picking_type` | 10 | `id`, `name`, `code`, `sequence_prefix`, `default_location_src_id`, `default_location_dest_id`, `company_id`, `active`, `create_date`, `write_date` |
| `stock_production_lot` | 8 | `id`, `name`, `product_id`, `ref`, `company_id`, `create_date`, `write_date`, `barcode` |
| `stock_putaway_rule` | 9 | `id`, `product_id`, `category_id`, `location_in_id`, `location_out_id`, `sequence`, `company_id`, `create_date`, `write_date` |
| `stock_quant` | 9 | `id`, `product_id`, `location_id`, `lot_id`, `quantity`, `reserved_quantity`, `company_id`, `create_date`, `write_date` |
| `stock_quant_package` | 7 | `id`, `name`, `location_id`, `picking_id`, `company_id`, `create_date`, `write_date` |
| `stock_valuation_layer` | 13 | `id`, `product_id`, `quantity`, `unit_cost`, `value`, `remaining_qty`, `remaining_value`, `counterpart_usage`, `description`, `account_move_id`, `company_id`, `create_date`, `write_date` |
| `stock_warehouse` | 12 | `id`, `name`, `code`, `company_id`, `lot_stock_id`, `view_location_id`, `in_type_id`, `out_type_id`, `int_type_id`, `active`, `create_date`, `write_date` |
| `stock_warehouse_orderpoint` | 12 | `id`, `product_id`, `location_id`, `product_min_qty`, `product_max_qty`, `qty_multiple`, `route`, `supplier_id`, `company_id`, `active`, `create_date`, `write_date` |

### mrp — manufacturing

`modules/mrp/MrpModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `mrp_bom` | 14 | `id`, `product_id`, `code`, `bom_type`, `product_qty`, `product_uom_id`, `company_id`, `active`, `create_date`, `write_date`, `bom_kind`, `revision`, `revision_of_id`, `subcontractor_id` |
| `mrp_bom_line` | 11 | `id`, `bom_id`, `product_id`, `product_qty`, `product_uom_id`, `sequence`, `create_date`, `write_date`, `reference_designators`, `note`, `fitted` |
| `mrp_forecast` | 7 | `id`, `product_id`, `date`, `forecast_qty`, `company_id`, `create_date`, `write_date` |
| `mrp_production` | 16 | `id`, `name`, `product_id`, `product_qty`, `product_uom_id`, `bom_id`, `state`, `location_src_id`, `location_dest_id`, `date_planned_start`, `qty_producing`, `origin`, `user_id`, `company_id`, `create_date`, `write_date` |
| `mrp_production_schedule` | 6 | `id`, `product_id`, `min_to_replenish`, `company_id`, `create_date`, `write_date` |
| `mrp_routing_workcenter` | 9 | `id`, `bom_id`, `workcenter_id`, `name`, `sequence`, `time_cycle_manual`, `company_id`, `create_date`, `write_date` |
| `mrp_workcenter` | 10 | `id`, `name`, `code`, `costs_hour`, `time_efficiency`, `capacity`, `company_id`, `active`, `create_date`, `write_date` |
| `mrp_workorder` | 15 | `id`, `production_id`, `workcenter_id`, `operation_id`, `name`, `sequence`, `state`, `duration_expected`, `duration`, `qty_produced`, `date_start`, `date_finished`, `company_id`, `create_date`, `write_date` |

### bom — the BOM importer

`modules/bom/BomModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `mrp_bom_import_line` | 17 | `id`, `bom_id`, `sequence`, `designators`, `quantity`, `mpn`, `manufacturer`, `value_text`, `footprint`, `description`, `product_id`, `severity`, `issues`, `candidates`, `fitted`, `create_date`, `write_date` |

### project

`modules/project/ProjectModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `project_project` | 15 | `id`, `name`, `code`, `description`, `partner_id`, `user_id`, `company_id`, `date_start`, `date_end`, `sequence`, `color`, `allow_timesheets`, `active`, `create_date`, `write_date` |
| `project_task` | 18 | `id`, `name`, `description`, `project_id`, `stage_id`, `user_id`, `partner_id`, `parent_id`, `company_id`, `date_deadline`, `date_end`, `kanban_state`, `sequence`, `priority`, `planned_hours`, `active`, `create_date`, `write_date` |
| `project_task_type` | 9 | `id`, `name`, `project_id`, `sequence`, `fold`, `is_closed`, `active`, `create_date`, `write_date` |
| `project_timesheet` | 11 | `id`, `name`, `date`, `project_id`, `task_id`, `employee_id`, `user_id`, `company_id`, `unit_amount`, `create_date`, `write_date` |

### help

`modules/help/HelpModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `help_article` | 13 | `id`, `book`, `book_label`, `slug`, `title`, `body`, `keywords`, `parent_id`, `sequence`, `is_section`, `active`, `create_date`, `write_date` |

### report

`modules/report/ReportModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `ir_report_template` | 24 | `id`, `name`, `model`, `template_html`, `paper_format`, `orientation`, `active`, `decimal_qty`, `decimal_price`, `decimal_subtotal`, `margin_top`, `margin_right`, `margin_bottom`, `margin_left`, `font_size`, `font_color`, `line_height`, `footer_text`, `footer_show_page_num`, `footer_page_num_fmt`, `footer_text_source`, `footer_line_color`, `footer_line_width`, `default_html` |

### portal — the customer portal

`modules/portal/PortalAccess.cpp`, `modules/portal/PortalModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `partner_rental_price` | 4 | `id`, `partner_id`, `product_id`, `price_unit` |
| `payment_proof` | 7 | `id`, `invoice_id`, `partner_id`, `filename`, `mimetype`, `filepath`, `upload_date` |
| `portal_access_token` | 8 | `id`, `model`, `res_id`, `token`, `created_at`, `expires_at`, `revoked`, `created_uid` |

### rental

`modules/rental/RentalMigrations.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `rental_contract` | 20 | `id`, `name`, `partner_id`, `state`, `date_start`, `date_cancelled`, `billing_period`, `billing_lead_days`, `payment_term_id`, `deposit_amount`, `deposit_state`, `currency_id`, `journal_id`, `notes`, `company_id`, `active`, `create_date`, `write_date`, `billing_interval`, `billing_unit` |
| `rental_contract_line` | 22 | `id`, `contract_id`, `unit_id`, `date_start`, `date_end`, `unit_price`, `discount_pct`, `tax_ids_json`, `billing_anchor_day`, `next_period_start`, `invoiced_through`, `proration_policy`, `state`, `company_id`, `create_date`, `write_date`, `partner_id`, `billing_mode`, `billing_months`, `billing_lead_days`, `billing_interval`, `billing_unit` |
| `rental_event` | 14 | `id`, `occurred_at`, `event_type`, `contract_id`, `line_id`, `unit_id`, `partner_id`, `user_id`, `summary`, `detail`, `ref_model`, `ref_id`, `company_id`, `create_date` |
| `rental_expense` | 19 | `id`, `date`, `name`, `category_id`, `amount`, `partner_id`, `unit_id`, `contract_id`, `account_id`, `state`, `move_id`, `is_recurring`, `recurrence_interval`, `recurrence_next_date`, `recurrence_end_date`, `recurrence_parent_id`, `company_id`, `create_date`, `write_date` |
| `rental_expense_category` | 8 | `id`, `name`, `account_id`, `is_operating`, `company_id`, `active`, `create_date`, `write_date` |
| `rental_invoice_link` | 9 | `id`, `move_id`, `contract_id`, `contract_line_id`, `period_start`, `period_end`, `amount`, `company_id`, `create_date` |
| `rental_unit` | 16 | `id`, `code`, `name`, `type_id`, `site`, `zone`, `floor`, `area_sqm`, `volume_m3`, `state`, `location_id`, `notes`, `company_id`, `active`, `create_date`, `write_date` |
| `rental_unit_type` | 12 | `id`, `name`, `code`, `default_rate`, `default_period`, `tax_ids_json`, `area_sqm`, `volume_m3`, `company_id`, `active`, `create_date`, `write_date` |

### website — the CMS

`modules/website/WebsiteForm.cpp`, `modules/website/WebsiteModule.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `website_form` | 10 | `id`, `slug`, `title`, `description`, `submit_label`, `success_message`, `target_model`, `active`, `create_date`, `write_date` |
| `website_form_field` | 11 | `id`, `form_id`, `name`, `label`, `field_type`, `placeholder`, `options`, `required`, `sequence`, `create_date`, `write_date` |
| `website_form_submission` | 8 | `id`, `form_id`, `data_json`, `state`, `source_ip`, `task_id`, `create_date`, `write_date` |
| `website_menu` | 9 | `id`, `name`, `url`, `page_id`, `parent_id`, `sequence`, `new_window`, `create_date`, `write_date` |
| `website_page` | 17 | `id`, `slug`, `title`, `blocks_json`, `is_published`, `is_indexed`, `is_homepage`, `sequence`, `meta_title`, `meta_description`, `meta_keywords`, `create_date`, `write_date`, `page_kind`, `publish_date`, `author`, `excerpt` |
| `website_page_revision` | 8 | `id`, `page_id`, `blocks_json`, `title`, `author_uid`, `author_name`, `note`, `create_date` |

### core — cross-cutting

`core/ControlPlane.cpp`, `core/MoneyMigrations.cpp`, `core/infrastructure/MigrationRunner.cpp`

| Table | Cols | Columns |
|---|---:|---|
| `account_partial_reconcile` | 10 | `id`, `payment_id`, `move_id`, `amount`, `amount_base`, `fx_diff`, `date`, `company_id`, `create_date`, `write_date` |
| `decimal_precision` | 3 | `id`, `name`, `digits` |
| `ir_sequence` | 14 | `id`, `code`, `name`, `prefix`, `suffix`, `padding`, `number_next`, `number_increment`, `reset_policy`, `last_reset_period`, `company_id`, `active`, `create_date`, `write_date` |
| `schema_migrations` | 3 | `version`, `description`, `applied_at` |

### The control plane — a **separate database**

`mc_membership` and `mc_shared_product` are not in a tenant database at all.
They live in `mc_control`, which `core/ControlPlane.cpp` provisions, and they
are how one identity reaches several tenants. Looking for them in `odoo` and
not finding them is expected.

| Table | Cols | Columns |
|---|---:|---|
| `mc_membership` | 4 | `identity`, `tenant_db`, `local_login`, `active` |
| `mc_shared_product` | 3 | `code`, `name`, `list_price` |

See [../architecture/multi-company.md](../architecture/multi-company.md).
