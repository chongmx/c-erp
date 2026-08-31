#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 09 — THE CUSTOMER PORTAL.
#
#   staff sell to two customers -> staff grant portal access ->
#   a customer signs in and sees their own account, and ONLY their own
#
# The portal is a second front door. It has its own login route, its own
# cookie, its own session store, and it is scoped by PARTNER rather than by
# user — so none of the staff-side access control covers it. Twenty routes of
# it were reachable by anyone on the internet with nothing asserting who they
# answered to except one test, on one route (rental invoices), for one object
# type.
#
# So the load-bearing half of this journey is section 7: every route asked for
# the OTHER customer's records. An access check exercised only with the right
# customer proves nothing — it passes identically whether the WHERE clause
# scopes on partner_id or selects the whole table.
#
# It also closes the loop the two halves share: the staff-side toggle. Access
# granted through portal.partner must actually open the door, and revoking it
# must actually shut it.
#
# Everything is prefixed PJ- / 'PJ ' and removed on the way out, on failure too.
# =============================================================
auth_or_die

QTY=3
PRICE=400            # majors; the API scales to micro-units on the way in
EXPECT=$((QTY * PRICE))
M=1000000
PW='Welcome1'        # what portal_reset_password sets
PW2='Correct-Horse-9'

# The login route is rate-limited: 10 attempts per IP per 5 minutes, and a
# SUCCESS clears the counter. Every deliberate bad login below is therefore
# followed by a good one before the next batch. Add a bad-login check without
# that and the whole journey starts failing on 429 somewhere further down,
# which reads as a portal bug rather than a test that spent its budget.

