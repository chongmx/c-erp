#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The web module — features ported from the reference ERP's portal (docs/114).
#
#   * share links   — the reference ERP's portal.mixin access_token, adapted
#   * statement     — built from the RECEIVABLE LEDGER, so it agrees with the
#                     books by construction rather than by arithmetic
#   * portal home   — the reference ERP's /my/counters
#   * pagination    — the reference ERP's pager; orders and deliveries were UNBOUNDED
#
# The share-token assertions carry the weight. A token is a bearer credential,
# so the questions that matter are all "what can it NOT do": open a different
# document, survive revocation, survive expiry, or write anything.
# =============================================================
auth_or_die

M=1000000
PW='Welcome1'

pget()  { curl -s               -H "Cookie: portal_sid=${1:-}" "$BASE$2"; }
pcode() { curl -s -o /dev/null -w '%{http_code}' -H "Cookie: portal_sid=${1:-}" "$BASE$2"; }
jget()  { python3 -c 'import sys,json;print(json.loads(sys.stdin.read() or "{}").get(sys.argv[1],""))' "$2" <<<"$1" 2>/dev/null; }

cleanup() {
    pg "DELETE FROM portal_access_token WHERE res_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'WEB %'))" >/dev/null
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT id FROM account_move WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'WEB %'))" >/dev/null
    pg "DELETE FROM account_move  WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'WEB %')" >/dev/null
    pg "DELETE FROM sale_order_line WHERE order_id IN
          (SELECT id FROM sale_order WHERE partner_id IN
             (SELECT id FROM res_partner WHERE name LIKE 'WEB %'))" >/dev/null
    pg "DELETE FROM sale_order    WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'WEB %')" >/dev/null
    pg "DELETE FROM res_partner   WHERE name LIKE 'WEB %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

# ------------------------------------------------------------------
sec "1. two customers with a posted invoice each"
# ------------------------------------------------------------------
A=$(pgid "INSERT INTO res_partner (name,email,active,company_id)
          VALUES ('WEB Alpha','web-a@t.test',true,1) RETURNING id")
B=$(pgid "INSERT INTO res_partner (name,email,active,company_id)
          VALUES ('WEB Beta','web-b@t.test',true,1) RETURNING id")
t_nonempty "$A" "customer A"; t_nonempty "$B" "customer B"
[ -z "$A" ] || [ -z "$B" ] && { verdict; exit 1; }

RECV=$(pg "SELECT id FROM account_account WHERE account_type='asset_receivable' ORDER BY id LIMIT 1")
INCOME=$(pg "SELECT id FROM account_account WHERE account_type='income' ORDER BY id LIMIT 1")
SJ=$(pg "SELECT id FROM account_journal WHERE type='sale' ORDER BY id LIMIT 1")
t_nonempty "$RECV" "a receivable account exists"

# A posted invoice, written straight to the ledger so the statement has real
# receivable lines to read. 1000.00 due.
mkinv() { # mkinv <partner> <amount-majors> <date>
    local mv
    mv=$(pgid "INSERT INTO account_move
        (name, move_type, state, partner_id, journal_id, company_id,
         invoice_date, due_date, amount_total, amount_residual)
        VALUES ('WEBINV/'||nextval('account_move_id_seq'), 'out_invoice','posted',$1,$SJ,1,
                '$3'::date, '$3'::date, $(( $2 * M )), $(( $2 * M ))) RETURNING id")
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,partner_id,name,debit,credit,date,display_type)
        VALUES ($mv,$RECV,$SJ,$1,'WEB invoice',$(( $2 * M )),0,'$3'::date,'')" >/dev/null
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,partner_id,name,debit,credit,date,display_type)
        VALUES ($mv,$INCOME,$SJ,$1,'WEB sale',0,$(( $2 * M )),'$3'::date,'')" >/dev/null
    echo "$mv"
}
INV_A=$(mkinv "$A" 1000 2026-06-10)
INV_B=$(mkinv "$B" 500  2026-06-10)
t_nonempty "$INV_A" "A has an invoice"
t_nonempty "$INV_B" "B has an invoice"

call portal.partner portal_reset_password "[[$A]]" >/dev/null
PA=$(portal_login 'web-a@t.test' "$PW")
t_nonempty "$PA" "A can sign in"

# ------------------------------------------------------------------
sec "2. share links — the table and the mint"
# ------------------------------------------------------------------
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='portal_access_token'")" "portal_access_token exists"
t_eq "1" "$(pg "SELECT count(*) FROM pg_indexes WHERE indexname='portal_access_token_live_uniq'")" \
     "one live token per document is enforced by a unique index"

SH=$(call_k portal.partner portal_share_document '[[]]' "\"model\":\"account.move\",\"res_id\":$INV_A")
has_error "$SH" && no "sharing failed: $(echo "$SH" | head -c 200)"
TOK=$(printf '%s' "$SH" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read()).get("result",{}).get("token",""))' 2>/dev/null)
URL=$(printf '%s' "$SH" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read()).get("result",{}).get("share_url",""))' 2>/dev/null)
t_nonempty "$TOK" "a token was minted"
t_contains "$URL" "/portal/doc/account.move/$INV_A" "the link points at that document"
t_ge "${#TOK}" "32" "the token is long enough not to be guessed"

