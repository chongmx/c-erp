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
# Smoke test for the screens added in docs/090.
#
# The feature tests drive the business logic; this drives the calls the UI
# makes on mount. A model can have perfect behaviour over RPC and still show
# "Internal Error" because get_views has no arch to return — that is exactly
# the class of bug docs/077 chased, so it gets its own cheap guard.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }

# A call is only healthy if it returns a result AND no error key.
chk(){ # label, response
    if echo "$2" | grep -q '"error"'; then
        no "$1 — $(echo "$2" | sed -n 's/.*"message":"\([^"]*\)".*/\1/p' | head -1)"
    else
        ok "$1"
    fi
}

echo "############ list + form views resolve ############"
for m in hr.expense hr.expense.sheet stock.quant.package; do
    R=$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$m\",\"method\":\"get_views\",\"args\":[[[false,\"list\"],[false,\"form\"]]],\"kwargs\":{$CTX}}}")
    chk "$m get_views" "$R"
done

echo "############ the list query each screen opens with ############"
chk "hr.expense search_read"        "$(call hr.expense search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "hr.expense.sheet search_read"  "$(call hr.expense.sheet search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "stock.quant.package search_read" "$(call stock.quant.package search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "stock.move search_read (lot+package columns)" "$(call stock.move search_read "[[]]" "{$CTX,\"limit\":5}")"
chk "stock.quant search_read"       "$(call stock.quant search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "stock.warehouse.orderpoint search_read" "$(call stock.warehouse.orderpoint search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "stock.putaway.rule search_read" "$(call stock.putaway.rule search_read "[[]]" "{$CTX,\"limit\":10}")"

echo "############ dropdown lookups the expense form needs ############"
chk "hr.employee search_read"    "$(call hr.employee search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "account.tax search_read"    "$(call account.tax search_read "[[]]" "{$CTX,\"limit\":10}")"
chk "account.journal search_read" "$(call account.journal search_read "[[]]" "{$CTX,\"limit\":10}")"

echo "############ the document-template editor's load path ############"
TID=$(PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "SELECT id FROM ir_report_template ORDER BY id LIMIT 1" 2>/dev/null | tr -d ' ')
chk "ir.report.template search_read" "$(call ir.report.template search_read "[[[\"model\",\"=\",\"account.move\"]]]" "{$CTX}")"
chk "ir.report.template read"        "$(call ir.report.template read "[[$TID]]" "{$CTX}")"

echo "############ the attachment panel's load path ############"
chk "ir.attachment search_read (empty record)" "$(call ir.attachment search_read "[[[\"res_model\",\"=\",\"hr.expense.sheet\"],[\"res_id\",\"=\",0]]]" "{$CTX}")"
chk "ir.attachment fields_get"                 "$(call ir.attachment fields_get "[[]]" "{$CTX}")"

echo "############ the product form's stat row ############"
PRD=$(PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "SELECT id FROM product_product ORDER BY id LIMIT 1" 2>/dev/null | tr -d ' ')
chk "stock.quant product_summary" "$(call stock.quant product_summary "[$PRD]" "{$CTX}")"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
