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
# Hand-entered credit notes and vendor bills (docs/092).
#
# verify_credit_note.sh covers the REVERSAL path — the tested, recommended way
# to raise a credit note. This covers the other one: a user clicking New on the
# Invoices list, choosing Credit Note, and typing the lines in.
#
# That path was broken and known to be broken (docs/082 follow-ups):
# recompute_totals assumed the customer-invoice sign convention everywhere.
# A credit note's product lines are DEBITS, so "sum the credits" returned zero
# and the counterparty line — matched by `WHERE debit>0` — was the revenue line
# rather than the receivable.
#
# The load-bearing assertions are per document type: the total must equal the
# lines, the tax must land on the SAME side as the lines it came from, and the
# entry must balance. A document that does not balance cannot be posted, and
# one that balances with the wrong sign quietly corrupts the ledger.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }

PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
JRN=$(pg "SELECT id FROM account_journal WHERE type='sale' ORDER BY id LIMIT 1")
PJRN=$(pg "SELECT id FROM account_journal WHERE type='purchase' ORDER BY id LIMIT 1")
INC=$(pg "SELECT id FROM account_account WHERE account_type='income' ORDER BY id LIMIT 1")
EXP=$(pg "SELECT id FROM account_account WHERE account_type='expense' ORDER BY id LIMIT 1")
AR=$(pg "SELECT id FROM account_account WHERE account_type='asset_receivable' ORDER BY id LIMIT 1")
AP=$(pg "SELECT id FROM account_account WHERE account_type='liability_payable' ORDER BY id LIMIT 1")
TAX=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' AND amount=10 ORDER BY id LIMIT 1")
[ -z "$TAX" ] && TAX=$(pg "SELECT id FROM account_tax WHERE type_tax_use='sale' ORDER BY id LIMIT 1")
RATE=$(pg "SELECT amount FROM account_tax WHERE id=$TAX")
echo "    fixtures: partner=$PARTNER income=$INC expense=$EXP AR=$AR AP=$AP tax=$TAX (${RATE}%)"

pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE ref LIKE 'QA-SIGN%')" >/dev/null
pg "DELETE FROM account_move WHERE ref LIKE 'QA-SIGN%'" >/dev/null

# build <move_type> <journal> <income-or-expense acct> <counterparty acct> <product-side>
# Creates a draft document with one 100.00 line carrying $TAX, on the side the
# document type calls for, then recomputes.
build(){
    local mtype="$1" jrn="$2" acct="$3" cpty="$4" side="$5" mv
    mv=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id,partner_id,ref,amount_total)
             VALUES ('/','$mtype','draft',CURRENT_DATE,$jrn,1,$PARTNER,'QA-SIGN-$mtype',0) RETURNING id" | head -1)
    if [ "$side" = "credit" ]; then
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,quantity,price_unit,tax_ids_json,debit,credit,display_type)
            VALUES ($mv,$acct,$jrn,1,CURRENT_DATE,'QA line',1000000,100000000,'[$TAX]',0,100000000,'')" >/dev/null
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
            VALUES ($mv,$cpty,$jrn,1,CURRENT_DATE,'QA counterparty',100000000,0,'')" >/dev/null
    else
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,quantity,price_unit,tax_ids_json,debit,credit,display_type)
            VALUES ($mv,$acct,$jrn,1,CURRENT_DATE,'QA line',1000000,100000000,'[$TAX]',100000000,0,'')" >/dev/null
        pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
            VALUES ($mv,$cpty,$jrn,1,CURRENT_DATE,'QA counterparty',0,100000000,'')" >/dev/null
    fi
    call account.move recompute_totals "[[$mv]]" "{$CTX}" >/dev/null
    echo "$mv"
}

