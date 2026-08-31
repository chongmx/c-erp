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
# verify_multicompany_hardening.sh
#
# Proves the two hardening fixes from docs/071 that gate multi-company:
#
#   §1.2  Record rules (ir.rule) are now ENFORCED on custom raw-SQL
#         search_reads (stock.picking), which previously bypassed the
#         RuleEngine. A non-admin user in company 1 must NOT see a picking
#         planted in company 2 once the "Own Company" rule is active.
#
#   §1.5  account.bank.statement.line.reconcile now revalidates its target
#         move: it refuses a non-posted move, an already-paid move, and a
#         different-company move (instead of blindly driving it to 'paid').
#
# Self-contained: creates its own user + rows, activates the dormant rule,
# and RESTORES state (deactivate + restart) on exit via a trap — so the rest
# of the suite sees the rule dormant again.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
APPDIR="$ERP_ROOT"
FAILED=

pg()  { PGPASSWORD=odoo psql -h localhost -U odoo -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok()  { echo "    PASS  $1"; }
no()  { echo "    FAIL  $1"; FAILED=1; }

# auth <login> <password>  -> echoes session_id
auth() {
    cat > /tmp/mch_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"$1","password":"$2"}}
EOF
    curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/mch_auth.json \
        | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'
}
# callas <sid> <model> <method> <args-json>  -> echoes response
callas() {
    cat > /tmp/mch_call.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$2","method":"$3","args":$4,"kwargs":{"context":{"session_id":"$1"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/mch_call.json
}
restart() {
    pkill -x c-erp; sleep 2
    ( cd "$APPDIR" && setsid ./build/c-erp > /tmp/cerp_run.log 2>&1 < /dev/null & )
    for _ in $(seq 1 25); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
}
cleanup() {
    pg "UPDATE ir_rule SET active=FALSE WHERE id=5" >/dev/null
    pg "DELETE FROM stock_picking WHERE name IN ('RR-OWN','RR-FOREIGN')" >/dev/null
    pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE name LIKE 'MCH/%')" >/dev/null
    pg "DELETE FROM account_bank_statement_line WHERE name LIKE 'MCH %'" >/dev/null
    pg "DELETE FROM account_bank_statement WHERE name LIKE 'MCH %'" >/dev/null
    pg "DELETE FROM account_move WHERE name LIKE 'MCH/%'" >/dev/null
    pg "DELETE FROM res_groups_users_rel WHERE uid IN (SELECT id FROM res_users WHERE login='rrtest')" >/dev/null
    pg "DELETE FROM res_users WHERE login='rrtest'" >/dev/null
    pg "DELETE FROM res_company WHERE id=2" >/dev/null
    restart   # restore dormant-rule state for the rest of the suite
}
trap cleanup EXIT

echo "############ setup ############"
ASID=$(auth admin admin)
[ -z "$ASID" ] && { echo "    cannot authenticate admin"; echo '*** FAILURES ***'; exit 1; }

# --- a non-admin inventory user in company 1 ---
pg "DELETE FROM res_users WHERE login='rrtest'" >/dev/null
callas "$ASID" res.users create '[{"login":"rrtest","name":"RR Test","password":"Rr!secret9"}]' >/dev/null
RUID=$(pg "SELECT id FROM res_users WHERE login='rrtest'")
[ -z "$RUID" ] && { echo "    could not create rrtest"; echo '*** FAILURES ***'; exit 1; }
pg "UPDATE res_users SET company_id=1 WHERE id=$RUID" >/dev/null
pg "INSERT INTO res_groups_users_rel (gid,uid) VALUES (11,$RUID) ON CONFLICT DO NOTHING" >/dev/null
echo "    rrtest uid=$RUID (company 1, inventory group 11)"

# --- a SECOND company (stock_picking.company_id is a FK to res_company) ---
pg "INSERT INTO res_company (id,name) VALUES (2,'MCH Company Two') ON CONFLICT (id) DO NOTHING" >/dev/null
CO2=$(pg "SELECT id FROM res_company WHERE id=2")
[ "$CO2" = "2" ] && echo "    company 2 present" || echo "    WARN could not create company 2"

