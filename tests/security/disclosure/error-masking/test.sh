#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# SECURITY — SEC-28: error messages must not disclose the schema.
#
# THE CONTROL: every catch block that writes to a response gates the detail
# behind devMode. `ex.what()` from pqxx carries the full SQL statement, the
# table and column names, and the constraint that failed — which is a map of
# the database handed to whoever can provoke an error.
#
# This test provokes errors on purpose, through several different paths, and
# asserts that what comes back is useful to a user and useless to an attacker.
#
# A pen test that passes proves a control works; it must fail loudly if the
# control is removed. So each check names SEC-28, and the failure prints the
# leaked text — you should not have to go looking for what escaped.
#
# NOTE ON devMode: with dev_mode=true the server returns ex.what() BY DESIGN.
# If everything here fails at once, check that first — the test says so in its
# output rather than leaving you to guess.
# =============================================================
auth_or_die

# The markers. Anything from this list in a client-visible message means the
# database is describing itself to the caller.
LEAKS='SELECT |INSERT INTO|UPDATE |DELETE FROM|pqxx|ERROR:|DETAIL:|relation "|constraint "|violates |column "|_pkey|_key"|at character'

leaked() {  # leaked <text> -> prints the offending fragment, or nothing
    echo "$1" | grep -oE "$LEAKS" | head -1
}

check_masked() {  # check_masked <label> <response>
    local what; what=$(leaked "$2")
    if [ -n "$what" ]; then
        no "SEC-28: '$1' leaked schema detail ($what) — $(echo "$2" | head -c 200)"
    else
        ok "SEC-28: '$1' is masked"
    fi
}

sec "1. a unique-constraint violation"
# The richest source of leakage: pqxx reports the table, the column, the
# constraint name and the offending value.
R1=$(call res.users create '[{"login":"admin","name":"dup"}]')
if has_error "$R1"; then check_masked "duplicate login" "$R1"
else no "creating a second user with login 'admin' SUCCEEDED — the unique constraint is missing"
     pg "DELETE FROM res_users WHERE login='admin' AND name='dup'" >/dev/null; fi

sec "2. a not-null violation"
R2=$(call res.users create '[{"name":"no login"}]')
if has_error "$R2"; then check_masked "missing required column" "$R2"
else ok "NOTE: creating a user without a login was accepted (nothing to mask)"; fi

sec "3. a type mismatch reaching the database"
R3=$(call sale.order.line create '[{"order_id":"not-a-number","name":"x","product_uom_qty":1}]')
if has_error "$R3"; then check_masked "type mismatch" "$R3"
else ok "NOTE: the bad type was rejected before SQL (nothing to mask)"; fi

sec "4. an unknown model"
R4=$(call no.such.model search_read '[[],["id"]]')
if has_error "$R4"; then
    check_masked "unknown model" "$R4"
    # Masked must not mean useless: a caller still has to be able to act on it.
    t_contains "$R4" '"message"' "the masked error still carries a message"
else no "an unknown model did not error at all"; fi

sec "5. an unknown column in a filter"
# S-49's territory, checked here for what it SAYS rather than what it refuses:
# the refusal must not name the columns that do exist.
R5=$(call res.users search_read '[[["nonexistent_col","=","x"]],["id"]]')
if has_error "$R5"; then check_masked "unknown filter column" "$R5"
else no "filtering on a column that does not exist was accepted"; fi

sec "6. a missing record on an HTTP route"
# HTML routes have their own catch blocks, and SEC-28 applies to them too —
# they were the original reason the rule exists.
BODY=$(http_get "/web/content/999999999")
CODE=$(http_code "/web/content/999999999")
echo "    /web/content/999999999 -> HTTP $CODE"
check_masked "missing attachment route" "$BODY"

sec "7. the report route with a bad id"
BODY7=$(http_get "/report/pdf/sale.order/999999999")
check_masked "report route" "$BODY7"

sec "8. a stack trace never reaches the client"
ALL="$R1$R2$R3$R4$R5$BODY$BODY7"
t_lacks "$ALL" ".cpp:"          "no source file:line in any response"
t_lacks "$ALL" "std::"          "no C++ type names in any response"
t_lacks "$ALL" "/home/"         "no filesystem paths in any response"

[ -n "$FAILED" ] && echo "    (if every check failed, confirm the server is NOT running with dev_mode=true —
     devMode returns ex.what() deliberately, and SEC-28 only applies with it off.)"

verdict
