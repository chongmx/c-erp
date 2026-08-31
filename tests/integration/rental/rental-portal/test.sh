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
# Rental invoices through the CUSTOMER PORTAL.
#
# The portal invoice feature already existed — list, detail, printable
# HTML, PDF download, payment-proof upload — and rental invoices are
# ordinary account_move rows, so they flow through it. What was missing
# was the ORIGIN link that a sale-generated invoice carries
# (invoice_origin + sale_id), so a rental invoice showed as an untitled
# "Sales Invoice" from nowhere.
#
# This asserts the integration AND the access control. The negative
# control matters most: an access check only ever exercised with the
# RIGHT customer proves nothing at all.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
M=1000000

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cleanup() {
    pg "DELETE FROM rental_invoice_link WHERE contract_line_id IN
          (SELECT id FROM rental_contract_line WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'PTEST%'))" >/dev/null
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'PTEST%'))" >/dev/null
    pg "DELETE FROM account_move WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'PTEST%')" >/dev/null
    pg "DELETE FROM rental_contract_line WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'PTEST%')" >/dev/null
    pg "DELETE FROM rental_event WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'PTEST%')" >/dev/null
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE 'PTEST%')" >/dev/null
    pg "DELETE FROM rental_unit WHERE code LIKE 'PT-%'" >/dev/null
    pg "DELETE FROM res_users WHERE login LIKE 'ptest_%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'PTEST%'" >/dev/null
}
cleanup

# The /rental/ routes authenticate (docs/061), and the billing run below
# happens BEFORE the portal-login section — so the admin cookie is
# captured here rather than there.
cat > /tmp/vrp_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c /tmp/vrp_cookie.txt -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/vrp_auth.json > /tmp/vrp_auth_out.json
grep -q '"session_id"' /tmp/vrp_auth_out.json || { echo "cannot authenticate as admin"; exit 1; }