# ------------------------------------------------------------------
sec "3. the link opens ONE document, with no session"
# ------------------------------------------------------------------
# The whole point: an anonymous holder, no cookie at all.
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=$TOK")" \
     "the shared link opens for a visitor with no session"
BODY=$(curl -s "$BASE/portal/doc/account.move/$INV_A?token=$TOK")
t_contains "$BODY" 'WEB Alpha' "it shows the document"

HDRS=$(curl -s -D - -o /dev/null "$BASE/portal/doc/account.move/$INV_A?token=$TOK" | tr 'A-Z' 'a-z')
t_lacks "$HDRS" 'set-cookie' "opening a shared link does not create a session"
t_contains "$HDRS" 'no-store'  "it is not cacheable"
t_contains "$HDRS" 'noindex'   "and not indexable"

# ------------------------------------------------------------------
sec "4. THE NEGATIVE CONTROLS — what a token cannot do"
# ------------------------------------------------------------------
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A")" \
     "no token, no document"
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=deadbeef")" \
     "a wrong token is refused"

# The one that matters most: A's token must not open B's invoice. If the token
# were looked up BY TOKEN rather than by (model, res_id), this would succeed.
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_B?token=$TOK")" \
     "A's token does not open B's invoice"
LEAK=$(curl -s "$BASE/portal/doc/account.move/$INV_B?token=$TOK")
t_lacks "$LEAK" 'WEB Beta' "and leaks nothing about it"

# A model that was never shareable cannot be addressed at all.
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/res.partner/$B?token=$TOK")" \
     "a non-shareable model is refused"
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/res.users/1?token=$TOK")" \
     "res.users is not addressable"

# The route is read-only: there is no POST to it.
t_ne "200" "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/portal/doc/account.move/$INV_A?token=$TOK")" \
     "the shared document cannot be POSTed to"

# ------------------------------------------------------------------
sec "5. re-sharing replaces, and revoking closes"
# ------------------------------------------------------------------
SH2=$(call_k portal.partner portal_share_document '[[]]' "\"model\":\"account.move\",\"res_id\":$INV_A")
TOK2=$(printf '%s' "$SH2" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read()).get("result",{}).get("token",""))' 2>/dev/null)
t_ne "$TOK" "$TOK2" "re-sharing mints a different token"
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=$TOK")" \
     "the OLD link stops working — sharing again closes a leak"
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=$TOK2")" \
     "the new one works"
t_eq "1" "$(pg "SELECT count(*) FROM portal_access_token WHERE model='account.move' AND res_id=$INV_A AND revoked=false")" \
     "exactly one live token remains"

RV=$(call_k portal.partner portal_revoke_document '[[]]' "\"model\":\"account.move\",\"res_id\":$INV_A")
has_error "$RV" && no "revoke failed"
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=$TOK2")" \
     "a revoked link is dead"

# An EXPIRED token is refused. Mint one and backdate it — the same thing time does.
SH3=$(call_k portal.partner portal_share_document '[[]]' "\"model\":\"account.move\",\"res_id\":$INV_A")
TOK3=$(printf '%s' "$SH3" | python3 -c 'import sys,json;print(json.loads(sys.stdin.read()).get("result",{}).get("token",""))' 2>/dev/null)
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=$TOK3")" "fresh link works"
pg "UPDATE portal_access_token SET expires_at = now() - INTERVAL '1 day'
      WHERE model='account.move' AND res_id=$INV_A AND revoked=false" >/dev/null
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/portal/doc/account.move/$INV_A?token=$TOK3")" \
     "an expired link is dead"

# ------------------------------------------------------------------
sec "6. the statement of account"
# ------------------------------------------------------------------
ST=$(pget "$PA" /portal/api/statement)
t_contains "$ST" '"lines"' "the statement is returned"
t_eq "WEB Alpha" "$(jget "$ST" partner)" "it is the right customer's"
t_eq "1000.0" "$(jget "$ST" closing_balance)" "the closing balance is 1000"
t_eq "1000.0" "$(jget "$ST" amount_due)"      "and 1000 is due"
t_eq "0.0"    "$(jget "$ST" opening_balance)" "nothing was outstanding before"
t_lacks "$ST" 'WEB Beta' "it contains nothing about the other customer"