# assert <label> <move-id> <expected counterparty side>
check(){
    local label="$1" mv="$2" cside="$3"
    local untaxed tax total dr cr expected
    untaxed=$(pg "SELECT amount_untaxed FROM account_move WHERE id=$mv")
    tax=$(pg "SELECT amount_tax FROM account_move WHERE id=$mv")
    total=$(pg "SELECT amount_total FROM account_move WHERE id=$mv")
    expected=$(pg "SELECT (100000000 * $RATE / 100)::bigint")

    [ "$untaxed" = "100000000" ] && ok "$label: untaxed = RM100 from the lines" \
                                 || no "$label: untaxed = $untaxed (expected 100000000)"
    [ "$tax" = "$expected" ] && ok "$label: tax = ${RATE}% of the line" \
                            || no "$label: tax = $tax (expected $expected)"
    [ "$total" = "$((100000000 + expected))" ] && ok "$label: total = lines + tax" \
                                               || no "$label: total = $total"

    # The generated tax line must sit on the same side as the product line.
    local tdr tcr
    tdr=$(pg "SELECT COALESCE(SUM(debit),0)  FROM account_move_line WHERE move_id=$mv AND tax_line_id IS NOT NULL")
    tcr=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$mv AND tax_line_id IS NOT NULL")
    if [ "$cside" = "credit" ]; then   # counterparty on credit → product+tax on debit
        [ "$tdr" = "$expected" ] && [ "$tcr" = "0" ] && ok "$label: the tax line is a DEBIT, like its product line" \
            || no "$label: tax line on the wrong side (dr=$tdr cr=$tcr)"
    else
        [ "$tcr" = "$expected" ] && [ "$tdr" = "0" ] && ok "$label: the tax line is a CREDIT, like its product line" \
            || no "$label: tax line on the wrong side (dr=$tdr cr=$tcr)"
    fi

    # The counterparty line must carry the full total, on its own side.
    local cval
    cval=$(pg "SELECT COALESCE(SUM($cside),0) FROM account_move_line l
                WHERE l.move_id=$mv AND l.tax_line_id IS NULL AND l.name='QA counterparty'")
    [ "$cval" = "$((100000000 + expected))" ] \
        && ok "$label: the counterparty line was updated to the total" \
        || no "$label: counterparty = $cval (expected $((100000000 + expected)))"

    # And the whole thing must balance, or it can never be posted.
    dr=$(pg "SELECT COALESCE(SUM(debit),0)  FROM account_move_line WHERE move_id=$mv")
    cr=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$mv")
    [ "$dr" = "$cr" ] && ok "$label: balances (Dr $dr = Cr $cr)" || no "$label: UNBALANCED Dr $dr vs Cr $cr"
}

echo "############ customer invoice (the shape that always worked) ############"
M1=$(build out_invoice "$JRN" "$INC" "$AR" credit)
check "invoice" "$M1" debit

echo "############ hand-entered credit note (the case that was broken) ############"
M2=$(build out_refund "$JRN" "$INC" "$AR" debit)
check "credit note" "$M2" credit

echo "############ vendor bill ############"
M3=$(build in_invoice "$PJRN" "$EXP" "$AP" debit)
check "vendor bill" "$M3" credit

echo "############ vendor credit note ############"
M4=$(build in_refund "$PJRN" "$EXP" "$AP" credit)
check "vendor credit" "$M4" debit

echo "############ recompute is idempotent for every type ############"
for mv in $M1 $M2 $M3 $M4; do
    before=$(pg "SELECT amount_total FROM account_move WHERE id=$mv")
    call account.move recompute_totals "[[$mv]]" "{$CTX}" >/dev/null
    call account.move recompute_totals "[[$mv]]" "{$CTX}" >/dev/null
    after=$(pg "SELECT amount_total FROM account_move WHERE id=$mv")
    [ "$before" = "$after" ] || { no "move $mv total drifted on recompute ($before -> $after)"; break; }
done
[ -z "$FAILED" ] && ok "recomputing twice more changes nothing" || true
# and no duplicate tax lines piled up
DUP=$(pg "SELECT COALESCE(MAX(c),0) FROM (SELECT count(*) AS c FROM account_move_line
          WHERE move_id IN ($M1,$M2,$M3,$M4) AND tax_line_id IS NOT NULL
          GROUP BY move_id, tax_line_id) x")
[ "$DUP" = "1" ] && ok "one tax line per tax, not one per recompute" || no "$DUP tax lines for a single tax"

echo "############ housekeeping ############"
pg "DELETE FROM account_move_line WHERE move_id IN ($M1,$M2,$M3,$M4)" >/dev/null
pg "DELETE FROM account_move WHERE id IN ($M1,$M2,$M3,$M4)" >/dev/null
BAL=$(pg "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted'")
[ "$BAL" = "0" ] && ok "fixtures removed; posted ledger still balances" || no "ledger unbalanced by $BAL"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
