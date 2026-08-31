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
# read_group — grouped aggregation (docs/095).
#
# This is the primitive under grouped lists, the pivot, the graph and the
# kanban board. Two properties matter more than the rest:
#
#   * the group counts must sum to search_count over the same domain. If they
#     do not, some rows are being dropped or double-counted, and every view
#     built on top inherits the error silently as a wrong total.
#   * a group's __domain must select exactly that group's rows, because that is
#     what a client drills into. Date buckets are the trap: adjacent months must
#     be half-open, or a row on the 1st lands in both.
#
# It is served from the MODEL, not the ViewModel, so it must work on models
# whose ViewModel is hand-written (account.move, sale.order, res.partner) —
# those are asserted explicitly.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }

rg(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"read_group\",\"args\":[$2,$3,$4],\"kwargs\":{\"context\":{\"session_id\":\"$SID\"}}}}"; }
cnt(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"search_count\",\"args\":[$2],\"kwargs\":{\"context\":{\"session_id\":\"$SID\"}}}}" \
      | sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }
sumcounts(){ echo "$1" | grep -o '"__count":[0-9]*' | cut -d: -f2 | paste -sd+ | bc; }

echo "############ it answers for models with a hand-written ViewModel ############"
# Registering read_group on GenericViewModel alone would have covered only the
# generic models and silently skipped exactly these.
for m in account.move sale.order res.partner purchase.order stock.picking; do
    R=$(rg "$m" '[]' '[]' '["company_id"]')
    echo "$R" | grep -q '"result"' && ok "$m groups" || no "$m read_group failed: $(echo "$R" | head -c 120)"
done

echo "############ counts reconcile with search_count ############"
for m in account.move sale.order; do
    R=$(rg "$m" '[]' '[]' '["state"]')
    S=$(sumcounts "$R"); T=$(cnt "$m" '[]')
    [ -n "$S" ] && [ "$S" = "$T" ] && ok "$m: groups total $S = search_count $T" \
                                   || no "$m: groups total '$S' <> search_count '$T'"
done

echo "############ a filtered domain is honoured ############"
R=$(rg account.move '[["move_type","=","out_invoice"]]' '[]' '["state"]')
S=$(sumcounts "$R"); T=$(cnt account.move '[["move_type","=","out_invoice"]]')
[ "$S" = "$T" ] && ok "filtered groups total $S = filtered count $T" || no "filtered mismatch $S/$T"

echo "############ measures are summed, and money keeps its scale ############"
R=$(rg account.move '[["move_type","=","out_invoice"]]' '["amount_total"]' '["state"]')
echo "$R" | grep -q '"amount_total"' && ok "amount_total is aggregated" || no "no amount_total in groups"
# Money is stored in micro-units; a raw SUM would come back a million times too big.
BIG=$(echo "$R" | grep -o '"amount_total":[0-9.]*' | cut -d: -f2 | sort -rn | head -1)
awk -v v="$BIG" 'BEGIN{exit !(v < 1000000000)}' \
    && ok "sums are in major units, not micro-units ($BIG)" \
    || no "money sum looks unscaled: $BIG"

echo "############ a non-numeric field is not summed ############"
R=$(rg account.move '[]' '["name","state"]' '["move_type"]')
echo "$R" | grep -q '"name":' && no "a char field was aggregated" || ok "char fields are skipped as measures"

echo "############ many2one groups carry a label ############"
R=$(rg account.move '[]' '[]' '["journal_id"]')
echo "$R" | grep -qE '"journal_id":\[[0-9]+,"' && ok "journal_id resolves to [id, name]" \
                                               || no "journal_id not resolved: $(echo "$R" | head -c 140)"

