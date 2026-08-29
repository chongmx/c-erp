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
# S-49 — the domain filter honours the field allowlist.
#
# Domain::sanitizeColumn_ used to charset-check the field name only, so an
# authenticated user could filter on ANY column — the SELECT list was
# restricted but the WHERE clause was not — and blind-extract it one
# `like` substring at a time.
#
#     password like 'pbkdf2'  -> 1 row   (substring present)
#     password like 'ZZZZZ'   -> 0 rows  (substring absent)
#
# The difference is the leak. After the fix, filtering on an unregistered
# column is REJECTED, so the two become indistinguishable.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vda_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vda_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

# $1 = model  $2 = domain  ->  echoes "N" rows or "ERR"
q() {
    cat > /tmp/vda.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"search_read",
 "args":[$2],
 "kwargs":{"fields":["id"],"limit":9,"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
         --data @/tmp/vda.json \
    | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d['result']) if 'result' in d else 'ERR')"
}

echo "############ 1. registered fields still filter correctly ############"
A=$(q res.users '[["login","=","admin"]]')
B=$(q res.users '[["login","=","definitely_no_such_login"]]')
echo "    login=admin -> $A   login=nope -> $B"
[ "$A" -ge 1 ] 2>/dev/null && ok "a true condition on a registered field returns rows" \
                           || no "registered-field filter broke: $A"
[ "$B" = "0" ]             && ok "a false condition returns none — the filter works" \
                           || no "expected 0, got $B"

echo
echo "############ 2. an UNREGISTERED column is rejected, not evaluated ############"
for dom in '[["password","=","x"]]' '[["password","like","pbkdf2"]]' '[["password","like","ZZZZZ"]]'; do
    R=$(q res.users "$dom")
    echo "    $dom -> $R"
    [ "$R" = "ERR" ] && ok "rejected" || no "$dom returned $R — column is filterable"
done

echo
echo "############ 3. THE blind-extraction channel is closed ############"
# Present vs absent substring must now be INDISTINGUISHABLE. If one is
# ERR and the other is a row count, the leak is back.
P=$(q res.users '[["password","like","pbkdf2"]]')
Z=$(q res.users '[["password","like","ZZZZZ"]]')
echo "    present-substring -> $P   absent-substring -> $Z"
[ "$P" = "$Z" ] && ok "present and absent substrings are indistinguishable" \
                || no "still leaking: present=$P absent=$Z"

echo
echo "############ 4. a nonexistent column is rejected too ############"
R=$(q res.users '[["no_such_column_at_all","=",1]]')
[ "$R" = "ERR" ] && ok "unknown column rejected" || no "unknown column returned $R"

echo
echo "############ 5. the guard holds on OR / NOT branches, not just top level ############"
# A leaf hidden inside an OR must be checked too — the compiler recurses.
R=$(q res.users '["|",["login","=","admin"],["password","like","pbkdf2"]]')
echo "    OR(login=admin, password like pbkdf2) -> $R"
[ "$R" = "ERR" ] && ok "a bad leaf inside OR is still rejected" \
                 || no "OR branch bypassed the allowlist: $R"

echo
echo "############ 6. it holds across models, not just res.users ############"
R=$(q res.partner '[["password","like","x"]]')
# res.partner has no password column at all; must be rejected, not a SQL error.
[ "$R" = "ERR" ] && ok "res.partner also rejects an unregistered column" \
                 || no "res.partner returned $R"

echo
echo "############ SUMMARY ############"
rm -f /tmp/vda.json /tmp/vda_auth.json
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
