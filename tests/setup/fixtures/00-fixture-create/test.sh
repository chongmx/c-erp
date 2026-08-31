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
# PHASE 1 of 3 — CREATE.  (docs/109)
#
# Runs FIRST, before every other integration script, and does two jobs at once:
#
#   1. it creates the canonical data the rest of the suite reads, and
#   2. it TESTS that creation, so a broken fixture fails here — loudly, in one
#      place — instead of surfacing as seven unrelated failures later.
#
# Seven scripts used to fail on a clean database because each looked up "the
# first product" or "the first sale order" and assumed one existed. Rather than
# copy a seeding block into seven files, the set is made once, here, and removed
# once in the delete phase. That makes the whole lifecycle observable:
# create is asserted here, use is the suite, delete is asserted at the end.
#
# Idempotent by design: running it twice must leave exactly one of each, because
# the suite may be re-run against a database that already has the set.
# =============================================================
# (repository root: handled by the harness preamble)
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
source tests/lib/fixtures.sh

echo "############ before ############"
BEFORE=$(fx_report | tr '\n' ' ')
echo "    $BEFORE"

echo "############ create ############"
if fx_create; then ok "fx_create completed"; else no "fx_create failed"; fi
echo "    partner=$FX_PARTNER product=$FX_PRODUCT sale=$FX_SALE line=$FX_SALE_LINE invoice=$FX_INVOICE"

echo "############ every piece exists ############"
[ -n "$FX_PARTNER" ]   && ok "a partner was created"        || no "no partner id"
[ -n "$FX_PRODUCT" ]   && ok "a product was created"        || no "no product id"
[ -n "$FX_SALE" ]      && ok "a sale order was created"     || no "no sale order id"
[ -n "$FX_SALE_LINE" ] && ok "a sale order line was created"|| no "no sale order line id"
[ -n "$FX_INVOICE" ]   && ok "an invoice was created"       || no "no invoice id"

echo "############ each is really in the database ############"
[ "$(fxq "SELECT count(*) FROM res_partner      WHERE id=$FX_PARTNER")" = "1" ] && ok "partner row present"      || no "partner row missing"
[ "$(fxq "SELECT count(*) FROM product_product  WHERE id=$FX_PRODUCT")" = "1" ] && ok "product row present"      || no "product row missing"
[ "$(fxq "SELECT count(*) FROM sale_order       WHERE id=$FX_SALE")"    = "1" ] && ok "sale order row present"   || no "sale order row missing"
[ "$(fxq "SELECT count(*) FROM sale_order_line  WHERE id=$FX_SALE_LINE")" = "1" ] && ok "sale line row present"  || no "sale line row missing"
[ "$(fxq "SELECT count(*) FROM account_move     WHERE id=$FX_INVOICE")" = "1" ] && ok "invoice row present"      || no "invoice row missing"

echo "############ the shapes the suite actually depends on ############"
# These are not decoration. Each mirrors a lookup one of the seven scripts makes,
# so if a fixture stops satisfying it, the failure lands here and says why.
[ -n "$(fxq "SELECT id FROM product_product ORDER BY id LIMIT 1")" ] \
  && ok "'first product' resolves"        || no "no product for 'ORDER BY id LIMIT 1'"
[ -n "$(fxq "SELECT id FROM sale_order_line ORDER BY id LIMIT 1")" ] \
  && ok "'first sale order line' resolves" || no "no sale order line"
[ -n "$(fxq "SELECT id FROM account_move WHERE amount_total > 0 ORDER BY id LIMIT 1")" ] \
  && ok "an invoice with a positive total exists" || no "no invoice with amount_total > 0"
[ -n "$(fxq "SELECT id FROM account_move WHERE move_type='out_invoice' AND state='draft' ORDER BY id LIMIT 1")" ] \
  && ok "a draft customer invoice exists" || no "no draft out_invoice"

echo "############ the line links the product and the order ############"
[ "$(fxq "SELECT product_id FROM sale_order_line WHERE id=$FX_SALE_LINE")" = "$FX_PRODUCT" ] \
  && ok "the sale line points at the fixture product" || no "sale line product_id is wrong"
[ "$(fxq "SELECT order_id FROM sale_order_line WHERE id=$FX_SALE_LINE")" = "$FX_SALE" ] \
  && ok "the sale line belongs to the fixture order"  || no "sale line order_id is wrong"
[ -n "$(fxq "SELECT id FROM account_move_line WHERE move_id=$FX_INVOICE LIMIT 1")" ] \
  && ok "the invoice has at least one line"           || no "the invoice has no lines"

echo "############ running it twice changes nothing ############"
# A suite re-run must not accumulate a second copy of the set.
fx_create >/dev/null
AGAIN=$(fx_report | tr '\n' ' ')
[ "$(fxq "SELECT count(*) FROM product_product WHERE default_code LIKE 'FX-%'")" = "1" ] \
  && ok "still exactly one fixture product" || no "a second product appeared: $AGAIN"
[ "$(fxq "SELECT count(*) FROM sale_order WHERE name LIKE 'FX-%'")" = "1" ] \
  && ok "still exactly one fixture order"   || no "a second order appeared: $AGAIN"
[ "$(fxq "SELECT count(*) FROM account_move WHERE name LIKE 'FX-%'")" = "1" ] \
  && ok "still exactly one fixture invoice" || no "a second invoice appeared: $AGAIN"

echo "############ after ############"
echo "    $(fx_report | tr '\n' ' ')"

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"
