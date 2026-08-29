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
# Accounting configuration reference data (docs/086):
#   Currencies · Account Types · Fiscal Positions (+ tax mapping) ·
#   Incoterms · Journal Groups
#
# These are the remaining Odoo Configuration dropdown entries. They render in
# the generic form, so the contract to guard is: the menu resolves to the right
# model, the seeded reference lists are present, and each model round-trips a
# create/read (no "Internal Error" on New).
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

echo "############ Configuration menus resolve to their models ############"
chk(){ [ "$(pg "SELECT a.res_model FROM ir_ui_menu m JOIN ir_act_window a ON a.id=m.action_id WHERE m.id=$1")" = "$2" ] \
       && ok "$3 menu → $2" || no "$3 menu missing/wrong"; }
chk 34 res.currency            "Currencies"
chk 35 account.account.type    "Account Types"
chk 36 account.fiscal.position "Fiscal Positions"
chk 37 account.incoterms       "Incoterms"
chk 38 account.journal.group   "Journal Groups"

echo "############ seeded reference lists ############"
[ "$(pg "SELECT count(*) FROM account_account_type")" -ge 9 ] && ok "account types seeded ($(pg "SELECT count(*) FROM account_account_type") rows)" || no "account types not seeded"
[ "$(pg "SELECT count(*) FROM account_incoterms")" -ge 11 ] && ok "Incoterms seeded ($(pg "SELECT count(*) FROM account_incoterms") rows)" || no "incoterms not seeded"
# every account_type actually used by the chart must exist in the list
MISSING=$(pg "SELECT count(*) FROM (SELECT DISTINCT account_type FROM account_account) a WHERE NOT EXISTS (SELECT 1 FROM account_account_type t WHERE t.code=a.account_type)")
[ "$MISSING" = "0" ] && ok "every account_type used by the chart is in the list" || no "$MISSING account types used by the chart are missing"

echo "############ models round-trip through the API ############"
# Creates the record, asserts it reads back, and leaves the new id in RT_ID.
RT_ID=
rt(){ # model, create-json, label
    RT_ID=$(call "$1" create "[$2]" "{$CTX}" | rid)
    if [ -n "$RT_ID" ]; then
        call "$1" read "[[$RT_ID]]" "{$CTX}" | grep -q '"id"' \
            && ok "$3 creates and reads back (id $RT_ID)" || no "$3 read failed"
    else
        no "$3 create failed"
    fi
}
rt account.account.type  '{"name":"QA Type","code":"qa_type","internal_group":"asset"}' "Account Type"
rt account.incoterms     '{"code":"QA1","name":"QA Incoterm"}'                          "Incoterm"
rt account.journal.group '{"name":"QA Group","journal_ids_json":"[1]"}'                 "Journal Group"
rt account.fiscal.position '{"name":"QA Export (zero-rated)","auto_apply":false}'       "Fiscal Position"
FP=$RT_ID

echo "############ fiscal position tax mapping ############"
T1=$(pg "SELECT id FROM account_tax ORDER BY id LIMIT 1")
T2=$(pg "SELECT id FROM account_tax ORDER BY id DESC LIMIT 1")
if [ -n "$FP" ] && [ -n "$T1" ]; then
    MAP=$(call account.fiscal.position.tax create "[{\"position_id\":$FP,\"tax_src_id\":$T1,\"tax_dest_id\":$T2}]" "{$CTX}" | rid)
    [ -n "$MAP" ] && ok "tax substitution row created (tax $T1 → $T2)" || no "tax mapping create failed"
    [ "$(pg "SELECT count(*) FROM account_fiscal_position_tax WHERE position_id=$FP")" -ge 1 ] \
        && ok "mapping is linked to its fiscal position" || no "mapping not linked"
else
    no "could not set up the tax mapping case"
fi

# housekeeping so the test is repeatable
pg "DELETE FROM account_fiscal_position_tax WHERE position_id IN (SELECT id FROM account_fiscal_position WHERE name LIKE 'QA %')" >/dev/null
pg "DELETE FROM account_fiscal_position WHERE name LIKE 'QA %'" >/dev/null
pg "DELETE FROM account_account_type WHERE code='qa_type'" >/dev/null
pg "DELETE FROM account_incoterms WHERE code='QA1'" >/dev/null
pg "DELETE FROM account_journal_group WHERE name='QA Group'" >/dev/null

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
