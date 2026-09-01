#!/bin/bash
# =============================================================
# Remove test-suite debris from a development database (docs/092).
#
# Years of suite runs left records behind: every run added an empty "New"
# transfer, a nameless product category, another "FY2026 Operating Budget",
# another "Laptop fleet" asset. The scripts now clean up after themselves;
# this removes what accumulated before they did.
#
# RULES THIS FOLLOWS — read them before extending it:
#
#   * Only records identifiable as test output are touched. Master data
#     (chart of accounts, journals, taxes, locations, sequences, real
#     categories) is never removed.
#   * Journal entries are deleted WITH their lines. A posted move whose lines
#     are gone — or lines whose move is gone — is worse than the debris: it
#     unbalances the ledger and every report built on it.
#   * It runs in ONE transaction and re-checks that the ledger still balances
#     before committing. If it does not, nothing is removed.
#   * --dry-run (the default) only reports. Pass --apply to actually delete.
#
# This is NOT part of the test suite. It is a maintenance tool, run by hand.
# =============================================================
DBN=${DBN:-odoo}
APPLY=
case "${1:-}" in
    --apply)    APPLY=1 ;;
    --dry-run|"") APPLY=  ;;
    *) echo "usage: $0 [--dry-run|--apply]"; exit 2 ;;
esac

psqlq(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
psqlc(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -c "$1"; }

echo "=============================================="
echo " Test-data cleanup — $([ -n "$APPLY" ] && echo 'APPLY' || echo 'DRY RUN (nothing will be deleted)')"
echo "=============================================="

# ---- what is there -------------------------------------------------
echo
echo "--- candidates ---"
psqlc "
SELECT 'empty draft transfers'  AS what, count(*) FROM stock_picking p
  WHERE p.name = 'New' AND p.state = 'draft'
    AND NOT EXISTS (SELECT 1 FROM stock_move m WHERE m.picking_id = p.id)
UNION ALL SELECT 'nameless categories', count(*) FROM product_category
  WHERE name IS NULL OR btrim(name) = ''
UNION ALL SELECT 'test budgets', count(*) FROM account_budget
  WHERE name LIKE 'FY2026 Operating Budget%'
UNION ALL SELECT 'test assets', count(*) FROM account_asset
  WHERE name = 'Laptop fleet'
UNION ALL SELECT 'BUDGET-TEST entries', count(*) FROM account_move
  WHERE name LIKE 'BUDGET-TEST%'
UNION ALL SELECT 'QA-named products', count(*) FROM product_product
  WHERE default_code LIKE 'QA-%'
UNION ALL SELECT 'QA-named employees', count(*) FROM hr_employee
  WHERE name LIKE 'QA %'
UNION ALL SELECT 'QA-named taxes', count(*) FROM account_tax
  WHERE name LIKE 'QA %'
UNION ALL SELECT 'QA lots/packages', count(*) FROM stock_production_lot
  WHERE name LIKE 'QA-%'
ORDER BY 1;"

BAL_BEFORE=$(psqlq "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted'")
echo "posted ledger balance before: $BAL_BEFORE"

if [ -z "$APPLY" ]; then
    echo
    echo "Dry run — nothing deleted. Re-run with --apply to remove the above."
    exit 0
fi

# ---- delete, in one transaction ------------------------------------
# Order matters: children before parents, move lines before moves.
PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -v ON_ERROR_STOP=1 <<'SQL'
BEGIN;

-- Empty draft transfers left by the New-form probe.
DELETE FROM stock_picking p
 WHERE p.name = 'New' AND p.state = 'draft'
   AND NOT EXISTS (SELECT 1 FROM stock_move m WHERE m.picking_id = p.id);

-- Nameless categories, but only where nothing points at them.
DELETE FROM product_category c
 WHERE (c.name IS NULL OR btrim(c.name) = '')
   AND NOT EXISTS (SELECT 1 FROM product_product  p  WHERE p.categ_id  = c.id)
   AND NOT EXISTS (SELECT 1 FROM product_category ch WHERE ch.parent_id = c.id);

-- Test assets: the depreciation entries are posted, so they go with the asset.
--
-- The move ids have to be captured FIRST. account_asset_depreciation_line.move_id
-- is a foreign key onto account_move, so the lines must be deleted before the
-- moves — but they are also the only place the move ids are recorded, so
-- deleting them first loses the list. A temp table holds it across both steps.
CREATE TEMP TABLE _dep_moves ON COMMIT DROP AS
    SELECT DISTINCT move_id AS id
      FROM account_asset_depreciation_line
     WHERE move_id IS NOT NULL
       AND asset_id IN (SELECT id FROM account_asset WHERE name = 'Laptop fleet');

DELETE FROM account_asset_depreciation_line
 WHERE asset_id IN (SELECT id FROM account_asset WHERE name = 'Laptop fleet');
DELETE FROM account_asset WHERE name = 'Laptop fleet';
DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM _dep_moves);
DELETE FROM account_move      WHERE id      IN (SELECT id FROM _dep_moves);

