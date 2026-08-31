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
# Runtime verification for the fixes recorded in docs/041.
# Self-authenticating. Safe to re-run.
#
# NOTE: call_kw resolves the session from kwargs.context.session_id,
# NOT from the Cookie header (see docs/042 S-48). The helper below
# injects it, which is what the OWL frontend does too.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
LOGIN=${LOGIN:-admin}
PASSWD=${PASSWD:-admin}
FAILED=

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

# call model method args-json  -> prints response
call() {
    local model=$1 method=$2 args=$3
    cat > /tmp/v_call.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$model","method":"$method",
 "args":$args,"kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/v_call.json
}

echo "############ setup ############"
cat > /tmp/v_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"$LOGIN","password":"$PASSWD"}}
EOF
A=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/v_auth.json)
SID=$(printf '%s' "$A" | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "    cannot authenticate: $A"; exit 1; }
echo "    sid=${SID:0:8}..."
printf '%s' "$(call product.category search_read '[[]]')" | grep -q '"result"' \
    && ok "authenticated call_kw works" || no "auth broken — later checks invalid"

# Guarantee a sale order exists so the PDF exec-path probes have a real record
# to render (they used to hardcode id=2, which need not exist).
source scripts/seed_test_fixtures.sh; ensure_sale_fixture "$SID" >/dev/null 2>&1
SO=$(pg "SELECT id FROM sale_order ORDER BY id LIMIT 1")

# =============================================================
echo
echo "############ S-39 — command injection in PDF path ############"
rm -f /tmp/pwned1 /tmp/pwned2 /tmp/pwned3

code=$(curl -s -H "Cookie: session_id=$SID" -o /tmp/v_ok.pdf -w '%{http_code}' "$BASE/report/pdf/sale.order/$SO")
if [ "$code" = "200" ] && head -c 4 /tmp/v_ok.pdf | grep -q '%PDF'; then
    ok "baseline PDF renders (http=$code, $(stat -c%s /tmp/v_ok.pdf) bytes) — exec path IS reached"
else
    no "baseline PDF failed (http=$code)"
fi

echo "    -- router reachability probe (does the segment reach the handler?) --"
for spec in 'plain:2' 'dollar:2$x' 'paren:2(x)' 'backtick:2`x`' 'semi:2;x' 'space:2%20x' 'dot:2.x' 'dash:2-x'; do
    label=${spec%%:*}; seg=${spec#*:}
    c=$(curl -s -g -H "Cookie: session_id=$SID" -o /dev/null -w '%{http_code}' "$BASE/report/pdf/sale.order/$seg")
    echo "      id='$seg' -> http=$c"
done

echo "    -- live injection probes --"
# Payloads must contain NO '/' — a slash adds a path segment and the request
# 404s at the router before reaching the handler, which would produce a
# meaningless pass. Marker files therefore land in the server's CWD.
CWD=$(cd /home/user/code/c-erp && pwd)
rm -f "$CWD"/pwnedA "$CWD"/pwnedB "$CWD"/pwnedC
for spec in 'cmdsub:2$(touch%20pwnedA)' 'backtick:2`touch%20pwnedB`' 'semicolon:2;touch%20pwnedC'; do
    label=${spec%%:*}; payload=${spec#*:}
    c=$(curl -s -g -H "Cookie: session_id=$SID" -o /dev/null -w '%{http_code}' "$BASE/report/pdf/sale.order/$payload")
    echo "      probe[$label] http=$c  (200 = reached the handler)"
done
sleep 1
for f in pwnedA pwnedB pwnedC; do
    [ -e "$CWD/$f" ] && { no "*** RCE: $CWD/$f created ***"; rm -f "$CWD/$f"; } || ok "$f absent"
done

# =============================================================
echo
echo "############ C-3 — category parent cycle rejected ############"
P=$(pg "SELECT id FROM product_category WHERE parent_id IS NULL ORDER BY id LIMIT 1")
C=$(pg "SELECT id FROM product_category WHERE parent_id=$P ORDER BY id LIMIT 1")
if [ -n "$P" ] && [ -n "$C" ]; then
    echo "    parent=$P child=$C ; set parent.parent_id := child"
    R=$(call product.category write "[[$P],{\"parent_id\":$C}]")
    printf '%s' "$R" | grep -qi 'descendant' && ok "cycle rejected" || no "not rejected: $(echo $R | head -c 150)"
    [ "$(pg "SELECT COALESCE(parent_id,0) FROM product_category WHERE id=$P")" = "0" ] \
        && ok "parent_id unchanged in DB" || no "parent_id was modified"
else
    echo "    SKIP  no parent/child pair"
fi

# =============================================================
echo
echo "############ C-4 — category delete guarded ############"
if [ -n "$P" ]; then
    R=$(call product.category unlink "[[$P]]")
    printf '%s' "$R" | grep -q 'Cannot delete this category' && ok "delete refused with counts" || no "not refused: $(echo $R | head -c 150)"
    [ "$(pg "SELECT count(*) FROM product_category WHERE id=$P")" = "1" ] \
        && ok "category still present" || no "category was deleted"
fi

# =============================================================
echo
echo "############ S-47 — audit on identity / privilege ############"
BEFORE=$(pg "SELECT count(*) FROM audit_log")
pg "DELETE FROM res_users WHERE login='audittest'" >/dev/null
R=$(call res.users create '[{"login":"audittest","name":"Audit Test","password":"Sup3rSecret!x"}]')
NEW=$(printf '%s' "$R" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
echo "    res.users create -> id=${NEW:-none}  $( [ -z "$NEW" ] && echo "resp=$(echo $R|head -c 120)")"
if [ -n "$NEW" ]; then
    call res.users write "[[$NEW],{\"name\":\"Audit Test 2\"}]" >/dev/null
    call res.users unlink "[[$NEW]]" >/dev/null
fi
AFTER=$(pg "SELECT count(*) FROM audit_log")
echo "    audit_log rows: $BEFORE -> $AFTER"
[ "$AFTER" -gt "$BEFORE" ] && ok "audit rows written" || no "no audit rows written"
echo "    recent entries:"
PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc \
  "SELECT model||' '||operation||' ids='||record_ids::text||' uid='||uid FROM audit_log ORDER BY id DESC LIMIT 6" \
  2>/dev/null | sed 's/^/      /'

# =============================================================
echo
echo "############ S-40 — proxy-aware client IP ############"
echo "    peer is 127.0.0.1 (trusted) so forwarding headers are honoured:"
for h in "X-Real-IP: 8.8.8.8" "X-Forwarded-For: 1.2.3.4, 9.9.9.9" "X-Forwarded-For: 7.7.7.7"; do
    curl -s -o /dev/null -H "$h" -X POST "$BASE/web/session/authenticate" \
         -H 'Content-Type: application/json' \
         --data "{\"jsonrpc\":\"2.0\",\"params\":{\"db\":\"$DBN\",\"login\":\"nosuch\",\"password\":\"bad\"}}"
    echo "      sent [$h]"
done
echo "    (XFF '1.2.3.4, 9.9.9.9' must bucket as 9.9.9.9 — the LAST element)"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