echo "############ date buckets are half-open ############"
R=$(rg account.move '[]' '[]' '["date:month"]')
echo "$R" | grep -q '"date:month"' && ok "date grouping buckets by month" || no "no month bucket"
# Every bucket needs BOTH bounds, or two adjacent months overlap on the 1st.
NB=$(echo "$R" | grep -o '\["date",">="' | wc -l)
NE=$(echo "$R" | grep -o '\["date","<"' | wc -l)
[ "$NB" = "$NE" ] && [ "$NB" != "0" ] && ok "each bucket has both bounds ($NB pairs)" \
                                      || no "bucket bounds unbalanced: $NB starts, $NE ends"
# ...and drilling into a bucket's own __domain must return its own count.
# grep -o, not sed: a sed `.*` prefix is greedy and matched the LAST bucket's
# domain, which then got compared against the FIRST bucket's count and reported
# a leak that did not exist.
FIRST=$(echo "$R" | grep -o '\[\["date",">=","[0-9-]*"\],\["date","<","[0-9-]*"\]\]' | head -1)
FCNT=$(echo "$R" | grep -o '"__count":[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$FIRST" ]; then
    D=$(cnt account.move "$FIRST")
    [ "$D" = "$FCNT" ] && ok "drilling into a bucket returns its own count ($D)" \
                       || no "bucket __domain gives $D, group said $FCNT"
else
    no "could not extract a bucket domain"
fi

echo "############ multi-level grouping ############"
R=$(rg account.move '[]' '[]' '["move_type","state"]')
S=$(sumcounts "$R"); T=$(cnt account.move '[]')
[ "$S" = "$T" ] && ok "two-level groups still total $S" || no "two-level total $S <> $T"

echo "############ bad input is refused, not guessed ############"
rg account.move '[]' '[]' '["no_such_field"]' | grep -q 'Unknown group-by field' \
    && ok "an unknown group-by field is rejected" || no "unknown field accepted"
rg account.move '[]' '[]' '["date:century"]' | grep -q 'Unsupported group-by interval' \
    && ok "an unsupported interval is rejected" || no "bad interval accepted"
rg account.move '[]' '[]' '[]' | grep -q 'at least one group-by' \
    && ok "grouping by nothing is rejected" || no "empty groupby accepted"

echo "############ grouping respects company scoping ############"
# A grouped total that counted rows the caller cannot open would leak by
# arithmetic even though no row is ever returned.
R=$(rg account.move '[]' '[]' '["company_id"]')
NC=$(echo "$R" | grep -o '"company_id":\[[0-9]*' | wc -l)
[ "$NC" -le 1 ] && ok "only the active company appears in the groups" \
                || no "groups span $NC companies"

echo "############ the views that consume it are wired in ############"
curl -s "$BASE/src/components/RecordViews.js" | grep -q 'class GroupedListView' \
    && ok "RecordViews.js is served" || no "RecordViews.js not served"
curl -s "$BASE/src/components/recordviews.css" | grep -q 'rv-kanban' \
    && ok "its stylesheet is served" || no "recordviews.css not served"
# The application shell moved from "/" to "/login" — "/" is the public
# website now (docs/126).
curl -s "$BASE/login" | grep -q 'RecordViews.js' \
    && ok "index.html loads it before app.js" || no "index.html does not load RecordViews.js"
for c in GroupedListView KanbanView PivotView GraphView CalendarView; do
    curl -s "$BASE/src/app.js" | grep -q "$c" && ok "ActionView can render $c" || no "$c not registered in app.js"
done
# OWL parses templates as XML, where a control character is invalid and takes the
# whole component down at compile time. One crept in as a cell-key separator and
# blanked the entire pivot, so the file is scanned for them.
if curl -s "$BASE/src/components/RecordViews.js" | grep -qP '[\x00-\x08\x0b\x0c\x0e-\x1f]'; then
    no "RecordViews.js contains a control character — OWL will fail to compile it"
else
    ok "no control characters in the template source"
fi

if [ -n "$FAILED" ]; then echo; echo "*** FAILURES ***"; exit 1; fi
echo; echo "  All checks passed."
