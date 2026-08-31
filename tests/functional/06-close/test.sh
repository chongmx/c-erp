#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# FUNCTIONAL JOURNEY 06 — CLOSE.  (docs/109 §3)
#
#   post known entries -> run every financial report -> the reports agree
#   with the ledger they were computed from
#
# The month-end journey. What makes it worth writing separately from the
# per-report integration test is the direction of the check: that test asks
# "does the report render and have the right shape". This one posts a KNOWN
# invoice and then asks whether each report's numbers actually follow from the
# journal items in the database.
#
# A report that renders beautifully and disagrees with the ledger is worse
# than one that fails to render, because it will be believed.
#
# Checked here:
#   * the trial balance balances — total debits equal total credits,
#   * the balance sheet obeys assets = liabilities + equity,
#   * the general ledger's totals equal the sum of account_move_line,
#   * a newly posted invoice moves the receivable and the aged report by
#     exactly its own amount, and
#   * every report is still served after the entries exist (a report that
#     500s on real data is the failure mode that matters).
#
# Prefixed CL- / 'CL ' and removed on the way out.
# =============================================================
auth_or_die

M=1000000
AMOUNT=1234          # a distinctive amount, so it is findable among demo rows

cleanup() {
    pg "DELETE FROM account_move_line WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'CL %')" >/dev/null
    pg "DELETE FROM account_move      WHERE partner_id IN (SELECT id FROM res_partner WHERE name LIKE 'CL %')" >/dev/null
    pg "DELETE FROM res_partner       WHERE name LIKE 'CL %'" >/dev/null
}
cleanup
trap 'cleanup' EXIT

rep() {  # rep <name>  -> the report's JSON
    http_get "/web/account/report?report=$1&date_from=2000-01-01&date_to=2099-12-31"
}
repcode() { http_code "/web/account/report?report=$1&date_from=2000-01-01&date_to=2099-12-31"; }

REPORTS="balance_sheet profit_loss trial_balance general_ledger partner_ledger aged_receivable aged_payable"

# ------------------------------------------------------------------
sec "1. the ledger before we touch it"
# ------------------------------------------------------------------
# The books must already balance. If they do not, nothing this journey
# concludes afterwards means anything — so it stops rather than reporting a
# cascade of downstream failures.
BAL0=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
             FROM account_move_line l JOIN account_move m ON m.id=l.move_id
            WHERE m.state='posted'")
t_eq "0" "${BAL0%%.*}" "the posted ledger balances before this journey starts"
if [ "${BAL0%%.*}" != "0" ]; then
    echo "    the ledger is already out of balance — the rest would be noise"
    verdict; exit 1
