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
# Fixed Assets (docs/084): register, straight-line schedule, and posting the
# depreciation entries. The assertions are the accounting ones:
#   * confirm builds N schedule lines that sum to the asset's gross value
#   * depreciating posts one balanced entry per due line (Dr expense / Cr accum)
#   * the asset's book value drops by exactly the depreciation posted
#   * the ledger stays balanced throughout
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
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }

EXP=$(pg "SELECT id FROM account_account WHERE account_type='expense' ORDER BY id LIMIT 1")
DEPR=$(pg "SELECT id FROM account_account WHERE account_type='liability_current' ORDER BY id LIMIT 1")
ASSETACC=$(pg "SELECT id FROM account_account WHERE account_type LIKE 'asset%' ORDER BY id LIMIT 1")
JMISC=$(pg "SELECT id FROM account_journal WHERE type='general' ORDER BY id LIMIT 1")
[ -z "$JMISC" ] && JMISC=$(pg "SELECT id FROM account_journal ORDER BY id LIMIT 1")

echo "############ asset type + asset ############"
AT=$(call account.asset.type create "[{\"name\":\"Office Equipment\",\"number\":12,\"period_months\":1,\"account_asset_id\":$ASSETACC,\"account_depreciation_id\":$DEPR,\"account_expense_id\":$EXP,\"journal_id\":$JMISC}]" "{$CTX}" | rid)
[ -n "$AT" ] && ok "asset type created ($AT)" || no "asset type failed"
AS=$(call account.asset create "[{\"name\":\"Laptop fleet\",\"value\":12000,\"number\":12,\"period_months\":1,\"acquisition_date\":\"2026-01-01\",\"asset_type_id\":$AT}]" "{$CTX}" | rid)
[ -n "$AS" ] && ok "asset created ($AS)" || { no "asset create failed"; echo "*** FAILURES ***"; exit 1; }

echo "############ confirm builds the straight-line schedule ############"
call account.asset action_confirm "[[$AS]]" "{$CTX}" >/dev/null
NL=$(pg "SELECT count(*) FROM account_asset_depreciation_line WHERE asset_id=$AS")
SUM=$(pg "SELECT COALESCE(SUM(amount),0) FROM account_asset_depreciation_line WHERE asset_id=$AS")
[ "$NL" = "12" ] && ok "12 depreciation lines generated" || no "line count = $NL (expected 12)"
[ "$SUM" = "12000000000" ] && ok "schedule sums to the RM12,000 gross value" || no "schedule sum = $SUM (expected 12000000000)"
[ "$(pg "SELECT state FROM account_asset WHERE id=$AS")" = "open" ] && ok "asset is now running (open)" || no "asset not open"
# accounts inherited from the type
[ "$(pg "SELECT account_expense_id FROM account_asset WHERE id=$AS")" = "$EXP" ] && ok "expense account inherited from the type" || no "accounts not inherited"

echo "############ post depreciation entries due by 2026-06-30 ############"
BAL0=$(pg "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted'")
call account.asset action_depreciate "[[$AS]]" "{\"date\":\"2026-06-30\",$CTX}" >/dev/null
POSTED=$(pg "SELECT count(*) FROM account_asset_depreciation_line WHERE asset_id=$AS AND posted=TRUE")
[ "$POSTED" -ge 1 ] && ok "$POSTED depreciation entries posted" || no "nothing posted"
# every generated depreciation move balances
UNBAL=$(pg "SELECT count(*) FROM (SELECT l.move_id FROM account_move_line l JOIN account_asset_depreciation_line d ON d.move_id=l.move_id WHERE d.asset_id=$AS GROUP BY l.move_id HAVING SUM(l.debit)<>SUM(l.credit)) x")
[ "$UNBAL" = "0" ] && ok "every depreciation entry balances (Dr expense == Cr accumulated)" || no "$UNBAL unbalanced entries"
# book value dropped by exactly the depreciation posted
EXPECT_RES=$(pg "SELECT 12000000000 - COALESCE(SUM(amount),0) FROM account_asset_depreciation_line WHERE asset_id=$AS AND posted=TRUE")
RES=$(pg "SELECT value_residual FROM account_asset WHERE id=$AS")
[ "$RES" = "$EXPECT_RES" ] && ok "book value = gross − depreciation posted ($RES)" || no "book value $RES != expected $EXPECT_RES"
# ledger still balanced
BAL1=$(pg "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted'")
[ "$BAL1" = "0" ] && ok "ledger still balances after depreciation" || no "ledger unbalanced by $BAL1"

echo "############ idempotent: re-running posts no duplicates ############"
call account.asset action_depreciate "[[$AS]]" "{\"date\":\"2026-06-30\",$CTX}" >/dev/null
[ "$(pg "SELECT count(*) FROM account_asset_depreciation_line WHERE asset_id=$AS AND posted=TRUE")" = "$POSTED" ] && ok "re-running to the same date posts nothing new" || no "duplicate entries created"

echo "############ housekeeping ############"
# 17 identical "Laptop fleet" assets had accumulated, one per run, each with a
# posted depreciation entry behind it (docs/092). The journal entries must go
# with the asset — they are posted, and orphaned depreciation would silently
# distort the P&L this suite's own report tests read.
pg "DELETE FROM account_move_line WHERE move_id IN (
        SELECT move_id FROM account_asset_depreciation_line
         WHERE move_id IS NOT NULL
           AND asset_id IN (SELECT id FROM account_asset WHERE name = 'Laptop fleet'))" >/dev/null
pg "DELETE FROM account_move WHERE id IN (
        SELECT move_id FROM account_asset_depreciation_line
         WHERE move_id IS NOT NULL
           AND asset_id IN (SELECT id FROM account_asset WHERE name = 'Laptop fleet'))" >/dev/null
pg "DELETE FROM account_asset_depreciation_line WHERE asset_id IN (SELECT id FROM account_asset WHERE name = 'Laptop fleet')" >/dev/null
pg "DELETE FROM account_asset WHERE name = 'Laptop fleet'" >/dev/null
[ "$(pg "SELECT count(*) FROM account_asset WHERE name = 'Laptop fleet'")" = "0" ] \
    && ok "test assets and their depreciation entries removed" || no "test assets left behind"
BAL2=$(pg "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_move_line l JOIN account_move m ON m.id=l.move_id WHERE m.state='posted'")
[ "$BAL2" = "0" ] && ok "ledger still balances after the cleanup" || no "cleanup unbalanced the ledger by $BAL2"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