cleanup() {
    pg "DELETE FROM payment_proof     WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'PJ %')" >/dev/null
    # Delete the lines by their MOVE, not by partner_id: an invoice's tax and
    # rounding lines carry a NULL partner_id, so a partner-scoped delete leaves
    # them behind and their FK then blocks the move from being removed at all.
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'PJ %'))" >/dev/null
    pg "DELETE FROM account_move      WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'PJ %')" >/dev/null
    # Validating a delivery posts a STOCK VALUATION entry to the STJ journal —
    # partner_id NULL, no customer anywhere on it. The only thread back to this
    # journey is the valuation layer, which records the product each posting
    # valued. This has to run while the PJ product still exists, so it comes
    # BEFORE the product delete below. (Same handle the stock-valuation-gl test
    # uses; without it a sale journey silently leaks its COGS entries.)
    SVL_MOVES=$(pg "SELECT string_agg(account_move_id::text,',') FROM stock_valuation_layer
                      WHERE account_move_id IS NOT NULL
                        AND product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'PJ-%')")
    [ -n "$SVL_MOVES" ] && pg "DELETE FROM account_move_line WHERE move_id IN ($SVL_MOVES)" >/dev/null
    pg "DELETE FROM stock_valuation_layer WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'PJ-%')" >/dev/null
    [ -n "$SVL_MOVES" ] && pg "DELETE FROM account_move WHERE id IN ($SVL_MOVES)" >/dev/null
    pg "DELETE FROM account_payment   WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'PJ %')" >/dev/null
    pg "DELETE FROM stock_move_line   WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'PJ-%')" >/dev/null
    pg "DELETE FROM stock_move        WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'PJ-%')" >/dev/null
    pg "DELETE FROM stock_picking     WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'PJ %')" >/dev/null
    pg "DELETE FROM stock_quant       WHERE product_id IN (SELECT id FROM product_product WHERE default_code LIKE 'PJ-%')" >/dev/null
    pg "DELETE FROM sale_order_line   WHERE order_id IN (SELECT id FROM sale_order WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'PJ %') OR origin='Portal Quote Request')" >/dev/null
    pg "DELETE FROM sale_order        WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'PJ %') OR origin='Portal Quote Request'" >/dev/null
    pg "DELETE FROM product_product   WHERE default_code LIKE 'PJ-%'" >/dev/null
    pg "DELETE FROM res_partner       WHERE name LIKE 'PJ %'" >/dev/null
    rm -f data/payment_proofs/*tmp_pj_proof.* tests/tmp_pj_* 2>/dev/null
}
cleanup
trap 'cleanup' EXIT

# jlen <json-array> — how many objects came back, without a JSON parser being
# optional. The portal answers with bare arrays, so `grep -c` on a one-line
# body counts nothing useful.
jlen() { python3 -c 'import sys,json;d=json.loads(sys.stdin.read() or "[]");print(len(d) if isinstance(d,list) else -1)' 2>/dev/null <<<"$1"; }
# jids <json-array> — the id column, one per line, for set comparisons.
jids() { python3 -c 'import sys,json;[print(r.get("id")) for r in json.loads(sys.stdin.read() or "[]")]' 2>/dev/null <<<"$1"; }
# jget <json-object> <key>
jget() { python3 -c 'import sys,json;print(json.loads(sys.stdin.read() or "{}").get(sys.argv[1],""))' "$2" 2>/dev/null <<<"$1"; }

# ------------------------------------------------------------------
sec "0. two customers and something to sell them"
# ------------------------------------------------------------------
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
# company_id explicit, never NULL: a companyless row means "shared by every
# company", and the multi-company isolation test would fail on this journey's
# debris instead of on anything it tests.
A=$(pgid "INSERT INTO res_partner (name, email, active, company_id)
          VALUES ('PJ Alpha Sdn Bhd','alpha@pj.test', true, 1) RETURNING id")
B=$(pgid "INSERT INTO res_partner (name, email, active, company_id)
          VALUES ('PJ Beta Sdn Bhd','beta@pj.test', true, 1) RETURNING id")
PRODUCT=$(pgid "INSERT INTO product_product
    (name, default_code, type, categ_id, uom_id, uom_po_id, list_price, standard_price,
     qty_available, active, sale_ok, purchase_ok, company_id)
    VALUES ('PJ Portal Widget','PJ-W1','product',$CAT,$UOM,$UOM,
            $((PRICE * 1000000)), 100000000, 0, true, true, true, 1) RETURNING id")
t_nonempty "$A" "customer A exists"
t_nonempty "$B" "customer B exists"
t_nonempty "$PRODUCT" "there is something to sell"
{ [ -z "$A" ] || [ -z "$B" ] || [ -z "$PRODUCT" ]; } && { verdict; exit 1; }

call stock.quant set_on_hand "[{\"product_id\":$PRODUCT,\"location_id\":4,\"quantity\":200}]" >/dev/null

# sell_to <partner> -> echoes "SO_ID PICK_ID INV_ID"
# The whole staff-side story in one function, run twice. Both customers get an
# identical set of documents on purpose: if the portal ever stopped scoping by
# partner, the lists would look plausible — right shape, right amounts, twice
# as many rows — and only a count would catch it.
sell_to() {
    local partner="$1" so pick inv name
    so=$(call sale.order create "[{\"partner_id\":$partner}]" | rid)
    [ -z "$so" ] && { echo ""; return; }
    call sale.order.line create \
        "[{\"order_id\":$so,\"product_id\":$PRODUCT,\"name\":\"PJ line\",\"product_uom_qty\":$QTY,\"price_unit\":$PRICE}]" >/dev/null
    call sale.order action_confirm "[[$so]]" >/dev/null
    name=$(pg "SELECT name FROM sale_order WHERE id=$so")
    # The picking and its moves are created inside the confirm, but the commit
    # can land just after the HTTP response — poll rather than sleep.
    for _ in 1 2 3 4 5 6; do
        pick=$(pg "SELECT id FROM stock_picking WHERE origin='$name' ORDER BY id LIMIT 1")
        [ -n "$pick" ] && break
        sleep 0.5
    done
    if [ -n "$pick" ]; then
        call stock.picking action_confirm "[[$pick]]" >/dev/null
        call stock.picking action_assign  "[[$pick]]" >/dev/null
        call stock.picking button_validate "[[$pick]]" >/dev/null
    fi
    call sale.order action_create_invoices "[[$so]]" >/dev/null
    inv=$(pg "SELECT id FROM account_move WHERE partner_id=$partner AND move_type='out_invoice' ORDER BY id DESC LIMIT 1")
    [ -n "$inv" ] && call account.move action_post "[[$inv]]" >/dev/null
    echo "$so $pick $inv"
}

read -r SO_A PICK_A INV_A <<<"$(sell_to "$A")"
read -r SO_B PICK_B INV_B <<<"$(sell_to "$B")"
t_nonempty "$SO_A" "A has an order"
t_nonempty "$PICK_A" "A has a delivery"
t_nonempty "$INV_A" "A has an invoice"
t_nonempty "$SO_B" "B has an order"
t_nonempty "$PICK_B" "B has a delivery"
t_nonempty "$INV_B" "B has an invoice"
{ [ -z "$INV_A" ] || [ -z "$INV_B" ] || [ -z "$PICK_B" ] || [ -z "$SO_B" ]; } && { verdict; exit 1; }
t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$INV_A")" "A's invoice is posted"
t_eq "done"   "$(pg "SELECT state FROM stock_picking WHERE id=$PICK_A")" "A's delivery went out"

# ------------------------------------------------------------------
sec "1. before access is granted, the door is shut"
# ------------------------------------------------------------------
# A partner row with an email is not a portal account. This is the check that
# says so — every customer in the database would otherwise be one bad default
# away from being able to log in.
t_eq "0" "$(pg "SELECT portal_active::int FROM res_partner WHERE id=$A")" "a new customer is not portal-enabled"
NOPE=$(portal_login 'alpha@pj.test' "$PW")
t_eq "" "$NOPE" "and cannot sign in"

# ------------------------------------------------------------------
sec "2. staff grant portal access"
# ------------------------------------------------------------------
GRANT=$(call portal.partner portal_reset_password "[[$A]]")
has_error "$GRANT" && no "granting portal access failed: $(echo "$GRANT" | head -c 200)"
t_eq "1" "$(pg "SELECT portal_active::int FROM res_partner WHERE id=$A")" "the partner is portal-enabled"
t_eq "1" "$(pg "SELECT (portal_password_hash IS NOT NULL)::int FROM res_partner WHERE id=$A")" "a password was set"
# The stored value must be a hash, not the password. Cheap to check, and the
# one mistake here is catastrophic and silent.
t_eq "0" "$(pg "SELECT (portal_password_hash = '$PW')::int FROM res_partner WHERE id=$A")" "it is stored hashed, not in clear"

call portal.partner portal_reset_password "[[$B]]" >/dev/null

# The staff-side list must reflect it, since that screen is where an operator
# checks whether a customer can get in.
LIST=$(call portal.partner search_read "[[[\"id\",\"=\",$A]]]")
has_error "$LIST" && no "the staff portal list errored: $(echo "$LIST" | head -c 200)"
t_contains "$LIST" '"portal_active":true' "the staff list shows access is on"
t_contains "$LIST" '"has_password":true'  "...and that a password exists"
t_lacks    "$LIST" 'portal_password_hash' "the hash itself is never sent to the browser"

# That list self-joins res_partner to resolve the company, so EVERY column is
# ambiguous unqualified — a filter on a bare name is the natural thing to do
# from the screen's own search box and used to 500.
BYNAME=$(call portal.partner search_read '[[["name","like","PJ Alpha"]]]')
has_error "$BYNAME" && no "filtering the staff list by name errored: $(echo "$BYNAME" | head -c 200)"
t_contains "$BYNAME" 'PJ Alpha Sdn Bhd' "the list can be filtered by name"
BYCO=$(call portal.partner search_read '[[["company_id","=",1],["email","like","alpha@pj"]]]')
has_error "$BYCO" && no "filtering by company and email errored: $(echo "$BYCO" | head -c 200)"
t_contains "$BYCO" 'alpha@pj.test' "...and by company and email together"

# S-49: the domain compiles against an allowlist of stored fields. Without one,
# an authenticated user names the hash column and reads it back a substring at
# a time through `like`, whatever the SELECT list is restricted to.
LEAK=$(call portal.partner search_read '[[["portal_password_hash","like","%"]]]')
if has_error "$LEAK"; then
    ok "a domain on portal_password_hash is refused"
else
    no "S-49: the hash column is filterable — $(echo "$LEAK" | head -c 120)"
fi

# ------------------------------------------------------------------
sec "3. the customer signs in"
# ------------------------------------------------------------------
BAD=$(portal_login 'alpha@pj.test' 'wrong-password')
t_eq "" "$BAD" "the wrong password is refused"
BADEMAIL=$(portal_login 'nobody@pj.test' "$PW")
t_eq "" "$BADEMAIL" "an unknown email is refused"

PA=$(portal_login 'alpha@pj.test' "$PW")
t_nonempty "$PA" "A signs in and gets a session cookie"
[ -z "$PA" ] && { verdict; exit 1; }
# Email match is case-insensitive — customers type their address how they like.
PUPPER=$(portal_login 'ALPHA@PJ.TEST' "$PW")
t_nonempty "$PUPPER" "the email match is case-insensitive"

PB=$(portal_login 'beta@pj.test' "$PW")
t_nonempty "$PB" "B signs in too"

# ------------------------------------------------------------------
sec "4. A sees A's documents"
# ------------------------------------------------------------------
ORDERS=$(portal_get "$PA" /portal/api/orders)
DELIVS=$(portal_get "$PA" /portal/api/deliveries)
INVS=$(portal_get   "$PA" /portal/api/invoices)
t_eq "1" "$(jlen "$ORDERS")" "one order in A's list"
t_eq "1" "$(jlen "$DELIVS")" "one delivery in A's list"
t_eq "1" "$(jlen "$INVS")"   "one invoice in A's list"
t_eq "$SO_A"   "$(jids "$ORDERS")" "it is A's order"
t_eq "$PICK_A" "$(jids "$DELIVS")" "it is A's delivery"
t_eq "$INV_A"  "$(jids "$INVS")"   "it is A's invoice"

# ------------------------------------------------------------------
sec "5. and B's are simply not there"
# ------------------------------------------------------------------
# Asserted by id, not by absence of a name: B's documents carry the same
# product and the same amounts, so a leak would be invisible to a text search.
jids "$ORDERS" | grep -qx "$SO_B"   && no "LEAK: B's order is in A's list"     || ok "B's order is not in A's list"
jids "$DELIVS" | grep -qx "$PICK_B" && no "LEAK: B's delivery is in A's list"  || ok "B's delivery is not in A's list"
jids "$INVS"   | grep -qx "$INV_B"  && no "LEAK: B's invoice is in A's list"   || ok "B's invoice is not in A's list"

# The mirror image, which is what catches a scope that is merely reversed.
BORDERS=$(portal_get "$PB" /portal/api/orders)
t_eq "$SO_B" "$(jids "$BORDERS")" "B sees B's order, and only that"

# ------------------------------------------------------------------
sec "6. opening their own: detail, print, PDF"
# ------------------------------------------------------------------
DET=$(portal_get "$PA" "/portal/api/order/$SO_A/detail")
t_eq "$SO_A" "$(jget "$DET" id)" "the order detail opens"
t_eq "$EXPECT" "$(jget "$DET" amount_total | cut -d. -f1)" "it shows what they were charged ($EXPECT)"
t_contains "$DET" 'PJ line' "the line they ordered is on it"

IDET=$(portal_get "$PA" "/portal/api/invoice/$INV_A/detail")
t_eq "$INV_A" "$(jget "$IDET" id)" "the invoice detail opens"
DDET=$(portal_get "$PA" "/portal/api/delivery/$PICK_A/detail")
t_eq "$PICK_A" "$(jget "$DDET" id)" "the delivery detail opens"
t_eq "Delivery Order" "$(jget "$DDET" document_title)" "an outgoing picking is titled a Delivery Order"

for kind in invoice order delivery; do
    case $kind in invoice) rec=$INV_A ;; order) rec=$SO_A ;; delivery) rec=$PICK_A ;; esac
    t_eq "200" "$(portal_code "$PA" "/portal/api/$kind/$rec/print")" "the $kind prints"
    PDF=$(portal_code "$PA" "/portal/api/$kind/$rec/pdf")
    case "$PDF" in
        200) ok "the $kind downloads as a PDF" ;;
        503) ok "the $kind PDF is unavailable (no wkhtmltopdf on this host) — reported, not crashed" ;;
        *)   no "the $kind PDF answered $PDF" ;;
    esac
done
# A PDF must actually be one. A 200 carrying an HTML error page is the failure
# mode worth catching here.
if [ "$(portal_code "$PA" "/portal/api/invoice/$INV_A/pdf")" = "200" ]; then
    HEAD=$(curl -s -H "Cookie: portal_sid=$PA" "$BASE/portal/api/invoice/$INV_A/pdf" | head -c 4)
    t_eq '%PDF' "$HEAD" "the bytes really are a PDF"
fi

# ------------------------------------------------------------------
sec "7. THE NEGATIVE CONTROL — A asks for B's records, on every route"
# ------------------------------------------------------------------
# Three object types x three routes. This is the section that would fail if
# any single WHERE clause lost its `AND partner_id = $2`, and the only one
# that can tell a scoped query from an unscoped one.
for kind in invoice order delivery; do
    case $kind in invoice) rec=$INV_B ;; order) rec=$SO_B ;; delivery) rec=$PICK_B ;; esac
    for route in detail print pdf; do
        CODE=$(portal_code "$PA" "/portal/api/$kind/$rec/$route")
        case "$CODE" in
            404|403) ok "A cannot open B's $kind via /$route ($CODE)" ;;
            200)     no "LEAK: A opened B's $kind via /$route" ;;
            *)       no "A's request for B's $kind via /$route answered $CODE" ;;
        esac
    done
done
# ...and the body of a refusal must not describe what it refused.
BODY=$(portal_get "$PA" "/portal/api/invoice/$INV_B/detail")
t_lacks "$BODY" 'PJ Beta' "the refusal does not name the other customer"
t_lacks "$BODY" 'amount_total' "nor leak any of their figures"

# A malformed id must be rejected, not fall through to an unscoped query.
t_eq "400" "$(portal_code "$PA" "/portal/api/invoice/notanumber/detail")" "a non-numeric id is rejected"

# ------------------------------------------------------------------
sec "8. an anonymous visitor gets nothing"
# ------------------------------------------------------------------
for path in orders deliveries invoices units products; do
    t_eq "401" "$(portal_code "" "/portal/api/$path")" "/$path needs a session"
done
t_eq "401" "$(portal_code "" "/portal/api/invoice/$INV_A/detail")" "so does an invoice detail"
t_eq "401" "$(portal_code "" "/portal/api/invoice/$INV_A/pdf")"    "so does an invoice PDF"
t_eq "401" "$(portal_post_code "" "/portal/api/change-password" '{"current_password":"x","new_password":"yyyyyyyy"}')" \
     "and so does changing a password"
# A made-up cookie is not a session. Without this, "no cookie" and "any cookie"
# are the same test.
t_eq "401" "$(portal_code "deadbeefdeadbeefdeadbeef" "/portal/api/invoices")" "a forged cookie is not a session"
# The staff session id is not a portal session id, in either direction.
t_eq "401" "$(portal_code "$SID" "/portal/api/invoices")" "a staff session does not open the portal"

# ------------------------------------------------------------------
sec "9. uploading a payment proof"
# ------------------------------------------------------------------
PNG=tests/tmp_pj_proof.png
printf '\211PNG\r\n\032\n' > "$PNG"; head -c 200 /dev/zero >> "$PNG"
UP=$(portal_upload "$PA" "/portal/api/invoice/$INV_A/proof" "$PNG")
t_contains "$UP" '"ok":true' "A attaches a receipt to their own invoice"
t_eq "1" "$(pg "SELECT count(*) FROM payment_proof WHERE invoice_id=$INV_A AND partner_id=$A")" "it is filed against that invoice"

# Against B's invoice: refused. An upload route that checks the session but not
# the invoice lets any customer write rows onto anyone's account.
UPB=$(portal_upload "$PA" "/portal/api/invoice/$INV_B/proof" "$PNG")
t_lacks "$UPB" '"ok":true' "A cannot attach anything to B's invoice"
t_eq "0" "$(pg "SELECT count(*) FROM payment_proof WHERE invoice_id=$INV_B")" "nothing was written against B's invoice"

# Extension allowlist (SEC-19) and filename sanitising (SEC-16).
EXE=tests/tmp_pj_proof.exe; cp "$PNG" "$EXE"
UPX=$(portal_upload "$PA" "/portal/api/invoice/$INV_A/proof" "$EXE")
t_lacks "$UPX" '"ok":true' "an .exe is refused"
t_eq "1" "$(pg "SELECT count(*) FROM payment_proof WHERE invoice_id=$INV_A")" "and was not filed"
rm -f "$EXE"

# The proof comes back on the invoice list, which is how the customer knows it
# arrived — an upload nobody can see is not a feature.
INVS2=$(portal_get "$PA" /portal/api/invoices)
t_contains "$INVS2" 'tmp_pj_proof.png' "the receipt shows on their invoice"
# ...and never on anyone else's.
t_lacks "$(portal_get "$PB" /portal/api/invoices)" 'tmp_pj_proof.png' "B does not see A's receipt"

# ------------------------------------------------------------------
sec "10. changing the password"
# ------------------------------------------------------------------
CP=$(portal_post "$PA" /portal/api/change-password '{"current_password":"nope","new_password":"'"$PW2"'"}')
t_lacks "$CP" '"ok":true' "the wrong current password is refused"
CP=$(portal_post "$PA" /portal/api/change-password '{"current_password":"'"$PW"'","new_password":"short"}')
t_lacks "$CP" '"ok":true' "a too-short new password is refused"
CP=$(portal_post "$PA" /portal/api/change-password '{"current_password":"'"$PW"'","new_password":"'"$PW2"'"}')
t_contains "$CP" '"ok":true' "the password changes"

# The half that is usually forgotten: the OLD password must stop working.
OLD=$(portal_login 'alpha@pj.test' "$PW")
t_eq "" "$OLD" "the old password no longer signs in"
NEW=$(portal_login 'alpha@pj.test' "$PW2")
t_nonempty "$NEW" "the new one does"

# ------------------------------------------------------------------
sec "11. revoking access shuts the door"
# ------------------------------------------------------------------
call_k portal.partner portal_set_active "[[$A]]" '"active":false' >/dev/null
t_eq "0" "$(pg "SELECT portal_active::int FROM res_partner WHERE id=$A")" "staff switch access off"
GONE=$(portal_login 'alpha@pj.test' "$PW2")
t_eq "" "$GONE" "the customer can no longer sign in"
# B is untouched — revoking one customer must not lock out the rest.
STILL=$(portal_login 'beta@pj.test' "$PW")
t_nonempty "$STILL" "the other customer is unaffected"

# ------------------------------------------------------------------
sec "12. signing out"
# ------------------------------------------------------------------
t_eq "200" "$(portal_code "$STILL" /portal/api/invoices)" "B's session is live before logging out"
portal_post "$STILL" /portal/api/logout '{}' >/dev/null
t_eq "401" "$(portal_code "$STILL" /portal/api/invoices)" "and dead after it"

# ------------------------------------------------------------------
sec "13. asking for a quote (docs/113 §3b)"
# ------------------------------------------------------------------
# THE RULE for every customer-facing write: the portal proposes, staff dispose.
# A quote request must produce a DRAFT order and nothing else — no confirmation,
# no stock movement, no ledger entry — because the portal's credential is a
# partner password, not a staff session.
PA2=$(portal_login 'alpha@pj.test' "$PW2")
if [ -z "$PA2" ]; then
    # A is revoked by §11; re-enable so this section has a live session.
    call_k portal.partner portal_set_active "[[$A]]" '"active":true' >/dev/null
    PA2=$(portal_login 'alpha@pj.test' "$PW2")
fi
t_nonempty "$PA2" "A signs in to request a quote"

QREQ="{\"lines\":[{\"product_id\":$PRODUCT,\"quantity\":7}],\"note\":\"Please quote for delivery in March.\"}"
Q=$(portal_post "$PA2" /portal/api/quote "$QREQ")
t_contains "$Q" '"ok":true'   "the quote request is accepted"
t_contains "$Q" '"state":"draft"' "and comes back as a draft"
QID=$(jget "$Q" order_id)
t_nonempty "$QID" "it created an order"

if [ -n "$QID" ]; then
    t_eq "draft" "$(pg "SELECT state FROM sale_order WHERE id=$QID")" "the order really is a draft"
    t_eq "$A" "$(pg "SELECT partner_id FROM sale_order WHERE id=$QID")" "it belongs to the customer who asked"
    t_eq "1" "$(pg "SELECT count(*) FROM sale_order_line WHERE order_id=$QID")" "the line was added"
    t_eq "7" "$(pg "SELECT product_uom_qty::int FROM sale_order_line WHERE order_id=$QID")" "for the quantity asked"
    # Nothing was reserved and nothing was posted. A draft that moved stock or
    # hit the ledger would be a customer writing to the books.
    t_eq "0" "$(pg "SELECT count(*) FROM stock_picking WHERE origin=(SELECT name FROM sale_order WHERE id=$QID)")" \
         "no delivery was created"
    t_eq "0" "$(pg "SELECT count(*) FROM account_move WHERE invoice_origin='Portal Quote Request'")" \
         "no invoice was posted"

    # The customer must not be able to name their own price: the line is priced
    # from the product, whatever the request said.
    QCHEAT=$(portal_post "$PA2" /portal/api/quote \
        "{\"lines\":[{\"product_id\":$PRODUCT,\"quantity\":1,\"price_unit\":1}]}")
    CID=$(jget "$QCHEAT" order_id)
    if [ -n "$CID" ]; then
        t_eq "$((PRICE * M))" "$(pg "SELECT price_unit FROM sale_order_line WHERE order_id=$CID")" \
             "the price comes from the product, not the request"
    else
        no "the second quote request failed"
    fi
fi

# The refusals.
t_eq "401" "$(portal_post_code "" /portal/api/quote "$QREQ")" "an anonymous visitor cannot request a quote"
EMPTY=$(portal_post "$PA2" /portal/api/quote '{"lines":[]}')
t_lacks "$EMPTY" '"ok":true' "an empty request is refused"
GHOST=$(portal_post "$PA2" /portal/api/quote '{"lines":[{"product_id":999999,"quantity":1}]}')
t_lacks "$GHOST" '"ok":true' "a request for a non-existent product is refused"
# ...and refusing must not leave a blank order behind for staff to puzzle over.
t_eq "0" "$(pg "SELECT count(*) FROM sale_order so WHERE so.partner_id=$A
                  AND so.origin='Portal Quote Request'
                  AND NOT EXISTS (SELECT 1 FROM sale_order_line l WHERE l.order_id=so.id)")" \
     "a refused request leaves no empty order"

# ------------------------------------------------------------------
sec "14. the invariant"
# ------------------------------------------------------------------
# Nothing the portal did changed the books. Every route it exposes is a read
# except the proof upload, which writes to its own table — so the ledger this
# journey created must still balance and must still total what was sold.
JBAL=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
             FROM account_move_line WHERE partner_id IN ($A,$B)")
t_eq "0" "${JBAL%%.*}" "the books balance across both customers"
SOLD=$(pg "SELECT COALESCE(SUM(amount_total),0) FROM account_move
            WHERE partner_id IN ($A,$B) AND move_type='out_invoice' AND state='posted'")
t_eq "$((EXPECT * 2 * M))" "${SOLD%%.*}" "the portal did not alter what was invoiced"

verdict
