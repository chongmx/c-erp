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
# Multi-company in ONE database — isolation (docs/094).
#
# Two companies share a database. This asserts that nothing crosses between
# them, on every path a record can be reached by.
#
# The interesting assertions are NOT "the list is filtered". A read-side filter
# is the easy half and it is not sufficient: write and unlink address rows by
# id, so a caller who simply GUESSES an id from another company would still
# modify or delete it if only SELECT were filtered. Each of read / search /
# search_count / write / unlink is therefore probed with a known-good id
# belonging to the other company.
#
# The other assertion that matters is that the ADMIN is scoped too. Record
# rules are bypassed for administrators by design (RuleEngine returns early on
# ctx.isAdmin), so company scoping deliberately does not go through ir.rule —
# if it did, the account almost everyone actually uses would see everything and
# the feature would be decorative.
#
# NULL company_id means "shared" (a country, a group-wide product). That is a
# real behaviour, not an oversight, so it is asserted rather than assumed.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

cleanup() {
    pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE ref='QA-MC-LEAK')" >/dev/null
    pg "DELETE FROM account_move WHERE ref='QA-MC-LEAK'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'QA-MC-%'" >/dev/null
    pg "DELETE FROM res_company_users_rel WHERE user_id IN (SELECT id FROM res_users WHERE login='qa_mc_userb')" >/dev/null
    pg "DELETE FROM res_company_users_rel WHERE company_id IN (SELECT id FROM res_company WHERE name='QA-MC Company B')" >/dev/null
    pg "DELETE FROM res_users WHERE login='qa_mc_userb'" >/dev/null
    pg "DELETE FROM res_company WHERE name='QA-MC Company B'" >/dev/null
    pg "DELETE FROM res_partner WHERE name='QA-MC Company B'" >/dev/null
}
cleanup
trap cleanup EXIT

echo "############ existing rows were attributed before a 2nd company existed ############"
# Rows written before multi-company carried company_id NULL, which now means
# "shared with every company" — so a newly created company would have started
# life able to see all of the first company's data. The startup backfill
# attributes them while there is still only one company. Asserted here, BEFORE
# this script creates its second company, because that is the only window in
# which the backfill is allowed to run.
NULLS=$(pg "SELECT COALESCE(SUM(n),0) FROM (
              SELECT count(*) n FROM res_partner     WHERE company_id IS NULL
    UNION ALL SELECT count(*)   FROM product_product WHERE company_id IS NULL
    UNION ALL SELECT count(*)   FROM account_move    WHERE company_id IS NULL
    UNION ALL SELECT count(*)   FROM stock_location  WHERE company_id IS NULL) s")
[ "$NULLS" = "0" ] && ok "no unattributed rows left in the core tables" \
                   || no "$NULLS row(s) still have company_id NULL"
# ...except global sequences, where NULL is load-bearing: it is what makes a
# sequence global, enforced by a partial unique index.
[ "$(pg "SELECT count(*) FROM ir_sequence WHERE company_id IS NULL")" != "0" ] \
    && ok "global ir_sequence rows were deliberately left alone" \
    || no "the backfill swallowed the global sequences"

echo "############ fixture: a second company and a user in it ############"
# The password hash is copied from admin, so user B's password is "admin".
pg "INSERT INTO res_partner (name, is_company, active) VALUES ('QA-MC Company B', true, true)" >/dev/null
PB_PARTNER=$(pg "SELECT id FROM res_partner WHERE name='QA-MC Company B' ORDER BY id DESC LIMIT 1")
pg "INSERT INTO res_company (name, partner_id, currency_id) SELECT 'QA-MC Company B', $PB_PARTNER, currency_id FROM res_company ORDER BY id LIMIT 1" >/dev/null
CB=$(pg "SELECT id FROM res_company WHERE name='QA-MC Company B'")
[ -n "$CB" ] && ok "company B created (id $CB)" || { no "could not create company B"; echo "*** FAILURES ***"; exit 1; }

pg "INSERT INTO res_users (login, password, company_id, active) SELECT 'qa_mc_userb', password, $CB, true FROM res_users WHERE login='admin'" >/dev/null
UB=$(pg "SELECT id FROM res_users WHERE login='qa_mc_userb'")
pg "INSERT INTO res_company_users_rel (company_id, user_id) VALUES ($CB, $UB) ON CONFLICT DO NOTHING" >/dev/null
# user B is a plain internal user, NOT an administrator
pg "INSERT INTO res_groups_users_rel (gid, uid) VALUES (2, $UB) ON CONFLICT DO NOTHING" >/dev/null
[ -n "$UB" ] && ok "user B created (uid $UB), member of company B only" || { no "no user B"; echo "*** FAILURES ***"; exit 1; }

