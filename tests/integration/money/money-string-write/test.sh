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
# verify_money_string_write.sh
#
# Regression for the factor-of-a-million bug: scaled/monetary fields entered in
# the UI arrive as STRINGS (HTML <input type=number> -> e.target.value is a
# string), and the write boundary must still scale them to BIGINT micro-units.
# Before the fix, keying 330 into a rate stored "330" raw -> read back 0.00033.
#
# Writes STRING values (exactly what the browser sends) and asserts raw DB
# micros = human*1e6 AND read-back = human, for scaled fields across modules,
# on create() and write(). A non-scaled Float rate is the negative control.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
pg(){ PGPASSWORD=odoo psql -w -h localhost -U odoo -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok(){ echo "    PASS  $1"; }
no(){ echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/msw_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/msw_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -n "$SID" ] || { echo "    cannot authenticate"; echo '*** FAILURES ***'; exit 1; }
call(){ cat > /tmp/msw_call.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,"kwargs":{"context":{"session_id":"$SID"}}}}
EOF
  curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/msw_call.json; }
# rd <model> <id> <col> -> read-back numeric value
rd(){ call "$1" read "[[$2],[\"$3\"]]" | sed -n "s/.*\"$3\":\([0-9.]*\).*/\1/p"; }

echo "############ scaled fields written as STRINGS (the UI's real payload) ############"

# ── rental.unit.type.default_rate (the reported bug) ──
pg "DELETE FROM rental_unit_type WHERE code='MSW1'" >/dev/null
call rental.unit.type create '[{"name":"MSW","code":"MSW1","default_rate":"330"}]' >/dev/null
RID=$(pg "SELECT id FROM rental_unit_type WHERE code='MSW1'")
RAW=$(pg "SELECT default_rate FROM rental_unit_type WHERE code='MSW1'")
[ "$RAW" = "330000000" ] && ok "rental default_rate CREATE: STRING \"330\" -> 330000000 micros" || no "default_rate DB=$RAW expected 330000000"
[ "$(rd rental.unit.type "$RID" default_rate)" = "330.0" ] && ok "rental default_rate reads back 330" || no "default_rate read-back wrong: $(rd rental.unit.type "$RID" default_rate)"
call rental.unit.type write "[[$RID],{\"default_rate\":\"12.50\"}]" >/dev/null
RAW=$(pg "SELECT default_rate FROM rental_unit_type WHERE code='MSW1'")
[ "$RAW" = "12500000" ] && ok "rental default_rate WRITE: STRING \"12.50\" -> 12500000 micros" || no "write default_rate DB=$RAW expected 12500000"
pg "DELETE FROM rental_unit_type WHERE code='MSW1'" >/dev/null

# ── product.product list_price + standard_price ──
pg "DELETE FROM product_product WHERE default_code='MSWP'" >/dev/null
call product.product create '[{"name":"MSW Product","default_code":"MSWP","list_price":"49.99","standard_price":"20"}]' >/dev/null
LP=$(pg "SELECT list_price FROM product_product WHERE default_code='MSWP'")
SP=$(pg "SELECT standard_price FROM product_product WHERE default_code='MSWP'")
[ "$LP" = "49990000" ] && ok "product list_price CREATE: STRING \"49.99\" -> 49990000 micros" || no "list_price DB=$LP expected 49990000"
[ "$SP" = "20000000" ] && ok "product standard_price CREATE: STRING \"20\" -> 20000000 micros" || no "standard_price DB=$SP expected 20000000"
pg "DELETE FROM product_product WHERE default_code='MSWP'" >/dev/null

echo "############ negative control: non-scaled Float rate must NOT be scaled ############"
pg "DELETE FROM account_tax WHERE name='MSW Tax 15'" >/dev/null
call account.tax create '[{"name":"MSW Tax 15","amount":"15","amount_type":"percent"}]' >/dev/null
TAMT=$(pg "SELECT amount FROM account_tax WHERE name='MSW Tax 15'")
# must be ~15 (unscaled), NOT 15000000 — use a numeric comparison, any dp format
if awk "BEGIN{exit !(($TAMT+0) > 14.9 && ($TAMT+0) < 15.1)}" 2>/dev/null; then
    ok "account.tax.amount (Float rate): STRING \"15\" stays $TAMT (NOT scaled)"
else
    no "tax amount scaled wrongly: $TAMT (expected ~15, not 15000000)"
fi
pg "DELETE FROM account_tax WHERE name='MSW Tax 15'" >/dev/null

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi
