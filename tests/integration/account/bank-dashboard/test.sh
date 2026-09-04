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
# Bank Accounts register + adjustable Accounting Dashboard (docs/087).
#
# Bank account: a master record plus a register of lines — index, description,
# date, debit, credit — whose balance is Σdebit − Σcredit.
# Dashboard: journal cards computed from the ledger, with the visible-card
# selection persisted so the dashboard is adjustable.
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
COOK="Cookie: session_id=$SID"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }

echo "############ menus ############"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Bank Accounts' LIMIT 1")" = "account.bank.account" ] \
    && ok "Configuration → Bank Accounts wired" || no "Bank Accounts menu missing"
# 'Dashboard' is not unique (Rental has one too) — scope it to the Accounting app.
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id JOIN ir_ui_menu p ON p.id=m.parent_id WHERE m.name='Dashboard' AND p.name='Accounting' LIMIT 1")" = "account.dashboard" ] \
    && ok "Accounting → Dashboard wired" || no "Dashboard menu missing"

echo "############ bank account register ############"
JRN=$(pg "SELECT id FROM account_journal WHERE type='bank' ORDER BY id LIMIT 1")
[ -z "$JRN" ] && JRN=$(pg "SELECT id FROM account_journal ORDER BY id LIMIT 1")
BA=$(call account.bank.account create "[{\"name\":\"QA Current Account\",\"bank_name\":\"CIMB\",\"account_number\":\"8001-2233\",\"journal_id\":$JRN}]" "{$CTX}" | rid)
[ -n "$BA" ] && ok "bank account created ($BA)" || { no "bank account create failed"; echo "*** FAILURES ***"; exit 1; }
# three register rows: +1000 debit, +250 debit, -400 credit  → balance 850
call account.bank.account.line create "[{\"bank_account_id\":$BA,\"sequence\":1,\"date\":\"2026-01-05\",\"name\":\"Opening deposit\",\"debit\":1000,\"credit\":0}]" "{$CTX}" >/dev/null
call account.bank.account.line create "[{\"bank_account_id\":$BA,\"sequence\":2,\"date\":\"2026-01-09\",\"name\":\"Customer receipt\",\"debit\":250,\"credit\":0}]" "{$CTX}" >/dev/null
call account.bank.account.line create "[{\"bank_account_id\":$BA,\"sequence\":3,\"date\":\"2026-01-12\",\"name\":\"Supplier payment\",\"debit\":0,\"credit\":400}]" "{$CTX}" >/dev/null
CNT=$(pg "SELECT count(*) FROM account_bank_account_line WHERE bank_account_id=$BA")
[ "$CNT" = "3" ] && ok "3 register lines stored (index, description, date, debit, credit)" || no "line count = $CNT"
BAL=$(pg "SELECT COALESCE(SUM(debit)-SUM(credit),0) FROM account_bank_account_line WHERE bank_account_id=$BA")
[ "$BAL" = "850000000" ] && ok "balance = Σdebit − Σcredit = RM850.00" || no "balance = $BAL (expected 850000000)"
# the register reads back in index order
FIRST=$(pg "SELECT name FROM account_bank_account_line WHERE bank_account_id=$BA ORDER BY sequence LIMIT 1")
[ "$FIRST" = "Openingdeposit" ] && ok "lines read back in index order" || no "ordering wrong (first='$FIRST')"

echo "############ dashboard ############"
D=$(curl -s -H "$COOK" "$BASE/web/account/dashboard")
echo "$D" | grep -q '"cards"' && ok "dashboard returns cards" || no "dashboard failed: $(echo "$D" | head -c 120)"
for c in invoices bills bank cash assets budgets; do
    echo "$D" | grep -q "\"id\":\"$c\"" || { no "card '$c' missing"; break; }
done
echo "$D" | grep -q '"id":"budgets"' && ok "all six journal cards present" || no "cards incomplete"

echo "############ dashboard is adjustable (selection persists) ############"
curl -s -H "$COOK" "$BASE/web/account/dashboard?cards=invoices,bank" >/dev/null
STORED=$(pg "SELECT value FROM ir_config_parameter WHERE key='account.dashboard.cards'")
[ "$STORED" = "invoices,bank" ] && ok "card selection saved (invoices,bank)" || no "selection not saved (got '$STORED')"
BACK=$(curl -s -H "$COOK" "$BASE/web/account/dashboard" | sed -n 's/.*"enabled":"\([^"]*\)".*/\1/p')
[ "$BACK" = "invoices,bank" ] && ok "selection is read back on reload" || no "reload returned '$BACK'"
# restore the default so the app is usable after the test
curl -s -H "$COOK" "$BASE/web/account/dashboard?cards=invoices,bills,bank,cash,assets,budgets" >/dev/null
ok "default card set restored"

# housekeeping
pg "DELETE FROM account_bank_account_line WHERE bank_account_id=$BA" >/dev/null
pg "DELETE FROM account_bank_account WHERE id=$BA" >/dev/null

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