echo "############ setup: two customers, each with a rented unit ############"
PA=$(pg "INSERT INTO res_partner (name,is_company,active,email)
         VALUES ('PTEST Alice',false,true,'ptest_alice@example.com') RETURNING id")
PB=$(pg "INSERT INTO res_partner (name,is_company,active,email)
         VALUES ('PTEST Bob',false,true,'ptest_bob@example.com') RETURNING id")
UA=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('PT-A1','Alice unit','available',1) RETURNING id")
UB=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('PT-B1','Bob unit','available',1) RETURNING id")
TAX=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' AND active ORDER BY id LIMIT 1")

# Alice is on a contract; Bob is a walk-in put on recurring billing. Both
# must work — the origin logic differs between them.
CON=$(pg "INSERT INTO rental_contract (name,partner_id,state,date_start,company_id)
          VALUES ('PTEST/RENT/0001',$PA,'active',CURRENT_DATE,1) RETURNING id")
pg "INSERT INTO rental_contract_line
      (contract_id,partner_id,unit_id,date_start,unit_price,tax_ids_json,state,
       billing_mode,billing_anchor_day,billing_months,billing_lead_days,
       next_period_start,company_id)
    VALUES ($CON,$PA,$UA,'2026-09-01',$((150*M)),'[$TAX]','active','recurring',1,1,7,'2026-09-01',1)" >/dev/null
pg "INSERT INTO rental_contract_line
      (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,
       billing_mode,billing_anchor_day,billing_months,billing_lead_days,
       next_period_start,company_id)
    VALUES ($PB,$UB,'2026-09-01',$((250*M)),'[$TAX]','active','recurring',1,1,7,'2026-09-01',1)" >/dev/null
echo "    Alice=$PA (contract $CON)   Bob=$PB (walk-in, no contract)"

curl -s -b /tmp/vrp_cookie.txt -X POST "$BASE/rental/billing/run?date=2026-08-26" > /dev/null
INV_A=$(pg "SELECT id FROM account_move WHERE partner_id=$PA AND move_type='out_invoice' ORDER BY id DESC LIMIT 1")
INV_B=$(pg "SELECT id FROM account_move WHERE partner_id=$PB AND move_type='out_invoice' ORDER BY id DESC LIMIT 1")
echo "    invoices: Alice=$INV_A  Bob=$INV_B"
[ -n "$INV_A" ] && [ -n "$INV_B" ] && ok "both invoices generated" || no "billing did not produce invoices"

echo
echo "############ 1. the invoice is tied to its rental origin ############"
ORG_A=$(pg "SELECT invoice_origin FROM account_move WHERE id=$INV_A")
ORG_B=$(pg "SELECT invoice_origin FROM account_move WHERE id=$INV_B")
RC_A=$(pg "SELECT COALESCE(rental_contract_id,0) FROM account_move WHERE id=$INV_A")
RC_B=$(pg "SELECT COALESCE(rental_contract_id,0) FROM account_move WHERE id=$INV_B")
echo "    Alice origin='$ORG_A' rental_contract_id=$RC_A"
echo "    Bob   origin='$ORG_B' rental_contract_id=$RC_B"
[ "$ORG_A" = "PTEST/RENT/0001" ] && ok "contract invoice carries the contract name as origin" \
                                 || no "origin is '$ORG_A'"
[ "$RC_A" = "$CON" ]             && ok "rental_contract_id set, mirroring sale_id" || no "rental_contract_id is $RC_A"
[ -n "$ORG_B" ]                  && ok "walk-in invoice still has an origin ('$ORG_B')" || no "walk-in origin empty"
[ "$RC_B" = "0" ]                && ok "walk-in has no contract id — correctly NULL"    || no "walk-in got contract $RC_B"

echo
echo "############ 2. what the invoice covers is recorded ############"
COV=$(pg "SELECT count(*) FROM rental_invoice_link WHERE move_id=$INV_A")
PSTART=$(pg "SELECT to_char(period_start,'YYYY-MM-DD') FROM rental_invoice_link WHERE move_id=$INV_A LIMIT 1")
echo "    coverage rows=$COV period_start=$PSTART"
[ "$COV" = "1" ]              && ok "one coverage row per unit billed" || no "$COV coverage rows"
[ "$PSTART" = "2026-09-01" ]  && ok "the period covered is recorded"   || no "period is $PSTART"

echo
echo "############ 3. portal login — a REAL session ############"
# Portal auth lives on res_partner (portal_password_hash + portal_active),
# not res_users. The hash is PBKDF2 with a random salt, so it cannot be
# forged in SQL — the admin action that sets a known password is used
# instead, which is also the flow a real operator follows.
cat > /tmp/pt_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/pt_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
for P in $PA $PB; do
    cat > /tmp/pt_rp.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"portal.partner",
 "method":"portal_reset_password","args":[[$P]],
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
         --data @/tmp/pt_rp.json > /dev/null
done
ACTIVE=$(pg "SELECT count(*) FROM res_partner WHERE id IN ($PA,$PB) AND portal_active")
echo "    portal-enabled partners: $ACTIVE"
[ "$ACTIVE" = "2" ] && ok "portal access provisioned for both customers" \
                    || no "only $ACTIVE provisioned"

# Unauthenticated first, so the authenticated results below cannot be
# mistaken for open access.
UNAUTH=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/api/invoices")
UNAUTH_PDF=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/api/invoice/$INV_A/pdf")
echo "    with no session: invoices -> $UNAUTH, pdf -> $UNAUTH_PDF"
[ "$UNAUTH" = "401" ]     && ok "portal refuses unauthenticated access"       || no "expected 401, got $UNAUTH"
[ "$UNAUTH_PDF" = "401" ] && ok "PDF download refuses unauthenticated access" || no "expected 401, got $UNAUTH_PDF"

# Log in as Alice.
cat > /tmp/pt_login.json <<EOF
{"email":"ptest_alice@example.com","password":"Welcome1"}
EOF
curl -s -c /tmp/pt_alice.cookie -X POST "$BASE/portal/api/login" \
     -H 'Content-Type: application/json' --data @/tmp/pt_login.json > /dev/null
cat > /tmp/pt_login_b.json <<EOF
{"email":"ptest_bob@example.com","password":"Welcome1"}
EOF
curl -s -c /tmp/pt_bob.cookie -X POST "$BASE/portal/api/login" \
     -H 'Content-Type: application/json' --data @/tmp/pt_login_b.json > /dev/null

LIST=$(curl -s -b /tmp/pt_alice.cookie "$BASE/portal/api/invoices")
echo "    Alice's invoice list: $(printf '%s' "$LIST" | head -c 220)"
printf '%s' "$LIST" | grep -q "\"id\":$INV_A" && ok "Alice sees her own invoice" \
                                              || no "invoice missing from the list"
printf '%s' "$LIST" | grep -q "PTEST/RENT/0001" && ok "the list shows the rental origin" \
                                                || no "origin absent from the list"
printf '%s' "$LIST" | grep -q "\"is_rental\":true" && ok "the list flags it as a rental invoice" \
                                                   || no "is_rental missing"
printf '%s' "$LIST" | grep -q "\"id\":$INV_B" && no "Alice can see BOB'S invoice — leak" \
                                              || ok "Bob's invoice is NOT in Alice's list"

DET=$(curl -s -b /tmp/pt_alice.cookie "$BASE/portal/api/invoice/$INV_A/detail")
echo "    detail: $(printf '%s' "$DET" | head -c 260)"
printf '%s' "$DET" | grep -q '"covers"'  && ok "detail says what the invoice covers" || no "no covers block"
printf '%s' "$DET" | grep -q 'PT-A1'     && ok "the covered unit is named"           || no "unit not named"
printf '%s' "$DET" | grep -q '2026-09-01'&& ok "the covered period is shown"         || no "period not shown"

echo
echo "############ 3b. the PDF actually downloads ############"
CODE=$(curl -s -b /tmp/pt_alice.cookie -o /tmp/pt_invoice.pdf -w '%{http_code}' \
       "$BASE/portal/api/invoice/$INV_A/pdf")
SIZE=$(stat -c%s /tmp/pt_invoice.pdf 2>/dev/null || echo 0)
MAGIC=$(head -c 4 /tmp/pt_invoice.pdf 2>/dev/null)
echo "    HTTP $CODE, $SIZE bytes, magic='$MAGIC'"
if [ "$CODE" = "200" ]; then
    ok "the customer can download their invoice"
    [ "$MAGIC" = "%PDF" ]  && ok "the file really is a PDF"  || no "magic bytes are '$MAGIC'"
    [ "$SIZE" -gt 1000 ]   && ok "the PDF has content ($SIZE bytes)" || no "only $SIZE bytes"
elif [ "$CODE" = "503" ]; then
    echo "    NOTE  503 means wkhtmltopdf is unavailable at runtime."
    no "PDF generation returned 503"
else
    no "PDF download returned $CODE"
fi

echo
echo "############ 4. the document renders as a RENTAL invoice ############"
# portalRenderDoc decides the title from whether rental_invoice_link rows
# exist, so a walk-in invoice is a rental invoice too.
ISR_A=$(pg "SELECT EXISTS (SELECT 1 FROM rental_invoice_link WHERE move_id=$INV_A)")
ISR_B=$(pg "SELECT EXISTS (SELECT 1 FROM rental_invoice_link WHERE move_id=$INV_B)")
[ "$ISR_A" = "t" ] && ok "Alice's invoice is detected as rental"          || no "not detected"
[ "$ISR_B" = "t" ] && ok "Bob's walk-in invoice is ALSO detected as rental" \
                   || no "walk-in not detected — title would say Sales Invoice"

# A genuinely non-rental invoice must NOT be mislabelled.
OTHER=$(pg "SELECT id FROM account_move WHERE move_type='out_invoice'
             AND NOT EXISTS (SELECT 1 FROM rental_invoice_link r WHERE r.move_id=account_move.id)
             ORDER BY id DESC LIMIT 1")
if [ -n "$OTHER" ]; then
    ISR_O=$(pg "SELECT EXISTS (SELECT 1 FROM rental_invoice_link WHERE move_id=$OTHER)")
    [ "$ISR_O" = "f" ] && ok "a non-rental invoice is not mislabelled as rental" \
                       || no "non-rental invoice detected as rental"
fi

echo
echo "############ 5. the invoice line names the unit and the period ############"
LBL=$(PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc \
      "SELECT name FROM account_move_line
        WHERE move_id=$INV_A AND credit>0 AND tax_line_id IS NULL AND display_type=''
        LIMIT 1" 2>/dev/null)
echo "    line label: $LBL"
printf '%s' "$LBL" | grep -q "PT-A1"      && ok "the unit code is on the line" || no "no unit code"
printf '%s' "$LBL" | grep -q "2026-09-01" && ok "the period is on the line"    || no "no period"

echo
echo "############ 6. NEGATIVE CONTROL — cross-customer access is refused ############"
# portalRenderDoc scopes with `WHERE am.id=$1 AND am.partner_id=$2`, so
# the wrong partner yields no row and the route returns 404. Proved by
# running the scoped query as the WRONG customer.
# Driven through the REAL portal session, not the SQL predicate. An
# access check exercised only with the right customer proves nothing —
# this logs in as Bob and asks for Alice's invoice.
BOB_DETAIL=$(curl -s -o /dev/null -w '%{http_code}' -b /tmp/pt_bob.cookie \
             "$BASE/portal/api/invoice/$INV_A/detail")
BOB_PDF=$(curl -s -o /dev/null -w '%{http_code}' -b /tmp/pt_bob.cookie \
          "$BASE/portal/api/invoice/$INV_A/pdf")
BOB_PRINT=$(curl -s -o /dev/null -w '%{http_code}' -b /tmp/pt_bob.cookie \
            "$BASE/portal/api/invoice/$INV_A/print")
echo "    Bob requesting Alice's invoice: detail=$BOB_DETAIL pdf=$BOB_PDF print=$BOB_PRINT"
[ "$BOB_DETAIL" = "404" ] && ok "detail refused (404)"       || no "detail returned $BOB_DETAIL"
[ "$BOB_PDF"    = "404" ] && ok "PDF download refused (404)" || no "PDF returned $BOB_PDF"
[ "$BOB_PRINT"  = "404" ] && ok "printable HTML refused (404)" || no "print returned $BOB_PRINT"

# And Bob's own invoice must still work, so the refusals above are scoping
# rather than everything being broken.
BOB_OWN=$(curl -s -o /dev/null -w '%{http_code}' -b /tmp/pt_bob.cookie \
          "$BASE/portal/api/invoice/$INV_B/pdf")
echo "    Bob requesting his OWN invoice: pdf=$BOB_OWN"
[ "$BOB_OWN" = "200" ] && ok "Bob can still download his own — scoping, not breakage" \
                       || no "Bob's own download returned $BOB_OWN"

echo
echo "############ 6b. My Units ############"
# docs/046 §7: a card per unit plus one balance figure. Same scoping
# discipline as invoices, and asserted the same way — with the wrong
# customer as well as the right one.
UNAUTH_U=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/api/units")
[ "$UNAUTH_U" = "401" ] && ok "units endpoint refuses unauthenticated access" \
                        || no "expected 401, got $UNAUTH_U"

AU=$(curl -s -b /tmp/pt_alice.cookie "$BASE/portal/api/units")
echo "    Alice's units: $(printf '%s' "$AU" | head -c 300)"
printf '%s' "$AU" | grep -q '"PT-A1"' && ok "Alice sees her own unit" || no "unit missing"
printf '%s' "$AU" | grep -q '"PT-B1"' && no "Alice sees BOB'S unit — leak" \
                                      || ok "Bob's unit is NOT in Alice's list"

# The rate must arrive in MAJOR units, like every other money value.
printf '%s' "$AU" | grep -q '"net_rate":150.0' && ok "rate reported as 150.0, not micro-units" \
                                               || no "rate conversion wrong"
# Balance comes from the same invoices the Invoices tab shows, so the two
# pages cannot tell the customer different things.
BAL=$(printf '%s' "$AU" | python3 -c "import json,sys; print(json.load(sys.stdin)['summary']['balance_due'])")
INV_TOT=$(pg "SELECT COALESCE(SUM(amount_residual),0)/1000000.0 FROM account_move
               WHERE partner_id=$PA AND move_type='out_invoice'
                 AND state='posted' AND amount_residual>0")
echo "    balance_due=$BAL   invoices residual=$INV_TOT"
python3 -c "
import sys
sys.exit(0 if abs(float('$BAL') - float('$INV_TOT' or 0)) < 0.01 else 1)" \
    && ok "the balance matches her open invoices exactly" \
    || no "balance $BAL vs invoices $INV_TOT — the two pages disagree"

CNT=$(printf '%s' "$AU" | python3 -c "import json,sys; print(json.load(sys.stdin)['summary']['count'])")
[ "$CNT" = "1" ] && ok "unit count is her own only" || no "count is $CNT"

# Bob's view must show HIS unit, so the exclusion above is scoping rather
# than the endpoint being empty for everyone.
BU=$(curl -s -b /tmp/pt_bob.cookie "$BASE/portal/api/units")
printf '%s' "$BU" | grep -q '"PT-B1"' && ok "Bob sees his own unit — scoping, not breakage" \
                                      || no "Bob's unit missing from his own list"

echo
echo "############ 6c. the portal page is wired up ############"
PJS=$(curl -s "$BASE/portal.js")
PHTML=$(curl -s "$BASE/portal.html")
printf '%s' "$PHTML" | grep -q 'data-section="units"' && ok "My Units nav item present" || no "no nav item"
printf '%s' "$PHTML" | grep -q 'id="section-units"'   && ok "section markup present"    || no "no section"
printf '%s' "$PJS"   | grep -q "loadUnits"            && ok "loader wired"              || no "no loader"
printf '%s' "$PJS"   | grep -q "showSection('units')" && ok "portal lands on My Units"  || no "does not land on units"
# A walk-in is billed by hand, so the page must not promise a next date.
printf '%s' "$PJS"   | grep -q 'u.recurring && u.next_period' \
    && ok "next period shown only for auto-billed units" \
    || no "a manually billed unit would show a next date it cannot keep"

echo
echo "############ 7. PDF generation is available ############"
WK=$(command -v wkhtmltopdf 2>/dev/null)
TPL=$(pg "SELECT count(*) FROM ir_report_template WHERE model='account.move' AND active")
echo "    wkhtmltopdf: ${WK:-not installed}   account.move templates: $TPL"
[ "$TPL" -ge 1 ] && ok "an invoice template exists for the PDF route" \
                 || no "no account.move report template — the PDF route would 404"
if [ -z "$WK" ]; then
    echo "    NOTE  wkhtmltopdf is absent here, so the PDF route returns 503 by design."
    echo "          The HTML render path above is what this suite verifies."
fi

echo
echo "############ cleanup ############"
cleanup
LEFT=$(pg "SELECT count(*) FROM res_partner WHERE name LIKE 'PTEST%'")
[ "$LEFT" = "0" ] && ok "test data removed" || no "$LEFT partners leaked"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
