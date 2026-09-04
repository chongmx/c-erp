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
# P6: the audit wrapper is automatic now. Any handler that ALSO
# still calls AuditService::log() itself would write TWO rows per
# operation — a silent corruption of the trail.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
PROBED=0
SKIPPED=0

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vd_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vd_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

call() {
    cat > /tmp/vda.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/vda.json
}

# model | create-payload | a field to write
probe() {
    local model=$1 payload=$2 wfield=$3
    local before after created delta
    before=$(pg "SELECT count(*) FROM audit_log WHERE model = '$model' AND operation = 'create'")
    R=$(call "$model" create "[$payload]")
    created=$(printf '%s' "$R" | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
    if [ -z "$created" ]; then
        echo "    SKIP  $model — create failed: $(printf '%s' "$R" | head -c 100)"
        SKIPPED=$((SKIPPED + 1))
        return
    fi
    PROBED=$((PROBED + 1))
    after=$(pg "SELECT count(*) FROM audit_log WHERE model = '$model' AND operation = 'create'")
    delta=$((after - before))
    printf '    %-22s create -> id=%-5s audit rows +%s' "$model" "$created" "$delta"
    if [ "$delta" = "1" ]; then echo "   PASS"; else echo "   FAIL (duplicate)"; FAILED=1; fi

    # write
    before=$(pg "SELECT count(*) FROM audit_log WHERE model = '$model' AND operation = 'write'")
    call "$model" write "[[$created],{$wfield}]" >/dev/null
    after=$(pg "SELECT count(*) FROM audit_log WHERE model = '$model' AND operation = 'write'")
    delta=$((after - before))
    printf '    %-22s write            audit rows +%s' "$model" "$delta"
    if [ "$delta" = "1" ]; then echo "   PASS"; else echo "   FAIL (duplicate)"; FAILED=1; fi

    # unlink
    before=$(pg "SELECT count(*) FROM audit_log WHERE model = '$model' AND operation = 'unlink'")
    call "$model" unlink "[[$created]]" >/dev/null
    after=$(pg "SELECT count(*) FROM audit_log WHERE model = '$model' AND operation = 'unlink'")
    delta=$((after - before))
    printf '    %-22s unlink           audit rows +%s' "$model" "$delta"
    if [ "$delta" = "1" ]; then echo "   PASS"; else echo "   FAIL (duplicate)"; FAILED=1; fi
}

# Discover real foreign keys so the stock.picking / mrp.bom probes create
# cleanly instead of SKIPping on a from-scratch DB (they used to hardcode
# picking_type_id=1 / product_id=1, ids that need not exist). Clear any
# leftovers from an interrupted prior run so create() cannot fail on a dup.
PTYPE=$(pg "SELECT id FROM stock_picking_type ORDER BY id LIMIT 1")
PROD=$(pg "SELECT id FROM product_product ORDER BY id LIMIT 1")
pg "DELETE FROM res_users WHERE login='p6probe'" >/dev/null

echo "############ exactly one audit row per operation ############"
echo "--- hand-written ViewModels ---"
# The write field must be one res.users ACTUALLY HAS. Its columns are
# login/partner_id/company_id/lang/tz/active/share — a user's name lives on
# res_partner. This probe used to write "name", which the old BaseModel::write
# silently dropped: the row was untouched, yet write_date was stamped and an
# audit row appeared, so the probe passed while asserting nothing. Once write()
# started rejecting unknown fields the write 400'd and the audit count went to
# zero, which is what exposed it. "lang" is a real column, so the write now
# changes something and the audit assertion means what it says.
probe "res.users"        '{"login":"p6probe","name":"P6 Probe","password":"Sup3rSecret!x"}' '"lang":"en_US"'
probe "stock.picking"    "{\"name\":\"P6PROBE\",\"picking_type_id\":$PTYPE}"                 '"name":"P6PROBE2"'
probe "mrp.bom"          "{\"product_id\":$PROD,\"product_qty\":1}"                          '"product_qty":2'

# GenericViewModel<T> backs MOST models in the system, and until now not
# one of them was covered here: res.users has a hand-written ViewModel,
# and the other three probes all SKIPPED because their creates fail
# without more setup. So the suite reported "no double audit" while every
# generic model logged twice — REGISTER_MUTATOR audited, and the handler
# then called AuditService::log() again (the P6 leftover, docs/050).
#
# These two models are chosen because they create cleanly with no
# required foreign keys, so the probe can never silently SKIP.
echo "--- GenericViewModel<T> (the path most models use) ---"
probe "rental.unit"             '{"code":"P6PROBE-U1","name":"P6 probe unit"}' '"zone":"P6Z"'
probe "rental.expense.category" '{"name":"P6 Probe Category"}'                 '"is_operating":false'

echo
echo "############ coverage ############"
echo "    models actually probed: $PROBED    skipped: $SKIPPED"
# A skip proves nothing. This suite passed for months while three of its
# four probes skipped and the one that ran was the only model NOT on the
# path that was broken. Coverage is therefore asserted, not just printed.
if [ "$PROBED" -lt 4 ]; then
    echo "    FAIL  fewer than 4 models probed — this run proves too little"
    FAILED=1
else
    echo "    PASS  $PROBED models probed, covering both ViewModel paths"
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES — duplicate audit rows ***" || echo "  All checks passed."