login(){ curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
         --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"$1\",\"password\":\"$2\"}}" \
         | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p'; }
SA=$(login admin admin)
SB=$(login qa_mc_userb admin)
[ -n "$SA" ] && ok "admin logged in (company A)" || no "admin login failed"
[ -n "$SB" ] && ok "user B logged in (company B)" || { no "user B login failed"; echo "*** FAILURES ***"; exit 1; }

# call <session> <model> <method> <args> [kwargs-extra]
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$2\",\"method\":\"$3\",\"args\":$4,\"kwargs\":{\"context\":{\"session_id\":\"$1\"}${5:+,$5}}}}"; }
rid(){ sed -n 's/.*"result":\([0-9][0-9]*\).*/\1/p'; }

echo "############ each company creates a record ############"
PA=$(call "$SA" res.partner create "[{\"name\":\"QA-MC-A-secret\"}]" | rid)
PBID=$(call "$SB" res.partner create "[{\"name\":\"QA-MC-B-secret\"}]" | rid)
[ -n "$PA" ] && ok "admin created a partner in company A (id $PA)" || no "admin create failed"
[ -n "$PBID" ] && ok "user B created a partner in company B (id $PBID)" || no "user B create failed"
[ "$(pg "SELECT company_id FROM res_partner WHERE id=$PA")" = "1" ] \
    && ok "it was stamped with company A automatically" || no "partner A company_id = $(pg "SELECT company_id FROM res_partner WHERE id=$PA")"
[ "$(pg "SELECT company_id FROM res_partner WHERE id=$PBID")" = "$CB" ] \
    && ok "and B's with company B" || no "partner B company_id wrong"

# A deliberately shared record: company_id NULL means every company sees it.
pg "INSERT INTO res_partner (name, active, company_id) VALUES ('QA-MC-shared', true, NULL)" >/dev/null
SH=$(pg "SELECT id FROM res_partner WHERE name='QA-MC-shared'")

echo "############ search / search_read do not cross over ############"
has(){ echo "$2" | grep -q "$1"; }
LA=$(call "$SA" res.partner search_read "[[[\"name\",\"like\",\"QA-MC-\"]]]")
LB=$(call "$SB" res.partner search_read "[[[\"name\",\"like\",\"QA-MC-\"]]]")
has 'QA-MC-A-secret' "$LA" && ok "admin sees its own record"        || no "admin cannot see its own record"
has 'QA-MC-B-secret' "$LA" && no "LEAK: admin sees company B's record" || ok "admin does NOT see company B's record"
has 'QA-MC-B-secret' "$LB" && ok "user B sees its own record"       || no "user B cannot see its own record"
has 'QA-MC-A-secret' "$LB" && no "LEAK: user B sees company A's record" || ok "user B does NOT see company A's record"
has 'QA-MC-shared'   "$LA" && ok "the shared record is visible to A" || no "shared record hidden from A"
has 'QA-MC-shared'   "$LB" && ok "and to B"                          || no "shared record hidden from B"

echo "############ a guessed id is not a way in ############"
R=$(call "$SB" res.partner read "[[$PA]]")
echo "$R" | grep -q 'QA-MC-A-secret' && no "LEAK: read() by id returned another company's row" \
                                     || ok "read() by a known id from company A returns nothing"
R=$(call "$SB" res.partner search_count "[[[\"id\",\"=\",$PA]]]")
echo "$R" | grep -q '"result":0' && ok "search_count by that id is 0" || no "search_count leaked: $R"

call "$SB" res.partner write "[[$PA],{\"name\":\"QA-MC-HACKED\"}]" >/dev/null
[ "$(pg "SELECT name FROM res_partner WHERE id=$PA")" = "QA-MC-A-secret" ] \
    && ok "write() against that id changed nothing" || no "LEAK: user B rewrote company A's record"

call "$SB" res.partner unlink "[[$PA]]" >/dev/null
[ "$(pg "SELECT count(*) FROM res_partner WHERE id=$PA")" = "1" ] \
    && ok "unlink() against that id deleted nothing" || no "LEAK: user B deleted company A's record"

# ...and the same probes in the other direction, because admin bypasses ir.rule.
R=$(call "$SA" res.partner read "[[$PBID]]")
echo "$R" | grep -q 'QA-MC-B-secret' && no "LEAK: admin read company B's row by id" \
                                     || ok "admin cannot read company B's row by id either"
call "$SA" res.partner write "[[$PBID],{\"name\":\"QA-MC-ADMIN-HACKED\"}]" >/dev/null
[ "$(pg "SELECT name FROM res_partner WHERE id=$PBID")" = "QA-MC-B-secret" ] \
    && ok "and cannot write it" || no "LEAK: admin rewrote company B's record"
call "$SA" res.partner unlink "[[$PBID]]" >/dev/null
[ "$(pg "SELECT count(*) FROM res_partner WHERE id=$PBID")" = "1" ] \
    && ok "and cannot delete it" || no "LEAK: admin deleted company B's record"

