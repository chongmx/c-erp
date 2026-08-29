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
# Financial statement reports (docs/081): Trial Balance, Profit & Loss,
# Balance Sheet, General Ledger, Aged Receivable — served from posted
# account.move.line. The load-bearing assertions are the double-entry
# invariants that a real accounting report must satisfy:
#   * Trial Balance: total debit == total credit
#   * Balance Sheet: total assets == total liabilities + equity
#   * P&L net profit == the change in equity's current-year earnings
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
get() { curl -s -H "Cookie: session_id=$SID" \
        "$BASE/web/account/report?report=$1&date_from=2000-01-01&date_to=2099-12-31"; }

echo "############ all five reports return JSON (no Internal Error) ############"
for r in trial_balance profit_loss balance_sheet general_ledger aged_receivable; do
    if get "$r" | grep -q "\"report\":\"$r\""; then ok "$r renders"; else no "$r failed: $(get "$r" | head -c 100)"; fi
done

echo "############ double-entry invariants ############"
python3 - "$SID" "$BASE" <<'PY'
import sys, json, urllib.request
sid, base = sys.argv[1], sys.argv[2]
def rep(r):
    u = base + "/web/account/report?report=%s&date_from=2000-01-01&date_to=2099-12-31" % r
    req = urllib.request.Request(u, headers={"Cookie": "session_id=" + sid})
    return json.load(urllib.request.urlopen(req))
def num(s): return float(str(s).replace(",", "")) if s not in ("", None) else 0.0
fails = 0
# Trial balance: total debit == total credit
tb = rep("trial_balance"); tot = tb["rows"][-1]["cells"]
if abs(num(tot[1]) - num(tot[2])) < 0.01: print("    PASS  Trial Balance balances (debit == credit ==", tot[1] + ")")
else: print("    FAIL  Trial Balance debit", tot[1], "!= credit", tot[2]); fails += 1
# Balance sheet: assets == liabilities + equity
bs = rep("balance_sheet"); rows = bs["rows"]
ta = num([r["cells"][1] for r in rows if r["cells"][0] == "Total Assets"][0])
le = num([r["cells"][1] for r in rows if r["cells"][0].startswith("Total Liabilities + Equity")][0])
if abs(ta - le) < 0.01: print("    PASS  Balance Sheet balances (Assets == L+E ==", ("%.2f" % ta) + ")")
else: print("    FAIL  Balance Sheet Assets %.2f != L+E %.2f" % (ta, le)); fails += 1
# P&L net == current-year earnings shown on the balance sheet
pl = rep("profit_loss"); net = num(pl["rows"][-1]["cells"][1])
cye = num([r["cells"][1] for r in rows if r["cells"][0] == "Current Year Earnings"][0])
if abs(net - cye) < 0.01: print("    PASS  P&L net profit == BS current-year earnings (%.2f)" % net)
else: print("    FAIL  P&L net %.2f != BS earnings %.2f" % (net, cye)); fails += 1
sys.exit(1 if fails else 0)
PY
[ $? -ne 0 ] && FAILED=1

echo "############ print (PDF) view renders ############"
curl -s -H "Cookie: session_id=$SID" "$BASE/web/account/report/print?report=balance_sheet&date_to=2099-12-31" \
    | grep -q "Balance Sheet" && ok "printable HTML renders" || no "print view broken"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
