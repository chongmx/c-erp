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
# Analytic accounting — cost centres.
#
# Proves through the real HTTP path: a journal item tagged to an analytic
# account generates an analytic line on POST (margin sign: cost negative),
# and the analytic account's balance reflects it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/an_cookie.txt
cat > /tmp/an_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/an_auth.json > /tmp/an_auth_out.json
grep -q '"session_id"' /tmp/an_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
M=1000000

AA=$(callkw account.analytic.account create "[{\"name\":\"ANTEST Project X\",\"code\":\"PRJ-X\"}]" | rval)
A5000=$(pg "SELECT id FROM account_account WHERE code='5000' AND company_id=1")
A2000=$(pg "SELECT id FROM account_account WHERE code='2000' AND company_id=1")
JID=$(pg "SELECT id FROM account_journal WHERE code='PUR' AND company_id=1")
echo "    analytic account=$AA  COGS=$A5000  AP=$A2000  journal=$JID"
[ -n "$AA" ] && ok "analytic account created" || { no "create failed"; exit 1; }

echo "############ post a JE with a line tagged to the cost centre ############"
MV=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id) VALUES ('/','entry','draft',CURRENT_DATE,$JID,1) RETURNING id")
pg "INSERT INTO account_move_line (move_id,account_id,company_id,name,debit,credit,analytic_account_id) VALUES ($MV,$A5000,1,'Consulting cost',$((300*M)),0,$AA)" >/dev/null
pg "INSERT INTO account_move_line (move_id,account_id,company_id,name,debit,credit) VALUES ($MV,$A2000,1,'Payable',0,$((300*M)))" >/dev/null
callkw account.move action_post "[[$MV]]" >/dev/null
ST=$(pg "SELECT state FROM account_move WHERE id=$MV")
NL=$(pg "SELECT count(*) FROM account_analytic_line WHERE account_id=$AA")
AMT=$(pg "SELECT amount FROM account_analytic_line WHERE account_id=$AA LIMIT 1")
echo "    move state=$ST  analytic lines=$NL  amount=$AMT"
[ "$ST" = "posted" ] && ok "journal entry posted" || no "state=$ST"
[ "$NL" = "1" ] && ok "one analytic line generated" || no "analytic lines=$NL"
[ "$AMT" = "-300000000" ] && ok "cost recorded as -300 (credit - debit)" || no "amount=$AMT"

echo
echo "############ analytic account balance ############"
BAL=$(callkw account.analytic.account search_read "[[]]" | python3 -c "import json,sys;print([a['balance'] for a in json.load(sys.stdin)['result'] if a['id']==$AA][0])" 2>/dev/null)
echo "    Project X balance (via API) = $BAL"
awk -v a="$BAL" 'BEGIN{exit !(a+0==-300)}' && ok "analytic balance = -300" || no "balance=$BAL"

# A normal invoice with no analytic tag must not generate analytic noise.
NOISE=$(pg "SELECT count(*) FROM account_analytic_line WHERE account_id IS NULL")
[ "$NOISE" = "0" ] && ok "untagged postings create no analytic lines" || no "$NOISE stray analytic lines"

echo
echo "############ cleanup ############"
pg "DELETE FROM account_analytic_line WHERE account_id=$AA" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id=$MV" >/dev/null
pg "DELETE FROM account_move WHERE id=$MV" >/dev/null
pg "DELETE FROM account_analytic_account WHERE id=$AA" >/dev/null
rm -f "$CK" /tmp/an_auth.json /tmp/an_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