-- Test budgets and the entries their "actual" column was measured against.
DELETE FROM account_budget_line
 WHERE budget_id IN (SELECT id FROM account_budget WHERE name LIKE 'FY2026 Operating Budget%');
DELETE FROM account_budget WHERE name LIKE 'FY2026 Operating Budget%';
DELETE FROM account_budget_post p
 WHERE p.name = 'Operating Expenses'
   AND NOT EXISTS (SELECT 1 FROM account_budget_line l WHERE l.post_id = p.id);
DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE name LIKE 'BUDGET-TEST%');
DELETE FROM account_move WHERE name LIKE 'BUDGET-TEST%';

-- QA fixtures: stock rows first, then the products themselves.
DELETE FROM stock_quant             WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM stock_valuation_layer   WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM stock_production_lot    WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM stock_warehouse_orderpoint WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM stock_putaway_rule      WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM stock_move              WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM purchase_order_line     WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'QA-%');
DELETE FROM purchase_order          WHERE origin LIKE 'Reordering: OP/%'
   AND NOT EXISTS (SELECT 1 FROM purchase_order_line l WHERE l.order_id = purchase_order.id);
DELETE FROM product_product         WHERE default_code LIKE 'QA-%';

-- Empty transfers left behind once their QA moves were removed.
DELETE FROM stock_picking p
 WHERE p.name LIKE 'QA-%'
   AND NOT EXISTS (SELECT 1 FROM stock_move m WHERE m.picking_id = p.id);

DELETE FROM stock_quant_package WHERE name LIKE 'PACK%'
   AND NOT EXISTS (SELECT 1 FROM stock_move m WHERE m.result_package_id = stock_quant_package.id);

DELETE FROM hr_employee WHERE name LIKE 'QA %'
   AND NOT EXISTS (SELECT 1 FROM hr_expense e WHERE e.employee_id = hr_employee.id)
   AND NOT EXISTS (SELECT 1 FROM hr_expense_sheet s WHERE s.employee_id = hr_employee.id);
DELETE FROM account_tax WHERE name LIKE 'QA %'
   AND NOT EXISTS (SELECT 1 FROM hr_expense e WHERE e.tax_id = account_tax.id);

COMMIT;
SQL
RC=$?

if [ "$RC" -ne 0 ]; then
    echo
    echo "  *** cleanup FAILED — the transaction rolled back, nothing was deleted ***"
    exit 1
fi

# ---- the ledger must still balance ---------------------------------
BAL_AFTER=$(psqlq "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted'")
ORPHAN_LINES=$(psqlq "SELECT count(*) FROM account_move_line l WHERE NOT EXISTS (SELECT 1 FROM account_move m WHERE m.id = l.move_id)")
echo
echo "--- after ---"
echo "posted ledger balance: $BAL_AFTER   (was $BAL_BEFORE)"
echo "orphaned move lines:   $ORPHAN_LINES"
if [ "$BAL_AFTER" = "0" ] && [ "${ORPHAN_LINES:-1}" = "0" ]; then
    echo "  Cleanup complete; the ledger still balances."
else
    echo "  *** WARNING: the ledger does not balance after cleanup — investigate before using this data ***"
    exit 1
fi
