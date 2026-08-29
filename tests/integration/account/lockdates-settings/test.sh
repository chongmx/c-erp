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
# Lock dates, Accounting Settings, and the analysis/audit reports (docs/088).
#
# The load-bearing assertion is the lock date: once set, an entry dated on or
# before it must be REFUSED at posting (with a clear message, not a 500), and
# posting must work again once the lock is lifted.
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

echo "############ analysis & audit reports render ############"
for r in aged_payable partner_ledger journals_audit invoice_analysis product_margins; do
    curl -s -H "$COOK" "$BASE/web/account/report?report=$r&date_from=2000-01-01&date_to=2099-12-31" \
        | grep -q "\"report\":\"$r\"" && ok "$r renders" || no "$r failed"
done

echo "############ settings load + save ############"
S=$(curl -s -H "$COOK" "$BASE/web/account/settings")
echo "$S" | grep -q '"values"' && ok "settings load (values, taxes, journals)" || no "settings failed: $(echo "$S"|head -c 100)"
curl -s -H "$COOK" "$BASE/web/account/settings?key=account.tax_periodicity&value=bimonthly" >/dev/null
[ "$(pg "SELECT value FROM ir_config_parameter WHERE key='account.tax_periodicity'")" = "bimonthly" ] \
    && ok "a setting saves (tax periodicity = bimonthly)" || no "setting did not save"
# unknown keys are rejected rather than written (allowlist)
curl -s -H "$COOK" "$BASE/web/account/settings?key=account.evil&value=x" >/dev/null
[ -z "$(pg "SELECT value FROM ir_config_parameter WHERE key='account.evil'")" ] \
    && ok "unknown setting keys are rejected (allowlisted)" || no "an unknown key was written"

echo "############ lock date blocks posting in a closed period ############"
JRN=$(pg "SELECT id FROM account_journal ORDER BY id LIMIT 1")
EXPA=$(pg "SELECT id FROM account_account WHERE account_type='expense' ORDER BY id LIMIT 1")
CASHA=$(pg "SELECT id FROM account_account WHERE account_type LIKE 'asset%' ORDER BY id LIMIT 1")
mkmove(){ # $1=date -> echoes new draft move id
    local mv=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id) VALUES ('/','entry','draft','$1',$JRN,1) RETURNING id" | head -1)
    pg "INSERT INTO account_move_line (move_id,account_id,journal_id,company_id,date,name,debit,credit) VALUES ($mv,$EXPA,$JRN,1,'$1','Lock test',5000000,0), ($mv,$CASHA,$JRN,1,'$1','Lock test',0,5000000)" >/dev/null
    echo "$mv"
}
# lock everything up to 2026-06-30
curl -s -H "$COOK" "$BASE/web/account/settings?key=account.lock_date&value=2026-06-30" >/dev/null
MV_LOCKED=$(mkmove 2026-05-15)
R=$(call account.move action_post "[[$MV_LOCKED]]" "{$CTX}")
echo "$R" | grep -qi 'locked' && ok "posting into the locked period is refused" || no "lock not enforced: $(echo "$R"|head -c 140)"
echo "$R" | grep -q 'ValidationError' && ok "refusal is a clean validation error (not an Internal Error)" || no "wrong error type"
[ "$(pg "SELECT state FROM account_move WHERE id=$MV_LOCKED")" = "draft" ] && ok "the entry stayed draft" || no "entry was posted despite the lock"

# a date AFTER the lock still posts
MV_OK=$(mkmove 2026-08-15)
call account.move action_post "[[$MV_OK]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM account_move WHERE id=$MV_OK")" = "posted" ] && ok "a date after the lock still posts" || no "posting blocked outside the locked period"

# lift the lock -> the previously blocked entry posts
curl -s -H "$COOK" "$BASE/web/account/settings?key=account.lock_date&value=" >/dev/null
call account.move action_post "[[$MV_LOCKED]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT state FROM account_move WHERE id=$MV_LOCKED")" = "posted" ] && ok "lifting the lock lets it post" || no "still blocked after lifting the lock"

# housekeeping: remove the probe entries, leave no lock behind
pg "DELETE FROM account_move_line WHERE move_id IN ($MV_LOCKED,$MV_OK)" >/dev/null
pg "DELETE FROM account_move WHERE id IN ($MV_LOCKED,$MV_OK)" >/dev/null
pg "DELETE FROM ir_config_parameter WHERE key IN ('account.lock_date','account.tax_lock_date')" >/dev/null
ok "probe entries removed and locks cleared"

echo "############ menus ############"
[ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.name='Settings' LIMIT 1")" = "account.settings" ] \
    && ok "Configuration → Settings wired" || no "Settings menu missing"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
