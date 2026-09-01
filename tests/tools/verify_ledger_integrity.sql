-- =============================================================
-- P2 Phase 5 — ledger integrity, asserted EXACTLY.
--
-- The point of int64 money is that these are equalities, not
-- tolerance checks. Before P2 every one of these would have needed
-- an epsilon; now any non-zero row is a real defect.
--
--   PGPASSWORD=odoo psql -h localhost -U odoo -d odoo -f tests/tools/verify_ledger_integrity.sql
-- =============================================================
\set ON_ERROR_STOP on
\pset footer off

-- account_move_line carries debit/credit, not price_total: the revenue side
-- of a customer invoice is the sum of its credit lines. This mirrors how
-- AccountModule recomputes the header (untaxed = SUM(credit) on product lines).
\echo '=== 1. invoice untaxed equals the sum of its revenue lines (exact) ==='
SELECT m.id, m.amount_untaxed AS header, COALESCE(SUM(l.credit), 0) AS lines_sum
  FROM account_move m
  LEFT JOIN account_move_line l
         ON l.move_id = m.id AND l.display_type = '' AND l.credit > 0
 WHERE m.move_type IN ('out_invoice', 'out_refund')
 GROUP BY m.id, m.amount_untaxed
HAVING m.amount_untaxed <> COALESCE(SUM(l.credit), 0);
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 1b. total = untaxed + tax, exactly ==='
SELECT id, amount_untaxed, amount_tax, amount_total
  FROM account_move
 WHERE amount_total <> amount_untaxed + amount_tax;
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 2. every journal entry balances: SUM(debit) = SUM(credit) ==='
SELECT move_id, SUM(debit) AS dr, SUM(credit) AS cr, SUM(debit) - SUM(credit) AS diff
  FROM account_move_line
 GROUP BY move_id
HAVING SUM(debit) <> SUM(credit);
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 3. sale order totals equal the sum of their lines ==='
SELECT o.id, o.amount_total AS header, COALESCE(SUM(l.price_total), 0) AS lines_sum
  FROM sale_order o
  LEFT JOIN sale_order_line l ON l.order_id = o.id
 GROUP BY o.id, o.amount_total
HAVING o.amount_total <> COALESCE(SUM(l.price_total), 0);
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 4. purchase order totals equal the sum of their lines ==='
SELECT o.id, o.amount_total AS header, COALESCE(SUM(l.price_total), 0) AS lines_sum
  FROM purchase_order o
  LEFT JOIN purchase_order_line l ON l.order_id = o.id
 GROUP BY o.id, o.amount_total
HAVING o.amount_total <> COALESCE(SUM(l.price_total), 0);
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 5. residual never exceeds the total, never negative ==='
SELECT id, amount_total, amount_residual
  FROM account_move
 WHERE amount_residual < 0 OR amount_residual > amount_total;
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 6. no money column left as NUMERIC (migration completeness) ==='
SELECT table_name, column_name, data_type
  FROM information_schema.columns
 WHERE data_type = 'numeric'
   AND column_name IN ('amount','amount_currency','amount_residual','amount_tax',
                       'amount_total','amount_untaxed','balance','credit','debit',
                       'discount','list_price','price_subtotal','price_tax',
                       'price_total','price_unit','product_qty','product_uom_qty',
                       'qty_delivered','qty_invoiced','qty_received','quantity',
                       'standard_price','rate')
   -- account_tax.amount is a tax RATE (8.0 = 8%), not a money amount. It is
   -- deliberately NUMERIC and deliberately not marked scaled — confirmed
   -- against AccountTax::registerFields(). Excluded so this check stays a
   -- real signal instead of a standing false positive.
   AND NOT (table_name = 'account_tax' AND column_name = 'amount')
 ORDER BY table_name, column_name;
\echo '    (zero rows above = pass — every money column is BIGINT)'

\echo ''
\echo '=== 7. columns that must NOT have been migrated ==='
SELECT table_name, column_name, data_type
  FROM information_schema.columns
 WHERE column_name IN ('weight','volume','purchase_lead_time','margin_top',
                       'margin_left','line_height','hours_per_day','rounding','factor')
   AND data_type = 'bigint'
 ORDER BY table_name, column_name;
\echo '    (zero rows above = pass — physical/layout values still NUMERIC)'

\echo ''
\echo '=== 8. precision settings are within the storage scale ==='
SELECT name, digits FROM decimal_precision WHERE digits < 0 OR digits > 6;
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== 9. every active currency has a usable rate ==='
SELECT name, rate FROM res_currency WHERE active AND rate <= 0;
\echo '    (zero rows above = pass)'

\echo ''
\echo '=== reference state ==='
SELECT 'base currency' AS item,
       (SELECT c.name FROM res_company co JOIN res_currency c ON c.id = co.currency_id LIMIT 1) AS value
UNION ALL SELECT 'migrations applied', (SELECT count(*)::text FROM schema_migrations WHERE version >= 900)
UNION ALL SELECT 'FX account',         (SELECT code||' '||name FROM account_account WHERE code = '7900' LIMIT 1)
UNION ALL SELECT 'precision rows',     (SELECT count(*)::text FROM decimal_precision);