echo "############ you cannot plant a record in a company you are not in ############"
R=$(call "$SB" res.partner create "[{\"name\":\"QA-MC-planted\",\"company_id\":1}]")
echo "$R" | grep -q 'another company' && ok "create with a foreign company_id is refused" \
                                      || no "create with foreign company_id was allowed: $R"
[ "$(pg "SELECT count(*) FROM res_partner WHERE name='QA-MC-planted'")" = "0" ] \
    && ok "and nothing was written" || no "a planted row exists"

echo "############ the switcher only offers companies you belong to ############"
sw(){ curl -s -X POST "$BASE/web/session/set_active_company" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"company_id\":$2,\"context\":{\"session_id\":\"$1\"}}}"; }
my(){ curl -s -X POST "$BASE/web/session/my_companies" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"context\":{\"session_id\":\"$1\"}}}"; }

R=$(sw "$SB" 1)
echo "$R" | grep -q 'do not have access' && ok "user B cannot switch to company A" || no "user B switched to A: $R"
R=$(my "$SB")
echo "$R" | grep -q "\"id\":1," && no "LEAK: company A is listed for user B" || ok "and company A is not even listed for them"
echo "$R" | grep -q "\"id\":$CB" && ok "their own company is listed" || no "user B's company missing: $R"

echo "############ granting access, then switching, moves what you can see ############"
acc(){ curl -s -X POST "$BASE/web/company/access" -H 'Content-Type: application/json' \
       --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"op\":\"$2\",\"user_id\":${3:-0},\"company_id\":${4:-0},\"context\":{\"session_id\":\"$1\"}}}"; }
R=$(acc "$SA" list)
echo "$R" | grep -q "\"ok\":true" && ok "admin can read the access matrix" || no "access list failed: $R"
R=$(curl -s -X POST "$BASE/web/company/access" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"op\":\"list\",\"context\":{\"session_id\":\"$SB\"}}}")
echo "$R" | grep -q 'administrator access required' && ok "a non-admin cannot" || no "non-admin read the access matrix: $R"

acc "$SA" grant 1 "$CB" >/dev/null
R=$(sw "$SA" "$CB")
echo "$R" | grep -q '"ok":true' && ok "after being granted, admin can switch to company B" || no "switch failed: $R"

LA2=$(call "$SA" res.partner search_read "[[[\"name\",\"like\",\"QA-MC-\"]]]")
has 'QA-MC-B-secret' "$LA2" && ok "and now sees company B's record" || no "does not see B after switching"
has 'QA-MC-A-secret' "$LA2" && no "LEAK: still sees company A's record after switching" \
                            || ok "and no longer sees company A's"
has 'QA-MC-shared'   "$LA2" && ok "the shared record follows into both" || no "shared record lost after switch"

NEW=$(call "$SA" res.partner create "[{\"name\":\"QA-MC-after-switch\"}]" | rid)
[ "$(pg "SELECT company_id FROM res_partner WHERE id=$NEW")" = "$CB" ] \
    && ok "records created after the switch belong to company B" || no "stamped with the wrong company"

R=$(sw "$SA" 1)
echo "$R" | grep -q '"ok":true' && ok "and switching back works" || no "switch back failed: $R"
LA3=$(call "$SA" res.partner search_read "[[[\"name\",\"like\",\"QA-MC-\"]]]")
has 'QA-MC-A-secret' "$LA3" && ok "company A's records are visible again" || no "A's records lost"
has 'QA-MC-B-secret' "$LA3" && no "LEAK: B's records still visible back in A" || ok "B's are not"

echo "############ the last company cannot be revoked ############"
R=$(acc "$SA" revoke "$UB" "$CB")
echo "$R" | grep -q 'at least one company' && ok "revoking a user's only company is refused" || no "revoke allowed: $R"

echo "############ the letterhead follows the active company ############"
pg "UPDATE res_company SET street='QA-MC-B-STREET' WHERE id=$CB" >/dev/null
pg "UPDATE res_company SET street='QA-MC-A-STREET' WHERE id=1" >/dev/null
P1=$(curl -s -b "session_id=$SA" "$BASE/report/preview/account.move")
echo "$P1" | grep -q 'QA-MC-A-STREET' && ok "preview shows company A's address while in A" || no "preview address wrong for A"
sw "$SA" "$CB" >/dev/null
P2=$(curl -s -b "session_id=$SA" "$BASE/report/preview/account.move")
echo "$P2" | grep -q 'QA-MC-B-STREET' && ok "and company B's after switching" || no "preview did not follow the switch"
echo "$P2" | grep -q 'QA-MC-A-STREET' && no "LEAK: A's address still on B's document" || ok "with no trace of A's"
sw "$SA" 1 >/dev/null
pg "UPDATE res_company SET street=NULL WHERE id=1 AND street='QA-MC-A-STREET'" >/dev/null
pg "UPDATE res_company SET street='07-02, Wisma Trax @ Chan Sow Lin' WHERE id=1 AND street IS NULL" >/dev/null

