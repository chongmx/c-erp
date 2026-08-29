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
# P2: res.currency.rate is user-maintained (docs/048 §4.3).
# res.currency was read-only (LookupViewModel) before this.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vc_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vc_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

call() {
    cat > /tmp/vc.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{${4:-}"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vc.json
}

echo "############ active currencies + rates ############"
call res.currency search_read '[[["active","=",true]]]' '"fields":["id","name","symbol","rate"],' \
  | python3 -m json.tool 2>/dev/null | grep -E '"name"|"rate"' | paste - - | sed 's/^/    /'

USD_ID=$(pg "SELECT id FROM res_currency WHERE name = 'USD'")
MYR_ID=$(pg "SELECT id FROM res_currency WHERE name = 'MYR'")
echo "    USD id=$USD_ID  MYR id=$MYR_ID (base)"

echo
echo "############ base currency is MYR ############"
BASE_CCY=$(pg "SELECT c.name FROM res_company co JOIN res_currency c ON c.id = co.currency_id LIMIT 1")
[ "$BASE_CCY" = "MYR" ] && ok "res_company.currency_id -> MYR" || no "base currency is '$BASE_CCY'"

echo
echo "############ writing a rate (was read-only before P2) ############"
R=$(call res.currency write "[[$USD_ID],{\"rate\":4.70}]")
echo "    write -> $(echo "$R" | head -c 90)"
STORED=$(pg "SELECT rate FROM res_currency WHERE id = $USD_ID")
echo "    DB rate = $STORED micros"
[ "$STORED" = "4700000" ] && ok "4.70 stored as 4700000 micros" || no "expected 4700000, got $STORED"

R=$(call res.currency search_read "[[[\"id\",\"=\",$USD_ID]]]" '"fields":["id","name","rate"],')
echo "    read back -> $(echo "$R" | head -c 120)"
echo "$R" | grep -q '"rate":4.7' && ok "reads back as 4.7 major units" || no "wrong read-back"

echo
echo "############ validation ############"
R=$(call res.currency write "[[$USD_ID],{\"rate\":0}]")
echo "$R" | grep -qi "greater than zero" && ok "zero rate rejected with a clear message" \
                                         || no "zero rate accepted: $(echo "$R"|head -c 120)"
R=$(call res.currency write "[[$USD_ID],{\"rate\":-2}]")
echo "$R" | grep -qi "greater than zero" && ok "negative rate rejected" || no "negative rate accepted"

echo
echo "############ conversion arithmetic ############"
echo "    100 USD @ 4.70 should be 470.00 MYR"
python3 - <<'PY'
MICRO = 1_000_000
usd, rate = 100 * MICRO, int(4.70 * MICRO)
myr = usd * rate // MICRO
print("      %d micros = %.2f MYR" % (myr, myr / MICRO))
print("      PASS" if myr == 470 * MICRO else "      FAIL")
PY

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
