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
# Rental models must inherit the audited path (ARCH-1, docs/040 §1.2).
#
# The module registers GenericViewModel<T> for every model rather than
# hand-rolling ViewModels, specifically so auditing, record rules and OCC
# come for free. That claim is worth proving rather than asserting: this
# writes through the API and requires an audit row to appear.
#
# Container::verifyViewModelCompliance_() already throws at boot if a
# ViewModel mutates outside REGISTER_MUTATOR — so a running server is
# itself evidence. This checks the other half: that the audit actually
# lands in the table.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/ra_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/ra_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }

call() {
    cat > /tmp/ra.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"$1","method":"$2","args":$3,
 "kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' --data @/tmp/ra.json
}

pg "DELETE FROM rental_unit WHERE code LIKE 'AUDIT-%'" >/dev/null
BEFORE=$(pg "SELECT count(*) FROM audit_log WHERE model='rental.unit'")

echo "############ create through the API ############"
R=$(call rental.unit create '[{"code":"AUDIT-01","name":"Audit probe","state":"available"}]')
echo "    $(printf '%s' "$R" | head -c 120)"
UID_=$(pg "SELECT id FROM rental_unit WHERE code='AUDIT-01'")
[ -n "$UID_" ] && ok "unit created (id $UID_)" || no "create failed"

echo
echo "############ write through the API ############"
call rental.unit write "[[$UID_],{\"zone\":\"AuditZone\",\"notes\":\"changed\"}]" > /dev/null
Z=$(pg "SELECT zone FROM rental_unit WHERE id=$UID_")
[ "$Z" = "AuditZone" ] && ok "write persisted" || no "zone is '$Z'"

echo
echo "############ the audit trail ############"
AFTER=$(pg "SELECT count(*) FROM audit_log WHERE model='rental.unit'")
ROWS=$((AFTER - BEFORE))
echo "    audit_log rows for rental.unit: $BEFORE -> $AFTER (+$ROWS)"
# EXACTLY 2 — one create, one write. ">= 2" would have passed against the
# double-audit bug this script was written to find.
[ "$ROWS" = "2" ] && ok "create AND write each audited exactly once" \
                  || no "expected exactly 2 audit rows, got $ROWS"

# audit_log stores record_ids as an integer ARRAY, and the acting user as
# `uid` — not res_id/user_id. Using the wrong names made every query
# return empty, which read as "not audited" when the rows were there.
OPS=$(PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc \
      "SELECT string_agg(DISTINCT operation, ',' ORDER BY operation)
         FROM audit_log WHERE model='rental.unit' AND record_ids @> ARRAY[$UID_]" 2>/dev/null | tr -d ' ')
echo "    operations recorded: $OPS"
printf '%s' "$OPS" | grep -q "create" && ok "create recorded" || no "no create in audit"
printf '%s' "$OPS" | grep -q "write"  && ok "write recorded"  || no "no write in audit"

USR=$(pg "SELECT uid FROM audit_log WHERE model='rental.unit' AND record_ids @> ARRAY[$UID_] LIMIT 1")
[ -n "$USR" ] && [ "$USR" != "0" ] && ok "the acting user is attributed (uid=$USR)" \
                                   || no "audit row has no user"

echo
echo "############ exactly once, not twice ############"
# P6 found duplicate audit rows where a converted mutator still made a
# manual log() call. One write must produce exactly one audit row.
N1=$(pg "SELECT count(*) FROM audit_log WHERE model='rental.unit' AND record_ids @> ARRAY[$UID_] AND operation='write'")
call rental.unit write "[[$UID_],{\"zone\":\"AuditZone2\"}]" > /dev/null
N2=$(pg "SELECT count(*) FROM audit_log WHERE model='rental.unit' AND record_ids @> ARRAY[$UID_] AND operation='write'")
echo "    write audit rows: $N1 -> $N2"
[ "$((N2 - N1))" = "1" ] && ok "one write produced exactly one audit row" \
                         || no "one write produced $((N2 - N1)) audit rows"

echo
echo "############ cleanup ############"
pg "DELETE FROM audit_log  WHERE model='rental.unit' AND record_ids @> ARRAY[$UID_]" >/dev/null
pg "DELETE FROM rental_unit WHERE code LIKE 'AUDIT-%'" >/dev/null
echo "    probe rows removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
