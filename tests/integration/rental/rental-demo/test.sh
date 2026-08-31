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
# Demo data tooling, and the auth on every /rental/* route.
#
# Two things under test:
#
#   1. AUTH. Every rental route mutates or discloses business data, so
#      every one must refuse an anonymous caller. The first cut of these
#      routes had none — /rental/billing/run would create invoices, and
#      /rental/dashboard would disclose MRR and receivables, to anyone
#      who could reach the port. Loopback binding and nginx decide who
#      can knock, not who gets in.
#
#   2. The demo seed/clear pair. clear() is destructive, so what it owns
#      is asserted precisely: it must remove the demo set and NOTHING
#      else, and a control row placed just outside that set must survive.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/vrd_cookie.txt
rm -f "$CK"

echo "############ 1. every /rental/ route refuses an anonymous caller ############"
# GETs disclose; POSTs mutate. Both must be 401.
for p in /rental/dashboard /rental/cashflow /rental/demo/status; do
    c=$(curl -s -o /dev/null -w '%{http_code}' "$BASE$p")
    printf '    GET  %-28s -> %s\n' "$p" "$c"
    [ "$c" = "401" ] && ok "refused" || no "$p returned $c, expected 401"
done
for p in /rental/billing/run /rental/expenses/generate /rental/demo/seed /rental/demo/clear; do
    c=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE$p")
    printf '    POST %-28s -> %s\n' "$p" "$c"
    [ "$c" = "401" ] && ok "refused" || no "$p returned $c, expected 401"
done

# A rejected billing run must not have created anything on its way out.
BEFORE_MV=$(pg "SELECT count(*) FROM account_move")
curl -s -o /dev/null -X POST "$BASE/rental/billing/run"
AFTER_MV=$(pg "SELECT count(*) FROM account_move")
[ "$BEFORE_MV" = "$AFTER_MV" ] && ok "the refused billing run created no invoices" \
                               || no "invoices went from $BEFORE_MV to $AFTER_MV while unauthenticated"

echo
echo "############ 2. authenticate ############"
cat > /tmp/vrd_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/vrd_auth.json > /tmp/vrd_auth_out.json
grep -q '"session_id"' /tmp/vrd_auth_out.json && ok "signed in as admin" || { no "cannot authenticate"; exit 1; }

AUTHED=$(curl -s -o /dev/null -w '%{http_code}' -b "$CK" "$BASE/rental/demo/status")
[ "$AUTHED" = "200" ] && ok "the same route succeeds WITH a session — auth, not breakage" \
                      || no "authenticated request returned $AUTHED"

# Remember the starting state so it can be restored at the end.
WAS_PRESENT=$(curl -s -b "$CK" "$BASE/rental/demo/status" \
              | python3 -c "import json,sys; print(str(json.load(sys.stdin)['present']).lower())")
echo "    demo facility present on entry: $WAS_PRESENT"