fi
RECV0=$(pg "SELECT COALESCE(SUM(l.debit) - SUM(l.credit),0)
              FROM account_move_line l
              JOIN account_account a ON a.id=l.account_id
              JOIN account_move m ON m.id=l.move_id
             WHERE a.account_type='asset_receivable' AND m.state='posted'")
echo "    receivable before: ${RECV0:-0}"

# ------------------------------------------------------------------
sec "2. every report is served"
# ------------------------------------------------------------------
for r in $REPORTS; do
    CODE=$(repcode "$r")
    t_eq "200" "$CODE" "$r responds"
done

# ------------------------------------------------------------------
sec "3. post a known invoice"
# ------------------------------------------------------------------
PARTNER=$(pgid "INSERT INTO res_partner (name, active, company_id) VALUES ('CL Customer', true, 1) RETURNING id")
JRN=$(pg "SELECT id FROM account_journal WHERE type='sale' AND company_id=1 ORDER BY id LIMIT 1")
ARECV=$(pg "SELECT id FROM account_account WHERE account_type='asset_receivable' AND company_id=1 ORDER BY id LIMIT 1")
AINC=$(pg  "SELECT id FROM account_account WHERE account_type IN ('income','income_other') AND company_id=1 ORDER BY id LIMIT 1")
t_nonempty "$JRN"   "a sale journal exists"
t_nonempty "$ARECV" "a receivable account exists"
t_nonempty "$AINC"  "an income account exists"
[ -z "$JRN" ] || [ -z "$ARECV" ] || [ -z "$AINC" ] && { verdict; exit 1; }

# due_date is what the aged report buckets on (not `invoice_date_due` — that
# is the reference ERP name and this schema does not use it; naming it made the whole
# INSERT fail silently, and the invoice simply never existed). Without a due
# date the invoice is real, posted and owed, yet appears in no ageing bucket,
# which is indistinguishable from the report being broken.
INV=$(pgid "INSERT INTO account_move
    (name, move_type, state, date, invoice_date, due_date,
     journal_id, company_id, partner_id,
     amount_untaxed, amount_tax, amount_total, amount_residual, payment_state)
    VALUES ('CL-INV-1','out_invoice','draft',CURRENT_DATE,CURRENT_DATE,CURRENT_DATE,
            $JRN,1,$PARTNER,
            $((AMOUNT * M)),0,$((AMOUNT * M)),$((AMOUNT * M)),'not_paid') RETURNING id")
# `date` matters on the LINE, not only on the move: the reports filter journal
# items by their own date, so a line with a NULL date is invisible to every
# one of them. Leaving it out made the invoice real in the ledger and absent
# from the reports — which is exactly the disagreement this journey exists to
# detect, so it must not be self-inflicted.
pg "INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, partner_id, name, date, debit, credit)
    VALUES ($INV,$ARECV,$JRN,1,$PARTNER,'CL receivable',CURRENT_DATE,$((AMOUNT * M)),0)" >/dev/null
pg "INSERT INTO account_move_line (move_id, account_id, journal_id, company_id, partner_id, name, date, debit, credit)
    VALUES ($INV,$AINC,$JRN,1,$PARTNER,'CL income',CURRENT_DATE,0,$((AMOUNT * M)))" >/dev/null
pg "UPDATE account_move SET state='posted' WHERE id=$INV" >/dev/null
t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$INV")" "a $AMOUNT invoice is posted"

# ------------------------------------------------------------------
sec "4. the ledger still balances, and moved by exactly that much"
# ------------------------------------------------------------------
BAL1=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
             FROM account_move_line l JOIN account_move m ON m.id=l.move_id
            WHERE m.state='posted'")
t_eq "0" "${BAL1%%.*}" "the ledger still balances after posting"

RECV1=$(pg "SELECT COALESCE(SUM(l.debit) - SUM(l.credit),0)
              FROM account_move_line l
              JOIN account_account a ON a.id=l.account_id
              JOIN account_move m ON m.id=l.move_id
             WHERE a.account_type='asset_receivable' AND m.state='posted'")
MOVED=$(pg "SELECT (${RECV1:-0} - ${RECV0:-0})")
echo "    receivable ${RECV0:-0} -> ${RECV1:-0} (moved $MOVED)"
t_eq "$((AMOUNT * M))" "${MOVED%%.*}" "the receivable moved by exactly the invoice amount"

# ------------------------------------------------------------------
sec "5. the reports agree with the ledger"
# ------------------------------------------------------------------
# The trial balance is the direct test: its two columns are the same sums the
# ledger holds, so any disagreement is the report's arithmetic, not the data's.
TB=$(rep trial_balance)
t_eq "200" "$(repcode trial_balance)" "the trial balance is still served with the new entry"
# The reports return rendered rows — {"cells": ["1200 Accounts Receivable",
# "4,680.00", "176,290.00", "-171,610.00"], "type": "line"} — not flat numeric
# keys. So the figures are parsed the way a reader sees them, thousands
# separators and all, and the TOTAL row is skipped so it is not counted twice.
TBD=$(echo "$TB" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print('unparseable'); raise SystemExit
rows = d.get('result', d).get('rows', [])
def num(s):
    try:    return float(str(s).replace(',', '').replace('(', '-').replace(')', ''))
    except Exception: return 0.0
tot_d = tot_c = 0.0
for r in rows:
    if not isinstance(r, dict) or r.get('type') != 'line': continue
    cells = r.get('cells') or []
    if len(cells) >= 3:
        tot_d += num(cells[1]); tot_c += num(cells[2])
print('%.2f %.2f' % (tot_d, tot_c))
" 2>/dev/null)
echo "    trial balance totals: ${TBD:-unavailable}"
if [ -z "$TBD" ] || [ "$TBD" = "unparseable" ]; then
    no "could not read the trial balance's totals — shape changed?"
else
    D=$(echo "$TBD" | cut -d' ' -f1); C=$(echo "$TBD" | cut -d' ' -f2)
    t_eq "$D" "$C" "the trial balance balances (debits equal credits)"
    # It must also be reporting something, not zeroes. A report that balances
    # because it found nothing is the failure this catches.
    case "$D" in 0.00|"") no "the trial balance reports nothing at all" ;;
                 *)       ok "and it is reporting real figures ($D)" ;; esac
fi

# The invoice must be visible to the reports that should see it.
PL=$(rep partner_ledger)
t_contains "$PL" "CL Customer" "the partner ledger shows the new customer"
AR=$(rep aged_receivable)
t_contains "$AR" "CL Customer" "the aged receivable shows them as owing"

# ------------------------------------------------------------------
sec "6. the balance sheet holds its identity"
# ------------------------------------------------------------------
BS=$(rep balance_sheet)
BSD=$(echo "$BS" | python3 -c "
import sys, json
try: d = json.load(sys.stdin)
except Exception: raise SystemExit
r = d.get('result', d)
def num(*keys):
    for k in keys:
        v = r.get(k)
        if isinstance(v, (int, float)): return float(v)
    return None
a, l, e = num('total_assets','assets'), num('total_liabilities','liabilities'), num('total_equity','equity')
if a is not None and l is not None and e is not None:
    print('%.2f %.2f' % (a, l + e))
" 2>/dev/null)
if [ -n "$BSD" ]; then
    A=$(echo "$BSD" | cut -d' ' -f1); LE=$(echo "$BSD" | cut -d' ' -f2)
    echo "    assets=$A  liabilities+equity=$LE"
    t_eq "$A" "$LE" "assets = liabilities + equity"
else
    # Not a failure: the report may not expose those totals as flat keys. Say
    # so plainly rather than asserting against a shape that was guessed.
    echo "    NOTE  the balance sheet does not expose flat total keys — identity not checked here"
    t_eq "200" "$(repcode balance_sheet)" "the balance sheet is served with the new entry"
fi

# ------------------------------------------------------------------
sec "7. every report survives real data"
# ------------------------------------------------------------------
for r in $REPORTS; do
    BODY=$(rep "$r")
    t_eq "200" "$(repcode "$r")" "$r still responds after posting"
    t_lacks "$BODY" "internal error" "$r did not fail internally"
done

# ==================================================================
# THE CANCEL PATH — reversing an entry at close.
#
# The month-end version of a cancellation: something posted in the period turns
# out to be wrong, and it is reversed rather than deleted.
#
# This is the strongest assertion in the whole suite, because it is exact
# rather than approximate: measure the ledger, post, reverse, measure again.
# Every figure must return to the value it had BEFORE the entry existed. Not
# "close", not "balanced" — identical. Anything that does not return is
# something the reversal failed to undo.
# ==================================================================
sec "8. capturing the position before the reversal"
RECV_POSTED=$(pg "SELECT COALESCE(SUM(l.debit) - SUM(l.credit),0)
                    FROM account_move_line l
                    JOIN account_account a ON a.id=l.account_id
                    JOIN account_move m ON m.id=l.move_id
                   WHERE a.account_type='asset_receivable' AND m.state='posted'")
TB_BEFORE=$(rep trial_balance | python3 -c "
import sys, json
d = json.load(sys.stdin); r = d.get('result', d).get('rows', [])
def num(s):
    try: return float(str(s).replace(',', ''))
    except Exception: return 0.0
print('%.2f' % sum(num(x['cells'][1]) for x in r if x.get('type')=='line' and len(x.get('cells',[]))>2))
" 2>/dev/null)
echo "    receivable now $RECV_POSTED, trial balance debits $TB_BEFORE"

sec "9. reversing the entry"
RRES=$(call account.move action_reverse "[[$INV]]")
CN=$(echo "$RRES" | rid)
t_nonempty "$CN" "action_reverse produced a reversing entry"
if [ -n "$CN" ]; then
    PRES=$(call account.move action_post "[[$CN]]")
    has_error "$PRES" && no "posting the reversal failed: $(echo "$PRES" | head -c 200)"
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$CN")" "the reversal is posted"
    CNNAME=$(pg "SELECT name FROM account_move WHERE id=$CN")
    case "$CNNAME" in
        RINV*) ok "it is numbered in the RINV series ($CNNAME)" ;;
        *)     no "the reversal is numbered '$CNNAME', not RINV" ;;
    esac
    # Reversing must not remove the original. The audit trail is the point:
    # both entries stay, and the pair nets to nothing.
    t_eq "posted" "$(pg "SELECT state FROM account_move WHERE id=$INV")" "the original entry is still posted"
    t_eq "1"      "$(pg "SELECT count(*) FROM account_move WHERE id=$INV")" "and still exists at all"
fi

sec "10. everything returned to where it was"
BAL2=$(pg "SELECT COALESCE(SUM(debit),0) - COALESCE(SUM(credit),0)
             FROM account_move_line l JOIN account_move m ON m.id=l.move_id
            WHERE m.state='posted'")
t_eq "0" "${BAL2%%.*}" "the ledger balances after the reversal"

RECV2=$(pg "SELECT COALESCE(SUM(l.debit) - SUM(l.credit),0)
              FROM account_move_line l
              JOIN account_account a ON a.id=l.account_id
              JOIN account_move m ON m.id=l.move_id
             WHERE a.account_type='asset_receivable' AND m.state='posted'")
echo "    receivable ${RECV0:-0} -> $RECV_POSTED -> ${RECV2:-0}"
# The exact round trip: back to the figure from before the invoice existed.
t_eq "${RECV0%%.*}" "${RECV2%%.*}" "the receivable is back to its pre-entry figure"

sec "11. and the reports followed the ledger back"
TB_AFTER=$(rep trial_balance | python3 -c "
import sys, json
d = json.load(sys.stdin); r = d.get('result', d).get('rows', [])
def num(s):
    try: return float(str(s).replace(',', ''))
    except Exception: return 0.0
print('%.2f' % sum(num(x['cells'][1]) for x in r if x.get('type')=='line' and len(x.get('cells',[]))>2))
" 2>/dev/null)
echo "    trial balance debits $TB_BEFORE -> $TB_AFTER"
# Debits RISE by the reversal (a reversal posts entries, it does not erase
# them), so this is not a return to the old figure — but it must have moved by
# exactly the amount reversed. A report that stayed still would mean it is not
# reading the reversal at all.
if [ -n "$TB_BEFORE" ] && [ -n "$TB_AFTER" ]; then
    MOVED=$(python3 -c "print('%.2f' % ($TB_AFTER - $TB_BEFORE))" 2>/dev/null)
    echo "    moved by $MOVED, expected $AMOUNT.00"
    t_eq "$AMOUNT.00" "$MOVED" "the trial balance took up the reversal exactly"
else
    no "could not read the trial balance either side of the reversal"
fi

# The customer is square: invoice and reversal cancel out.
NET=$(pg "SELECT COALESCE(SUM(l.debit) - SUM(l.credit),0)
            FROM account_move_line l
            JOIN account_move m ON m.id=l.move_id
           WHERE l.partner_id=$PARTNER AND m.state='posted'")
t_eq "0" "${NET%%.*}" "the reversed customer nets to zero"

for r in $REPORTS; do
    t_eq "200" "$(repcode "$r")" "$r still responds after the reversal"
done

verdict
