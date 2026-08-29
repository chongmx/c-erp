#!/bin/bash
# --- harness ---------------------------------------------------------------
# Walk up for CMakeLists.txt rather than counting `../`, so this test behaves
# the same whether the runner invoked it or you ran it directly, and so it can
# be nested a folder deeper without a preamble edit.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# PHASE 3 of 3 — DELETE.  (docs/109)
#
# Runs LAST, after every other integration script. Two jobs, mirroring the
# create phase:
#
#   1. it removes the canonical set, and
#   2. it TESTS the removal — which is the half nobody writes and the half that
#      catches real defects.
#
# Deleting is where a data model tells the truth. A row that will not delete
# because something still points at it, or a child left behind when its parent
# goes, is a modelling bug that no amount of creating and reading will reveal.
# So this asserts more than "the rows are gone":
#
#   * nothing the fixtures owned survives,
#   * no orphaned children are left pointing at deleted parents,
#   * deleting is idempotent — running it twice is not an error,
#   * and the wider database is untouched: this phase must not take demo data,
#     reference data or another script's rows with it.
# =============================================================
# (repository root: handled by the harness preamble)
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
source tests/lib/fixtures.sh

echo "############ before ############"
echo "    $(fx_report | tr '\n' ' ')"

# What the rest of the database looks like, so we can prove we did not touch it.
OTHER_PROD=$(fxq "SELECT count(*) FROM product_product WHERE default_code NOT LIKE 'FX-%' OR default_code IS NULL")
OTHER_PART=$(fxq "SELECT count(*) FROM res_partner    WHERE name NOT LIKE 'FX %'")
OTHER_MOVE=$(fxq "SELECT count(*) FROM account_move   WHERE name NOT LIKE 'FX-%' OR name IS NULL")
CATS=$(fxq "SELECT count(*) FROM product_category")
UNITS=$(fxq "SELECT count(*) FROM part_unit")
echo "    elsewhere: products=$OTHER_PROD partners=$OTHER_PART moves=$OTHER_MOVE categories=$CATS units=$UNITS"

# Capture the ids before they go, so the orphan checks have something to look for.
SALE_ID=$(fxq "SELECT id FROM sale_order WHERE name='FX-SO-1'")
INV_ID=$(fxq "SELECT id FROM account_move WHERE name='FX-INV-1'")
PROD_ID=$(fxq "SELECT id FROM product_product WHERE default_code='FX-PROD-1'")

echo "############ delete ############"
if fx_drop; then ok "fx_drop completed"; else no "fx_drop failed"; fi

echo "############ nothing the fixtures owned survives ############"
[ "$(fxq "SELECT count(*) FROM product_product WHERE default_code LIKE 'FX-%'")" = "0" ] && ok "products gone"       || no "fixture products remain"
[ "$(fxq "SELECT count(*) FROM sale_order      WHERE name LIKE 'FX-%'")"        = "0" ] && ok "sale orders gone"    || no "fixture orders remain"
[ "$(fxq "SELECT count(*) FROM account_move    WHERE name LIKE 'FX-%'")"        = "0" ] && ok "invoices gone"       || no "fixture invoices remain"
[ "$(fxq "SELECT count(*) FROM res_partner     WHERE name LIKE 'FX %'")"        = "0" ] && ok "partners gone"       || no "fixture partners remain"

echo "############ no orphans left behind ############"
# The check that earns its keep. A child pointing at a deleted parent is
# invisible until something joins on it and silently returns nothing.
if [ -n "$SALE_ID" ]; then
    [ "$(fxq "SELECT count(*) FROM sale_order_line WHERE order_id=$SALE_ID")" = "0" ] \
      && ok "no sale order lines orphaned" || no "sale order lines survived their order"
fi
if [ -n "$INV_ID" ]; then
    [ "$(fxq "SELECT count(*) FROM account_move_line WHERE move_id=$INV_ID")" = "0" ] \
      && ok "no journal items orphaned" || no "journal items survived their move"
fi
if [ -n "$PROD_ID" ]; then
    [ "$(fxq "SELECT count(*) FROM sale_order_line WHERE product_id=$PROD_ID")" = "0" ] \
      && ok "no sale lines point at the deleted product" || no "sale lines still reference the product"
    [ "$(fxq "SELECT count(*) FROM part_parameter WHERE product_id=$PROD_ID")" = "0" ] \
      && ok "no part parameters orphaned" || no "part parameters survived their product"
fi
# A general sweep: any line whose order no longer exists at all.
DANGLING=$(fxq "SELECT count(*) FROM sale_order_line l
                 WHERE NOT EXISTS (SELECT 1 FROM sale_order o WHERE o.id = l.order_id)")
[ "${DANGLING:-0}" = "0" ] && ok "no dangling sale order lines anywhere" || no "$DANGLING dangling sale order lines"

echo "############ deleting twice is not an error ############"
if fx_drop; then ok "fx_drop is idempotent" || true; else no "a second fx_drop failed"; fi
[ "$(fxq "SELECT count(*) FROM product_product WHERE default_code LIKE 'FX-%'")" = "0" ] \
  && ok "still clean after the second run" || no "the second drop changed something"

echo "############ the rest of the database is untouched ############"
# Teardown that quietly takes demo or reference data with it would be far worse
# than leaving fixtures behind.
[ "$(fxq "SELECT count(*) FROM product_product WHERE default_code NOT LIKE 'FX-%' OR default_code IS NULL")" = "$OTHER_PROD" ] \
  && ok "other products untouched ($OTHER_PROD)" || no "other products changed"
[ "$(fxq "SELECT count(*) FROM res_partner WHERE name NOT LIKE 'FX %'")" = "$OTHER_PART" ] \
  && ok "other partners untouched ($OTHER_PART)" || no "other partners changed"
[ "$(fxq "SELECT count(*) FROM account_move WHERE name NOT LIKE 'FX-%' OR name IS NULL")" = "$OTHER_MOVE" ] \
  && ok "other journal entries untouched ($OTHER_MOVE)" || no "other journal entries changed"
[ "$(fxq "SELECT count(*) FROM product_category")" = "$CATS" ] \
  && ok "categories untouched ($CATS)"  || no "categories changed — reference data was deleted"
[ "$(fxq "SELECT count(*) FROM part_unit")" = "$UNITS" ] \
  && ok "part units untouched ($UNITS)" || no "part units changed — reference data was deleted"

echo "############ after ############"
echo "    $(fx_report | tr '\n' ' ')"

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"