echo "############ totals on hand-written report SQL are scoped too ############"
# The dashboard and the financial statements are raw aggregate queries. They do
# not pass through BaseModel and so get none of its scoping for free — before
# this was fixed, every card summed every company. A ledger figure crossing
# companies is the most consequential leak of the lot, so it is asserted with a
# posted journal entry rather than by reading the SQL.
JRN=$(pg "SELECT id FROM account_journal WHERE company_id=1 ORDER BY id LIMIT 1")
ACC=$(pg "SELECT id FROM account_account ORDER BY id LIMIT 1")
RPT="$BASE/web/account/report?date_from=1900-01-01&date_to=2999-12-31&report"
if [ -n "$JRN" ] && [ -n "$ACC" ]; then
    # Compared byte-for-byte before and after, rather than grepped for the
    # amount. A first attempt grepped for "777.00" and reported a leak that was
    # not one: an unrelated entry named STJ/777 and a running balance ending in
    # 777.00 both matched. An exact before/after diff cannot be fooled that way
    # and needs no guess about how a figure is formatted.
    D_BEFORE=$(curl -s -b "session_id=$SA" "$BASE/web/account/dashboard")
    declare -A R_BEFORE
    for rep in trial_balance profit_loss balance_sheet general_ledger; do
        R_BEFORE[$rep]=$(curl -s -b "session_id=$SA" "$RPT=$rep")
    done

    pg "INSERT INTO account_move (name, ref, move_type, state, date, journal_id, company_id, amount_total, amount_residual)
        VALUES ('QA-MC/LEAK','QA-MC-LEAK','out_invoice','posted',CURRENT_DATE,$JRN,$CB,777000000,777000000)" >/dev/null
    MV=$(pg "SELECT id FROM account_move WHERE ref='QA-MC-LEAK'")
    pg "INSERT INTO account_move_line (move_id, account_id, debit, credit, company_id, name, date)
        VALUES ($MV,$ACC,777000000,0,$CB,'QA-MC-LEAK line',CURRENT_DATE)" >/dev/null

    D_AFTER=$(curl -s -b "session_id=$SA" "$BASE/web/account/dashboard")
    [ "$D_BEFORE" = "$D_AFTER" ] && ok "company B's posted invoice does not move A's dashboard" \
                                 || no "LEAK: company B's invoice changed company A's dashboard"
    for rep in trial_balance profit_loss balance_sheet general_ledger; do
        A=$(curl -s -b "session_id=$SA" "$RPT=$rep")
        [ "${R_BEFORE[$rep]}" = "$A" ] && ok "$rep is unchanged by company B's entry" \
                                       || no "LEAK: $rep changed when company B posted"
    done

    # ...and it IS counted from inside company B, so the filter scopes rather
    # than simply hiding the row from everyone.
    sw "$SA" "$CB" >/dev/null
    B_GL=$(curl -s -b "session_id=$SA" "$RPT=general_ledger")
    echo "$B_GL" | grep -q 'QA-MC/LEAK' && ok "and it IS in the general ledger when switched into B" \
                                        || no "the entry is missing from B's own general ledger"
    sw "$SA" 1 >/dev/null
    A_GL=$(curl -s -b "session_id=$SA" "$RPT=general_ledger")
    echo "$A_GL" | grep -q 'QA-MC/LEAK' && no "LEAK: B's entry is named in A's general ledger" \
                                        || ok "and named nowhere in A's"

    pg "DELETE FROM account_move_line WHERE move_id=$MV" >/dev/null
    pg "DELETE FROM account_move WHERE id=$MV" >/dev/null
else
    ok "(no journal/account fixture available — report scoping not probed)"
fi

echo "############ identity has exactly one home ############"
[ "$(pg "SELECT count(*) FROM ir_config_parameter WHERE key LIKE 'company.%' OR key IN ('report.addr1','report.reg_number','report.currency_code','report.payment_term_days') OR key LIKE 'report.bank.%'")" = "0" ] \
    && ok "no company identity left in ir_config_parameter" || no "identity keys are back in ir_config_parameter"
[ "$(pg "SELECT count(*) FROM res_partner p JOIN res_company c ON c.partner_id=p.id WHERE p.name IS DISTINCT FROM c.name")" = "0" ] \
    && ok "every company's partner carries the company's own name" || no "a company and its partner disagree on the name"

if [ -n "$FAILED" ]; then echo; echo "*** FAILURES ***"; exit 1; fi
echo; echo "  All checks passed."