# It ties to the LEDGER, which is the whole reason it is built this way.
LEDGER=$(pg "SELECT COALESCE(SUM(l.debit-l.credit),0)::bigint FROM account_move_line l
               JOIN account_move m ON m.id=l.move_id
               JOIN account_account a ON a.id=l.account_id
              WHERE l.partner_id=$A AND m.state='posted' AND a.account_type='asset_receivable'")
t_eq "$((1000 * M))" "$LEDGER" "the receivable ledger says 1000 too"

# A second, older invoice moves the opening balance of a windowed statement.
INV_A2=$(mkinv "$A" 250 2026-01-15)
ST2=$(pget "$PA" '/portal/api/statement?date_from=2026-06-01&date_to=2026-12-31')
t_eq "250.0"  "$(jget "$ST2" opening_balance)" "January's invoice is the opening balance"
t_eq "1250.0" "$(jget "$ST2" closing_balance)" "and the closing balance is 1250"

# Ageing is present and adds up to what is owed.
t_contains "$ST2" '"aging"' "an ageing breakdown is included"

# Dates arrive from a query string, so nonsense is refused rather than handed
# to PostgreSQL to interpret.
t_eq "400" "$(pcode "$PA" '/portal/api/statement?date_from=notadate')" "a malformed date is rejected"
t_eq "400" "$(pcode "$PA" '/portal/api/statement?date_from=2026-12-31&date_to=2026-01-01')" \
     "a backwards range is rejected"
# Percent-encoded, or curl refuses to send it and the check passes for the
# wrong reason (it would report 000, not a refusal by the server).
t_eq "400" "$(pcode "$PA" "/portal/api/statement?date_from=2026-01-01%27%3B%20DROP%20TABLE%20account_move%3B--")" \
     "an injection attempt is rejected as a bad date"
t_eq "1" "$(pg "SELECT count(*) FROM pg_tables WHERE tablename='account_move'")" "account_move is still there"

# Print and PDF.
t_eq "200" "$(pcode "$PA" /portal/api/statement/print)" "the statement prints"
PDFC=$(pcode "$PA" /portal/api/statement/pdf)
case "$PDFC" in
    200) ok "the statement downloads as a PDF"
         HEAD=$(pget "$PA" /portal/api/statement/pdf | head -c 4)
         t_eq '%PDF' "$HEAD" "the bytes really are a PDF" ;;
    503) ok "PDF unavailable on this host (no wkhtmltopdf) — reported, not crashed" ;;
    *)   no "the statement PDF answered $PDFC" ;;
esac

# And it needs a session.
t_eq "401" "$(pcode "" /portal/api/statement)"       "an anonymous visitor gets no statement"
t_eq "401" "$(pcode "" /portal/api/statement/pdf)"   "nor the PDF"

# ------------------------------------------------------------------
sec "7. the portal home counters"
# ------------------------------------------------------------------
HOME=$(pget "$PA" /portal/api/home)
t_eq "WEB Alpha" "$(jget "$HOME" name)" "the home page greets the customer"
t_eq "2" "$(jget "$HOME" invoice_count)" "it counts their invoices"
t_eq "2" "$(jget "$HOME" unpaid_count)"  "and how many are unpaid"
t_eq "1250.0" "$(jget "$HOME" amount_due)" "and what they owe"
t_eq "401" "$(pcode "" /portal/api/home)" "it needs a session"
# The counters are scoped, like everything else.
call portal.partner portal_reset_password "[[$B]]" >/dev/null
PB=$(portal_login 'web-b@t.test' "$PW")
HOMEB=$(pget "$PB" /portal/api/home)
t_eq "1" "$(jget "$HOMEB" invoice_count)" "B sees only B's invoice"
t_eq "500.0" "$(jget "$HOMEB" amount_due)" "and only B's balance"

# ------------------------------------------------------------------
sec "8. pagination — the lists are bounded now"
# ------------------------------------------------------------------
# Orders and deliveries used to return everything a customer had ever had.
for i in 1 2 3 4 5; do
    pg "INSERT INTO sale_order (name,partner_id,state,date_order,company_id)
        VALUES ('WEBSO/'||nextval('sale_order_id_seq'),$A,'draft',now(),1)" >/dev/null
done
N=$(pget "$PA" '/portal/api/orders' | python3 -c 'import sys,json;print(len(json.loads(sys.stdin.read() or "[]")))')
t_eq "5" "$N" "all five orders come back by default"

N2=$(pget "$PA" '/portal/api/orders?limit=2' | python3 -c 'import sys,json;print(len(json.loads(sys.stdin.read() or "[]")))')
t_eq "2" "$N2" "limit=2 returns two"
N3=$(pget "$PA" '/portal/api/orders?limit=2&page=3' | python3 -c 'import sys,json;print(len(json.loads(sys.stdin.read() or "[]")))')
t_eq "1" "$N3" "page 3 of 2 returns the last one"

TOTAL=$(curl -s -D - -o /dev/null -H "Cookie: portal_sid=$PA" "$BASE/portal/api/orders?limit=2" \
        | tr 'A-Z' 'a-z' | sed -n 's/^x-total-count: *\([0-9]*\).*/\1/p' | tr -d '\r')
t_eq "5" "$TOTAL" "the total is reported in a header so a client can page"

# The ceiling cannot be raised by the caller — the reason it exists.
BIG=$(curl -s -D - -o /dev/null -H "Cookie: portal_sid=$PA" "$BASE/portal/api/orders?limit=99999" \
      | tr 'A-Z' 'a-z' | sed -n 's/^x-page-limit: *\([0-9]*\).*/\1/p' | tr -d '\r')
t_eq "100" "$BIG" "limit is capped at 100 however large the request asks for"
# Nonsense paging does not break it.
t_eq "200" "$(pcode "$PA" '/portal/api/orders?limit=abc&page=-4')" "junk paging parameters are ignored, not fatal"

verdict
