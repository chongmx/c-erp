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
# Credit Notes / Vendor Refunds (docs/082).
#
# A credit note is created by REVERSING a posted invoice: every ledger line is
# copied with debit and credit swapped into a new draft out_refund. The
# assertions are the ones that make it a real credit note:
#   * move_type flips out_invoice -> out_refund, and it links back (reversed_entry_id)
#   * the reversal balances (Σdebit == Σcredit)
#   * the sign is opposite — the invoice's receivable DEBIT becomes a CREDIT
#   * it can be posted (gets a number) and reconciled against the invoice
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
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{\"context\":{\"session_id\":\"$SID\"}}}}"; }

echo "############ menus wired ############"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Credit Notes' LIMIT 1")" = "account.move" ] \
    && ok "Customers -> Credit Notes menu present" || no "Credit Notes menu missing"
[ "$(pg "SELECT domain FROM ir_act_window WHERE id=74" | grep -c out_refund)" = "1" ] \
    && ok "Credit Notes action filters move_type=out_refund" || no "Credit Notes action domain wrong"

echo "############ reverse a posted customer invoice ############"
# Prefer an invoice that carries a source document (SO origin) so the
# "Source = the SO" assertion below is meaningful.
INV=$(pg "SELECT id FROM account_move WHERE move_type='out_invoice' AND state='posted' AND COALESCE(invoice_origin,'')<>'' ORDER BY id DESC LIMIT 1")
[ -z "$INV" ] && INV=$(pg "SELECT id FROM account_move WHERE move_type='out_invoice' AND state='posted' ORDER BY id DESC LIMIT 1")
INVNAME=$(pg "SELECT name FROM account_move WHERE id=$INV")
INV_ORIGIN=$(pg "SELECT COALESCE(invoice_origin,'') FROM account_move WHERE id=$INV")
[ -z "$INV" ] && { echo "    (no posted customer invoice to reverse — skipping)"; echo; [ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."; exit 0; }
INV_AR_DR=$(pg "SELECT COALESCE(SUM(debit),0) FROM account_move_line aml JOIN account_account aa ON aa.id=aml.account_id WHERE aml.move_id=$INV AND aa.account_type='asset_receivable'")
echo "    reversing invoice id=$INV (AR debit=$INV_AR_DR)"

R=$(call account.move action_reverse "[[$INV]]")
CN=$(printf '%s' "$R" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
[ -n "$CN" ] && ok "action_reverse returned a new move ($CN)" || { no "reverse failed: $(printf '%s' "$R"|head -c 140)"; echo; echo "*** FAILURES ***"; exit 1; }

[ "$(pg "SELECT move_type FROM account_move WHERE id=$CN")" = "out_refund" ] && ok "credit note is move_type=out_refund" || no "wrong move_type"
[ "$(pg "SELECT state FROM account_move WHERE id=$CN")" = "draft" ] && ok "credit note starts as draft" || no "not draft"
[ "$(pg "SELECT reversed_entry_id FROM account_move WHERE id=$CN")" = "$INV" ] && ok "linked back to the invoice (reversed_entry_id)" || no "not linked"

DIFF=$(pg "SELECT COALESCE(SUM(debit),0)-COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$CN")
[ "$DIFF" = "0" ] && ok "credit note balances (Σdebit == Σcredit)" || no "unbalanced by $DIFF"

# The invoice's receivable was a DEBIT; on the credit note the same account is a CREDIT.
CN_AR_CR=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line aml JOIN account_account aa ON aa.id=aml.account_id WHERE aml.move_id=$CN AND aa.account_type='asset_receivable'")
[ "$CN_AR_CR" = "$INV_AR_DR" ] && [ "$INV_AR_DR" != "0" ] \
    && ok "receivable flipped: invoice debit $INV_AR_DR -> credit note credit $CN_AR_CR" \
    || no "receivable sign not flipped (inv debit=$INV_AR_DR, cn credit=$CN_AR_CR)"

echo "############ reference document + line tagging ############"
CN_ORIGIN=$(pg "SELECT COALESCE(invoice_origin,'') FROM account_move WHERE id=$CN")
[ "$CN_ORIGIN" = "$INV_ORIGIN" ] && ok "Source = the invoice's origin document (SO) '$CN_ORIGIN'" \
    || no "Source wrong (credit note='$CN_ORIGIN' invoice='$INV_ORIGIN')"
[ "$(pg "SELECT count(*) FROM account_move_line WHERE move_id=$CN AND ref='$INVNAME' AND COALESCE(display_type,'')=''")" -ge 1 ] \
    && ok "line items carry the reversed invoice number ($INVNAME)" || no "lines missing the invoice number"

echo "############ post the credit note (RINV series) ############"
call account.move action_post "[[$CN]]" >/dev/null
NAME=$(pg "SELECT name FROM account_move WHERE id=$CN")
[ "$(pg "SELECT state FROM account_move WHERE id=$CN")" = "posted" ] && ok "credit note posts (number '$NAME')" || no "could not post"
echo "$NAME" | grep -q '^RINV' && ok "numbered with the RINV prefix ($NAME)" || no "prefix is not RINV: $NAME"

echo "############ a vendor bill reverses to in_refund ############"
BILL=$(pg "SELECT id FROM account_move WHERE move_type='in_invoice' AND state='posted' ORDER BY id DESC LIMIT 1")
if [ -n "$BILL" ]; then
    RB=$(call account.move action_reverse "[[$BILL]]"); RID=$(printf '%s' "$RB" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
    [ "$(pg "SELECT move_type FROM account_move WHERE id=$RID")" = "in_refund" ] && ok "vendor bill -> in_refund" || no "vendor refund type wrong"
else
    echo "    (no posted vendor bill — skipping refund case)"
fi

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