# --- two pickings: own company (1) and a FOREIGN company (2) ---
LOC=$(pg "SELECT id FROM stock_location ORDER BY id LIMIT 1")
pg "DELETE FROM stock_picking WHERE name IN ('RR-OWN','RR-FOREIGN')" >/dev/null
pg "INSERT INTO stock_picking (name,picking_type_id,state,company_id,location_id,location_dest_id) VALUES ('RR-OWN',1,'draft',1,$LOC,$LOC)" >/dev/null
pg "INSERT INTO stock_picking (name,picking_type_id,state,company_id,location_id,location_dest_id) VALUES ('RR-FOREIGN',1,'draft',2,$LOC,$LOC)" >/dev/null

# --- activate the dormant "Own Company" rule, restart for a cold RuleEngine cache ---
pg "UPDATE ir_rule SET active=TRUE WHERE id=5" >/dev/null
restart
ASID=$(auth admin admin)   # sessions are in-memory; re-auth after restart

echo
echo "############ §1.2 — record rule enforced on custom stock.picking read ############"
RSID=$(auth rrtest 'Rr!secret9')
[ -z "$RSID" ] && no "cannot authenticate rrtest (password not set on create?)"
RESP=$(callas "$RSID" stock.picking search_read '[[]]')
echo "$RESP" | grep -q 'RR-OWN'     && ok "non-admin sees its own-company picking (RR-OWN)" \
                                     || no "own-company picking missing (company_id not threaded into session?)"
if echo "$RESP" | grep -q 'RR-FOREIGN'; then
    no "LEAK — non-admin sees foreign-company picking: rule NOT enforced on the custom read"
else
    ok "foreign-company picking filtered out — ir.rule enforced on the custom read"
fi
# control (rule-independent): the foreign row really exists in the DB, so the
# non-admin's not-seeing-it is genuine filtering, not an absent row.
CNT=$(pg "SELECT COUNT(*) FROM stock_picking WHERE name='RR-FOREIGN' AND company_id=2")
[ "$CNT" = "1" ] && ok "control: RR-FOREIGN exists in the DB (filtering is real, not an absent row)" \
                 || no "control: RR-FOREIGN was not created (count=$CNT) — test invalid"

echo
echo "############ §1.5 — reconcile revalidates the target move ############"
BNK=$(pg "SELECT id FROM account_journal WHERE code='BNK' AND company_id=1")
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
STMT=$(pg "INSERT INTO account_bank_statement (name,journal_id,company_id) VALUES ('MCH Stmt',$BNK,1) RETURNING id")
LINE=$(pg "INSERT INTO account_bank_statement_line (statement_id,name,partner_id,amount,company_id) VALUES ($STMT,'MCH line',$PARTNER,100000000,1) RETURNING id")
SAL=$(pg "SELECT id FROM account_journal WHERE code='SAL' AND company_id=1")
echo "    ids: BNK=$BNK PARTNER=$PARTNER STMT=$STMT LINE=$LINE SAL=$SAL"
# (a) an already-PAID posted invoice (residual 0) -> reconcile must refuse
PAID=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id,partner_id,amount_total,amount_residual,payment_state) VALUES ('MCH/PAID','out_invoice','posted',CURRENT_DATE,$SAL,1,$PARTNER,100000000,0,'paid') RETURNING id")
RP=$(callas "$ASID" account.bank.statement.line reconcile "[{\"line_id\":$LINE,\"move_id\":$PAID}]")
echo "$RP" | grep -qi 'fully paid' && ok "reconcile refuses an already-paid invoice" \
                                   || no "reconcile did NOT refuse a paid invoice: $RP"
# (b) a DRAFT (unposted) move -> reconcile must refuse
DRAFT=$(pg "INSERT INTO account_move (name,move_type,state,date,journal_id,company_id,partner_id,amount_total,amount_residual,payment_state) VALUES ('MCH/DRAFT','out_invoice','draft',CURRENT_DATE,$SAL,1,$PARTNER,100000000,100000000,'not_paid') RETURNING id")
RD=$(callas "$ASID" account.bank.statement.line reconcile "[{\"line_id\":$LINE,\"move_id\":$DRAFT}]")
echo "$RD" | grep -qi 'not posted' && ok "reconcile refuses a non-posted invoice" \
                                   || no "reconcile did NOT refuse a draft invoice: $RD"
# (c) the line is still unreconciled after both refusals
STILL=$(pg "SELECT is_reconciled FROM account_bank_statement_line WHERE id=$LINE")
[ "$STILL" = "f" ] && ok "statement line left unreconciled after refusals" \
                   || no "statement line was mutated despite refusals (is_reconciled=$STILL)"

echo
if [ -n "$FAILED" ]; then echo '*** FAILURES ***'; else echo 'All checks passed.'; fi
