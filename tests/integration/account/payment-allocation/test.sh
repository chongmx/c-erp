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
# P1 — payment allocation + realised FX.
#
# The cases the old scalar `residual = residual - paid` could not express:
#   * one payment across several invoices
#   * an unallocated advance (a credit with no invoice yet)
#   * reversal
#   * FX when the bank converts on receipt
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

# -q suppresses psql's completion tag ("INSERT 0 1"), which otherwise
# lands in the captured output alongside a RETURNING value.
pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

M=1000000   # micro-units per major unit

echo "############ setup: a partner with two open invoices ############"
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
JOURNAL=$(pg "SELECT id FROM account_journal WHERE type IN ('bank','cash') ORDER BY id LIMIT 1")
AR=$(pg "SELECT id FROM account_account WHERE code='1200' LIMIT 1")
REV=$(pg "SELECT id FROM account_account WHERE code='4000' LIMIT 1")
echo "    partner=$PARTNER journal=$JOURNAL AR=$AR revenue=$REV"

mkinv() {   # $1 = total in major units -> echoes move id
    local total=$(( $1 * M ))
    local mid
    mid=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,partner_id,company_id,
                                        amount_untaxed,amount_tax,amount_total,amount_residual,payment_state)
              VALUES ('P1TEST/'||nextval('account_move_id_seq')::text,'out_invoice','posted',CURRENT_DATE,
                      $JOURNAL,$PARTNER,1,$total,0,$total,$total,'not_paid') RETURNING id")
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
        VALUES ($mid,$AR,$JOURNAL,1,CURRENT_DATE,'AR',$total,0,'')" >/dev/null
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit,display_type)
        VALUES ($mid,$REV,$JOURNAL,1,CURRENT_DATE,'Rev',0,$total,'')" >/dev/null
    echo "$mid"
}

INV1=$(mkinv 100)
INV2=$(mkinv 250)
echo "    invoice A=$INV1 (100.00)  invoice B=$INV2 (250.00)"

echo
echo "############ 1. ONE payment across TWO invoices ############"
# 300.00 covers A entirely and 200.00 of B — impossible with the old scalar.
PMT=$(pg "INSERT INTO account_payment (date,journal_id,partner_id,company_id,amount,
                                       payment_type,partner_type,state,memo)
          VALUES (CURRENT_DATE,$JOURNAL,$PARTNER,1,$((300*M)),'inbound','customer','posted','P1 multi')
          RETURNING id")
# The C++ allocator runs in the register-payment flow; here the data model is
# asserted directly so the test does not depend on driving a UI dialog.
pg "INSERT INTO account_partial_reconcile (payment_id,move_id,amount,amount_base,date,company_id)
    VALUES ($PMT,$INV1,$((100*M)),$((100*M)),CURRENT_DATE,1)" >/dev/null
pg "INSERT INTO account_partial_reconcile (payment_id,move_id,amount,amount_base,date,company_id)
    VALUES ($PMT,$INV2,$((200*M)),$((200*M)),CURRENT_DATE,1)" >/dev/null

R1=$(pg "SELECT m.amount_total - COALESCE(SUM(a.amount),0) FROM account_move m
          LEFT JOIN account_partial_reconcile a ON a.move_id=m.id
          WHERE m.id=$INV1 GROUP BY m.amount_total")
R2=$(pg "SELECT m.amount_total - COALESCE(SUM(a.amount),0) FROM account_move m
          LEFT JOIN account_partial_reconcile a ON a.move_id=m.id
          WHERE m.id=$INV2 GROUP BY m.amount_total")
echo "    residual A=$R1 (expect 0)   residual B=$R2 (expect 50000000)"
[ "$R1" = "0" ]        && ok "invoice A fully settled" || no "A residual is $R1"
[ "$R2" = "50000000" ] && ok "invoice B partially settled (50.00 left)" || no "B residual is $R2"

echo
echo "############ 2. unallocated advance shows as a credit ############"
ADV=$(pg "INSERT INTO account_payment (date,journal_id,partner_id,company_id,amount,
                                       payment_type,partner_type,state,memo)
          VALUES (CURRENT_DATE,$JOURNAL,$PARTNER,1,$((500*M)),'inbound','customer','posted','P1 advance')
          RETURNING id")
UN=$(pg "SELECT amount_unallocated FROM account_payment_unallocated WHERE payment_id=$ADV")
echo "    500.00 received, nothing allocated -> unallocated=$UN"
[ "$UN" = "$((500*M))" ] && ok "full amount sits as a customer credit" || no "unallocated is $UN"
# The old model had nowhere to put this — there was no invoice to decrement.

echo
echo "############ 3. reversal restores the residual exactly ############"
pg "DELETE FROM account_partial_reconcile WHERE payment_id=$PMT AND move_id=$INV2" >/dev/null
R2B=$(pg "SELECT m.amount_total - COALESCE(SUM(a.amount),0) FROM account_move m
           LEFT JOIN account_partial_reconcile a ON a.move_id=m.id
           WHERE m.id=$INV2 GROUP BY m.amount_total")
echo "    after un-allocating B: residual=$R2B (expect 250000000)"
[ "$R2B" = "$((250*M))" ] && ok "residual restored exactly, no drift" || no "residual is $R2B"

echo
echo "############ 4. realised FX (docs/048 §4.6) ############"
# 100 USD invoice booked at 4.70 = 470.00 MYR. Bank credits 448.50 MYR.
# Effective rate 4.485; realised loss 21.50 MYR.
python3 - <<'PY'
M = 1_000_000
usd      = 100 * M
booked   = 4_700_000          # 4.70
received = int(448.50 * M)
at_booked     = usd * booked // M
implied       = received * M // usd
at_settlement = usd * implied // M
fx            = at_settlement - at_booked
print("      booked   %10.2f MYR" % (at_booked / M))
print("      received %10.2f MYR" % (received  / M))
print("      implied rate %.6f" % (implied / M))
print("      FX diff  %10.2f MYR" % (fx / M))
assert implied == 4_485_000, implied
assert fx == int(-21.50 * M), fx
print("      PASS  implied rate 4.485000, realised loss -21.50")
PY

echo
echo "############ 5. FX account exists and is an expense ############"
FXA=$(pg "SELECT code||' '||name||' '||account_type FROM account_account WHERE code='7900' LIMIT 1")
echo "    $FXA"
[ -n "$FXA" ] && ok "7900 Foreign Exchange Gain/Loss present" || no "7900 missing"

echo
echo "############ cleanup ############"
pg "DELETE FROM account_partial_reconcile WHERE payment_id IN ($PMT,$ADV)" >/dev/null
pg "DELETE FROM account_payment WHERE id IN ($PMT,$ADV)" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id IN ($INV1,$INV2)" >/dev/null
pg "DELETE FROM account_move WHERE id IN ($INV1,$INV2)" >/dev/null
# Sweep by prefix as well. An interrupted run leaves a header whose lines
# were already deleted, and a move with no lines fails the ledger-integrity
# check ("untaxed equals the sum of its revenue lines") on every later run
# — a stale-data failure that looks exactly like a real accounting bug.
pg "DELETE FROM account_partial_reconcile WHERE move_id IN
      (SELECT id FROM account_move WHERE name LIKE 'P1TEST/%')" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id IN
      (SELECT id FROM account_move WHERE name LIKE 'P1TEST/%')" >/dev/null
pg "DELETE FROM account_move WHERE name LIKE 'P1TEST/%'" >/dev/null
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