echo
echo "############ 3. a control row that must survive everything ############"
# Placed deliberately just outside the demo set: same shape, different
# site. If clear() ever widens, this is what notices.
# Both control rows are cleared first. An earlier run that failed part-way
# left its expense behind, so a second was created and the "exactly 1
# survivor" assertion failed against 2 — reporting a data-destruction bug
# that had not happened.
pg "DELETE FROM rental_unit WHERE code='KEEPME-1'" >/dev/null
pg "DELETE FROM rental_expense WHERE name='KEEPME utilities'" >/dev/null
KEEP=$(pg "INSERT INTO rental_unit (code,name,site,state,company_id)
           VALUES ('KEEPME-1','not demo data','Real Warehouse','available',1) RETURNING id")
KEEPEXP=$(pg "INSERT INTO rental_expense (date,name,amount,is_recurring,
                                          recurrence_interval,recurrence_next_date,
                                          company_id,state)
              VALUES (CURRENT_DATE,'KEEPME utilities',9900000,TRUE,'monthly',
                      CURRENT_DATE,1,'draft') RETURNING id")
echo "    control unit=$KEEP  control expense=$KEEPEXP"

echo
echo "############ 4. seed ############"
curl -s -b "$CK" -X POST "$BASE/rental/demo/clear" > /dev/null   # start clean
R=$(curl -s -b "$CK" -X POST "$BASE/rental/demo/seed")
U=$(printf '%s' "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['units'])")
T=$(printf '%s' "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['tenancies'])")
E=$(printf '%s' "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['expense_templates'])")
echo "    units=$U tenancies=$T expense budgets=$E"
[ "$U" = "45" ] && ok "45 units across five zones" || no "$U units"
[ "$T" = "22" ] && ok "22 tenancies (18 active + 4 pending)" || no "$T tenancies"
[ "$E" = "7" ]  && ok "7 recurring expense budgets"  || no "$E budgets"

# States are DERIVED by the trigger from the tenancies, never set here.
OCC=$(pg "SELECT count(*) FROM rental_unit WHERE site='Demo Warehouse' AND state='occupied'")
RES=$(pg "SELECT count(*) FROM rental_unit WHERE site='Demo Warehouse' AND state='reserved'")
MNT=$(pg "SELECT count(*) FROM rental_unit WHERE site='Demo Warehouse' AND state='maintenance'")
echo "    derived states: occupied=$OCC reserved=$RES maintenance=$MNT"
[ "$OCC" = "18" ] && ok "occupied derived from the active tenancies" || no "$OCC occupied"
[ "$RES" = "4" ]  && ok "reserved derived from the pending tenancies" || no "$RES reserved"
[ "$MNT" = "2" ]  && ok "maintenance set as an operator fact"         || no "$MNT in maintenance"

# Every code must have matched a real unit. The shell version once used
# `seq -w`, which pads to the width of the LARGEST value — so zone C got
# C1 while the let-list said C01, and 6 tenancies silently vanished.
[ "$T" = "22" ] && ok "every unit code in the let-list matched a real unit" \
                || no "only $T of 22 tenancies created — a code did not match"

echo
echo "############ 5. seeding twice adds nothing ############"
R2=$(curl -s -b "$CK" -X POST "$BASE/rental/demo/seed")
C2=$(printf '%s' "$R2" | python3 -c "
import json,sys; c=json.load(sys.stdin)['created']; print(sum(c.values()))")
U2=$(printf '%s' "$R2" | python3 -c "import json,sys; print(json.load(sys.stdin)['units'])")
echo "    second seed created $C2 row(s); units still $U2"
[ "$C2" = "0" ] && ok "idempotent — a re-run creates nothing" || no "created $C2 rows on the second run"
[ "$U2" = "45" ] && ok "no duplicate units"                   || no "$U2 units after re-seeding"

echo
echo "############ 6. clear removes the demo set ############"
R3=$(curl -s -b "$CK" -X POST "$BASE/rental/demo/clear")
echo "    $(printf '%s' "$R3" | head -c 200)"
LEFT_U=$(pg "SELECT count(*) FROM rental_unit WHERE site='Demo Warehouse'")
LEFT_C=$(pg "SELECT count(*) FROM rental_contract WHERE name LIKE 'DEMO/%'")
LEFT_E=$(pg "SELECT count(*) FROM rental_expense WHERE name='Wifi / broadband'")
[ "$LEFT_U" = "0" ] && ok "demo units gone"     || no "$LEFT_U units left"
[ "$LEFT_C" = "0" ] && ok "demo contracts gone" || no "$LEFT_C contracts left"
[ "$LEFT_E" = "0" ] && ok "demo expense budgets gone" || no "$LEFT_E budgets left"

echo
echo "############ 7. THE control row survived ############"
# The assertion that matters. A clear() that widened its scope would take
# this with it, and nothing else in the suite would notice.
SURV_U=$(pg "SELECT count(*) FROM rental_unit WHERE code='KEEPME-1'")
SURV_E=$(pg "SELECT count(*) FROM rental_expense WHERE name='KEEPME utilities'")
echo "    control unit present=$SURV_U  control expense present=$SURV_E"
[ "$SURV_U" = "1" ] && ok "the non-demo unit was NOT deleted" \
                    || no "clear() destroyed data outside the demo set"
[ "$SURV_E" = "1" ] && ok "the non-demo expense was NOT deleted" \
                    || no "clear() destroyed an expense outside the demo set"

echo
echo "############ 8. invoices are kept on purpose ############"
# Posted accounting documents carry ir.sequence numbers. Deleting them
# would leave a gap in the invoice series, which is exactly what an
# auditor asks about.
curl -s -b "$CK" -X POST "$BASE/rental/demo/seed" > /dev/null
curl -s -b "$CK" -X POST "$BASE/rental/billing/run?date=$(date -d '+40 days' +%F 2>/dev/null || date +%F)" > /dev/null
INV_BEFORE=$(pg "SELECT count(*) FROM account_move WHERE move_type='out_invoice'")
curl -s -b "$CK" -X POST "$BASE/rental/demo/clear" > /dev/null
INV_AFTER=$(pg "SELECT count(*) FROM account_move WHERE move_type='out_invoice'")
DANGLING=$(pg "SELECT count(*) FROM rental_invoice_link ril
                WHERE NOT EXISTS (SELECT 1 FROM rental_contract_line l
                                   WHERE l.id = ril.contract_line_id)")
echo "    invoices $INV_BEFORE -> $INV_AFTER   dangling links=$DANGLING"
[ "$INV_BEFORE" = "$INV_AFTER" ] && ok "invoices kept — no gap punched in the series" \
                                 || no "invoices went $INV_BEFORE -> $INV_AFTER"
[ "$DANGLING" = "0" ] && ok "no dangling invoice links left behind" || no "$DANGLING dangling links"

echo
echo "############ 9. the panel is registered and served ############"
c=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/src/components/rental/RentalDemoData.js")
[ "$c" = "200" ] && ok "RentalDemoData.js served" || no "returned $c"
HTML=$(curl -s "$BASE/index.html")
D=$(printf '%s' "$HTML" | grep -n 'RentalDemoData.js' | head -1 | cut -d: -f1)
A=$(printf '%s' "$HTML" | grep -n 'src/app.js' | head -1 | cut -d: -f1)
[ -n "$D" ] && [ "$D" -lt "$A" ] && ok "loaded before app.js references it" \
                                 || no "load order wrong"
curl -s "$BASE/src/app.js" | grep -q "'rental.demo.data'" \
    && ok "registered in CUSTOM_VIEWS" || no "not registered"
MEN=$(pg "SELECT count(*) FROM ir_ui_menu WHERE name='Demo Data' AND parent_id=101")
[ "$MEN" = "1" ] && ok "menu entry under Settings -> Technical" || no "menu entry missing"
# The destructive action must not be a reflex click.
JS=$(curl -s "$BASE/src/components/rental/RentalDemoData.js")
printf '%s' "$JS" | grep -q "confirm !== 'REMOVE'" \
    && ok "removal requires typing REMOVE, not just a click" \
    || no "no type-to-confirm on the destructive action"

echo
echo "############ cleanup ############"
pg "DELETE FROM rental_unit WHERE code='KEEPME-1'" >/dev/null
pg "DELETE FROM rental_expense WHERE name='KEEPME utilities'" >/dev/null

# Leave the facility as this suite found it. Other suites — the grid, the
# dashboard — read the demo data as ambient context, so a run that ends
# with it cleared makes THEM fail, on a machine where nothing is wrong.
# A test that changes the world for its neighbours is not isolated just
# because it cleaned up after itself.
if [ "$WAS_PRESENT" = "true" ]; then
    curl -s -b "$CK" -X POST "$BASE/rental/demo/seed" > /dev/null
    RESTORED=$(pg "SELECT count(*) FROM rental_unit WHERE site='Demo Warehouse'")
    echo "    demo facility restored ($RESTORED units) — it was present on entry"
    [ "$RESTORED" = "45" ] && ok "restored to the state found on entry" || no "only $RESTORED units restored"
else
    curl -s -b "$CK" -X POST "$BASE/rental/demo/clear" > /dev/null
    echo "    demo facility left absent — it was absent on entry"
fi

rm -f "$CK" /tmp/vrd_auth.json /tmp/vrd_auth_out.json
echo "    control rows removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
