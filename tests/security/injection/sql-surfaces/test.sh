#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# SECURITY — S-49: every place a CALLER NAMES A COLUMN.
#
# THE CONTROL: a column name that reaches SQL must be checked against the
# model's registered fields — not merely validated for [A-Za-z0-9_].
#
# Why the charset check is not enough, and why this test exists: values are
# bound as $N, so injection through a *value* is already impossible. The hole
# is the column NAME. A charset-valid name like `password` is not injection —
# it is a real column — and a `like` filter on it leaks the contents one
# substring at a time, blind, no matter how restricted the SELECT list is.
# The leak is in the WHERE.
#
# So this asserts REFUSAL, not escaping. A response of "no rows" is a FAIL
# here: it means the filter ran.
#
# Surfaces covered: domain fields, ORDER BY, GROUP BY (read_group), and the
# catalogue's parametric facet keys — every entry point where the client
# supplies an identifier rather than a value.
# =============================================================
auth_or_die

refused() {  # refused <label> <response>
    # A refusal has three legitimate shapes, and all three count:
    #   {"error":{...}}                    a JSON-RPC error
    #   {"error":"Invalid JSON", ...}      rejected before it was even parsed
    #   an HTTP-level "Bad request"
    # Only the last one is subtle: a probe containing a quote character makes
    # the REQUEST malformed, so the server never builds a query from it. That
    # is still the attack failing — but it is not the allowlist doing the
    # work, so the message says which one stopped it.
    case "$2" in
        *'"error":{'*|*'"error": {'*)  ok "$1 is refused" ;;
        *'Invalid JSON'*|*'Bad request'*) ok "$1 is refused (rejected as a malformed request)" ;;
        *) no "$1 was ACCEPTED — the column name reached SQL: $(echo "$2" | head -c 160)" ;;
    esac
}

# Build a domain probe safely: the payloads deliberately contain quotes and
# backslashes, so they are serialised by json.dumps rather than pasted into a
# string. Pasting them is how the previous version turned an injection probe
# into a syntax error and then asserted on the wrong thing.
domain_probe() {  # domain_probe <model> <field>
    local args
    args=$(python3 -c "
import json,sys
print(json.dumps([[[sys.argv[1], '=', 1]], ['id']]))" "$2" 2>/dev/null)
    call "$1" search_read "$args"
}

sec "1. a domain naming a real but unregistered column"
# The blind-read attack in its purest form. `password` exists on res_users;
# it is not a registered field, so naming it must be an error.
refused "filtering res.users on 'password'" \
        "$(call res.users search_read '[[["password","like","a"]],["id"]]')"
refused "filtering res.users on 'password_crypt'" \
        "$(call res.users search_read '[[["password_crypt","like","a"]],["id"]]')"
refused "filtering on a column of another table" \
        "$(call res.partner search_read '[[["session_id","!=",false]],["id"]]')"

sec "2. a domain field carrying SQL"
for probe in 'id) OR 1=1--' 'id;DROP TABLE res_partner' 'id"' "id'" '(SELECT 1)' 'id\\' 'id/*x*/'; do
    refused "domain field '$probe'" "$(domain_probe res.partner "$probe")"
done

sec "3. ORDER BY"
refused "order by an unregistered column" \
        "$(call_k res.partner search_read '[[],["id"]]' '"order":"password"')"
refused "order by a statement"            \
        "$(call_k res.partner search_read '[[],["id"]]' '"order":"id; DROP TABLE res_partner"')"
refused "order by a subselect"            \
        "$(call_k res.partner search_read '[[],["id"]]' '"order":"(SELECT 1)"')"
OKORD=$(call_k res.partner search_read '[[],["id"]]' '"order":"name desc"')
has_error "$OKORD" && no "a LEGITIMATE order was refused — the allowlist is too tight" \
                   || ok "a legitimate order still works"

sec "4. GROUP BY"
refused "grouping on an unregistered column" \
        "$(call_k res.partner read_group '[[],["id"],["password"]]' '')"

sec "5. the catalogue's facet keys"
# The parts catalogue takes keys like `param:resistance`. Unlike the surfaces
# above, the part of the key after `param:` is a VALUE looked up in
# part_parameter — it is bound as $n, not interpolated as a column — so this
# one is structurally safe rather than allowlisted.
#
# So the assertion is different in kind: not "refused", but "nothing was
# executed and nothing leaked".
FACET=$(call part.catalog search '[{"facets":{"param:x\") OR 1=1--":["1"]}}]')
t_lacks "$FACET" "SELECT "  "a hostile facet key does not echo SQL"
t_lacks "$FACET" "ERROR:"   "a hostile facet key does not leak a database error"
# NOTE, deliberately not a failure: an unrecognised facet key is currently
# IGNORED, so the response is the unfiltered catalogue rather than an error or
# an empty result. That is safe — but a user who filters on something the
# server does not recognise gets everything back and may believe it is
# filtered. Worth tightening in the catalogue itself; it is not a hole here.
case "$FACET" in
    *'"rows"'*) echo "    NOTE  an unknown facet key is ignored, not refused — the caller gets the unfiltered list" ;;
esac

sec "6. nothing was actually executed"
# The point of the group: prove the attacks failed, not just that they
# returned an error. If any DROP had run, these would be gone.
for t in res_partner res_users product_product account_move; do
    t_eq "1" "$(pg "SELECT count(*) FROM information_schema.tables WHERE table_name='$t'")" \
         "$t still exists"
done
t_ge "$(pg "SELECT count(*) FROM res_users")" 1 "users still present"

sec "7. and nothing leaked on the way"
# A refusal that names the columns it knows about is a slower version of the
# same disclosure. SEC-28 covers the message; this covers the shape.
R=$(call res.users search_read '[[["password","like","a"]],["id"]]')
t_lacks "$R" "password_crypt" "the refusal does not name neighbouring columns"
t_lacks "$R" "SELECT "        "the refusal does not quote the SQL"

verdict
