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
# Bank reconciliation.
#
# Proves through the real HTTP path:
#   * suggest_matches offers the open invoice that clears a statement line
#   * reconcile posts the bank entry (Dr Bank / Cr Receivable), clears the
#     invoice residual (paid), and marks the statement line reconciled
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/bk_cookie.txt
cat > /tmp/bk_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/bk_auth.json > /tmp/bk_auth_out.json
grep -q '"session_id"' /tmp/bk_auth_out.json || { echo "cannot authenticate"; exit 1; }

callkw() {
    curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":${3:-[]},\"kwargs\":{}}}"
}
rval() { python3 -c "import json,sys; print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }
M=1000000

AR=$(pg "SELECT id FROM account_account WHERE code='1200' AND company_id=1")
SALES=$(pg "SELECT id FROM account_account WHERE code='4000' AND company_id=1")
BANKA=$(pg "SELECT id FROM account_account WHERE code='1100' AND company_id=1")
BNK=$(pg "SELECT id FROM account_journal WHERE code='BNK' AND company_id=1")
SAL=$(pg "SELECT id FROM account_journal WHERE code='SAL' AND company_id=1")
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")

# A posted customer invoice, AMT outstanding.
#
# AMT is a deliberately odd number, not a round 100. suggest_matches returns
# `ORDER BY (amount_residual = $1) DESC, (partner_id = $2) DESC, date LIMIT 20`,
# and this database accumulates open invoices for exactly 100 against the first
# partner, dated today. Every tie-break in that ORDER BY then ties, the ordering
# falls back to arbitrary, and the fixture's own invoice — the newest — drops
# off the end of the 20. The test failed for a reason that had nothing to do
# with reconciliation.
#
# A distinctive amount puts this invoice first on the exact-amount match, so the
# assertion depends on the feature and not on what else is in the database.
AMT=1373
INV=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id,partner_id,amount_total,amount_residual,payment_state) VALUES ('INV/BKT','out_invoice','posted',CURRENT_DATE,$SAL,1,$PARTNER,$((AMT*M)),$((AMT*M)),'not_paid') RETURNING id")
pg "INSERT INTO account_move_line (move_id,account_id,company_id,name,debit,credit) VALUES ($INV,$AR,1,'Receivable',$((AMT*M)),0)" >/dev/null
pg "INSERT INTO account_move_line (move_id,account_id,company_id,name,debit,credit) VALUES ($INV,$SALES,1,'Revenue',0,$((AMT*M)))" >/dev/null
echo "    invoice=$INV (residual $AMT)  AR=$AR bank=$BANKA journal=$BNK"

echo "############ 1. a bank statement line ############"
STMT=$(callkw account.bank.statement create "[{\"name\":\"BKT-Statement\",\"journal_id\":$BNK}]" | rval)
LINE=$(callkw account.bank.statement.line create "[{\"statement_id\":$STMT,\"name\":\"Customer payment\",\"partner_id\":$PARTNER,\"amount\":$AMT}]" | rval)
echo "    statement=$STMT  line=$LINE"
[ -n "$LINE" ] && ok "statement + line created" || { no "setup failed"; exit 1; }

echo
echo "############ 2. suggest_matches offers the invoice ############"
SM=$(callkw account.bank.statement.line suggest_matches "[{\"line_id\":$LINE}]")
echo "    suggestions -> $(printf '%s' "$SM" | head -c 160)"
printf '%s' "$SM" | grep -q "\"id\":$INV" && ok "open invoice suggested as a match" || no "invoice not suggested"

echo
echo "############ 3. reconcile clears the invoice + posts the bank entry ############"
RES=$(callkw account.bank.statement.line reconcile "[{\"line_id\":$LINE,\"move_id\":$INV}]")
BJE=$(printf '%s' "$RES" | python3 -c "import json,sys;print(json.load(sys.stdin)['result']['bank_move_id'])" 2>/dev/null)
echo "    reconcile -> bank journal entry $BJE"
RESID=$(pg "SELECT amount_residual FROM account_move WHERE id=$INV")
PS=$(pg "SELECT payment_state FROM account_move WHERE id=$INV")
REC=$(pg "SELECT is_reconciled FROM account_bank_statement_line WHERE id=$LINE")
DRBANK=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line WHERE move_id=$BJE AND account_id=$BANKA")
CRAR=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$BJE AND account_id=$AR")
echo "    invoice residual=$RESID  payment_state=$PS  line reconciled=$REC  Dr bank=$DRBANK Cr AR=$CRAR"
[ "$RESID" = "0" ] && ok "invoice fully paid (residual 0)"      || no "residual=$RESID"
[ "$PS" = "paid" ] && ok "payment_state = paid"                 || no "payment_state=$PS"
[ "$REC" = "t" ] && ok "statement line marked reconciled"       || no "is_reconciled=$REC"
[ "$DRBANK" = "$((AMT*M))" ] && ok "bank debited $AMT"          || no "Dr bank=$DRBANK"
[ "$CRAR" = "$((AMT*M))" ] && ok "receivable credited $AMT"     || no "Cr AR=$CRAR"

echo
echo "############ cleanup ############"
pg "DELETE FROM account_move_line WHERE move_id IN ($INV,$BJE)" >/dev/null
pg "DELETE FROM account_bank_statement_line WHERE id=$LINE" >/dev/null
pg "DELETE FROM account_bank_statement WHERE id=$STMT" >/dev/null
pg "DELETE FROM account_move WHERE id IN ($INV,$BJE)" >/dev/null
rm -f "$CK" /tmp/bk_auth.json /tmp/bk_auth_out.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
