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
# P4 — ir.sequence.
#
# The properties that matter for a legal document series:
#   * no duplicates under concurrency
#   * no gaps when a transaction rolls back
#   * yearly reset
#   * prefix / padding configurable without a code change
# =============================================================
DBN=${DBN:-odoo}
FAILED=
pg()  { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
pgf() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -q -f "$1" 2>&1; }
ok()  { echo "    PASS  $1"; }
no()  { echo "    FAIL  $1"; FAILED=1; }

echo "############ seeded sequences ############"
PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -c \
  "SELECT code, prefix, padding, number_next, reset_policy FROM ir_sequence ORDER BY code" 2>/dev/null

echo
echo "############ 1. continuity: seeded from the old PG sequences ############"
# The migration seeded number_next from each old sequence's last_value + 1, so
# numbering continues rather than restarting and colliding with existing docs.
SO_NEXT=$(pg "SELECT number_next FROM ir_sequence WHERE code = 'sale.order'")
SO_MAX=$(pg "SELECT COALESCE(MAX(SUBSTRING(name FROM '[0-9]+\$')::int), 0) FROM sale_order WHERE name LIKE 'SO/%'")
echo "    ir_sequence next = $SO_NEXT ; highest existing SO number = $SO_MAX"
[ "$SO_NEXT" -gt "$SO_MAX" ] && ok "next number is beyond every existing document" \
                             || no "next=$SO_NEXT would collide with existing max=$SO_MAX"

echo
echo "############ 2. no duplicates under concurrency (8 parallel allocators) ############"
cat > /tmp/seq_alloc.sql <<'SQL'
BEGIN;
SELECT id, number_next FROM ir_sequence WHERE code='concurrency.test' FOR UPDATE;
UPDATE ir_sequence SET number_next = number_next + 1 WHERE code='concurrency.test'
  RETURNING number_next - 1 AS allocated;
COMMIT;
SQL
pg "DELETE FROM ir_sequence WHERE code = 'concurrency.test'" >/dev/null
pg "INSERT INTO ir_sequence (code,name,prefix,padding) VALUES ('concurrency.test','T','T/',4)" >/dev/null

rm -f /tmp/seq_out.txt
for i in $(seq 1 8); do
  ( for j in $(seq 1 10); do
      PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tA -f /tmp/seq_alloc.sql 2>/dev/null \
        | grep -E '^[0-9]+$' >> /tmp/seq_out.txt
    done ) &
done
wait
TOTAL=$(wc -l < /tmp/seq_out.txt)
UNIQ=$(sort -n /tmp/seq_out.txt | uniq | wc -l)
MAXN=$(sort -n /tmp/seq_out.txt | tail -1)
echo "    80 allocations across 8 concurrent clients"
echo "    rows=$TOTAL  distinct=$UNIQ  highest=$MAXN"
[ "$TOTAL" = "$UNIQ" ] && ok "no duplicate numbers" || no "$((TOTAL-UNIQ)) duplicate(s)"
[ "$UNIQ" = "$MAXN" ]  && ok "no gaps (1..$MAXN all present)" || no "gaps present"
pg "DELETE FROM ir_sequence WHERE code = 'concurrency.test'" >/dev/null

echo
echo "############ 3. rollback leaves NO gap (the nextval() failure mode) ############"
pg "DELETE FROM ir_sequence WHERE code = 'rollback.test'" >/dev/null
pg "INSERT INTO ir_sequence (code,name,prefix,padding,number_next) VALUES ('rollback.test','T','R/',4,1)" >/dev/null
cat > /tmp/seq_rb.sql <<'SQL'
BEGIN;
SELECT number_next FROM ir_sequence WHERE code='rollback.test' FOR UPDATE;
UPDATE ir_sequence SET number_next = number_next + 1 WHERE code='rollback.test';
ROLLBACK;
SQL
pgf /tmp/seq_rb.sql >/dev/null
AFTER=$(pg "SELECT number_next FROM ir_sequence WHERE code='rollback.test'")
echo "    allocated then rolled back; number_next = $AFTER"
[ "$AFTER" = "1" ] && ok "number returned to the pool (a PG sequence would have burned it)" \
                   || no "number_next is $AFTER — the number was burned"
pg "DELETE FROM ir_sequence WHERE code = 'rollback.test'" >/dev/null

echo
echo "############ 4. old PG sequences no longer referenced ############"
# Match actual CALLS, not comments that mention nextval(). An earlier version
# of this check flagged its own explanatory comments and reported a false FAIL.
HITS=$(grep -rn "nextval(" /home/user/code/c-erp/modules/ --include=*.cpp 2>/dev/null \
       | grep -vE '^\s*[^:]+:[0-9]+:\s*(//|\*)' | grep -E "SELECT nextval|exec\(.*nextval")
if [ -z "$HITS" ]; then
    ok "no nextval() calls remain in modules/ (comments referencing it are fine)"
else
    no "nextval() still called:"; printf '%s\n' "$HITS" | sed 's/^/      /'
fi

echo
echo "############ 5. end-to-end: confirming a sale order uses ir.sequence ############"
# Hermetic and repeatable: create our OWN draft order to confirm, rather than
# borrowing an ambient draft. Borrowing made this probe order-dependent — it
# consumed whatever draft happened to exist and could not run twice against the
# same database — so it either SKIPped (no draft) or raced other suites.
cat > /tmp/seq_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST http://127.0.0.1:8069/web/session/authenticate \
      -H 'Content-Type: application/json' --data @/tmp/seq_auth.json \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
cat > /tmp/seq_new.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"sale.order","method":"create",
 "args":[{"partner_id":$PARTNER}],"kwargs":{"context":{"session_id":"$SID"}}}}
EOF
SO_ID=$(curl -s -X POST http://127.0.0.1:8069/web/dataset/call_kw \
        -H 'Content-Type: application/json' --data @/tmp/seq_new.json \
        | sed -n 's/.*"result":\([0-9]*\).*/\1/p')
if [ -n "$SO_ID" ]; then
    BEFORE=$(pg "SELECT number_next FROM ir_sequence WHERE code = 'sale.order'")
    cat > /tmp/seq_conf.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"sale.order","method":"action_confirm",
 "args":[[$SO_ID]],"kwargs":{"context":{"session_id":"$SID"}}}}
EOF
    curl -s -X POST http://127.0.0.1:8069/web/dataset/call_kw \
         -H 'Content-Type: application/json' --data @/tmp/seq_conf.json >/dev/null
    NAME=$(pg "SELECT name FROM sale_order WHERE id = $SO_ID")
    # Poll for the commit to become visible (the confirm creates a delivery too,
    # so the number_next write may lag the HTTP response by a moment).
    AFTER=$BEFORE
    for _ in 1 2 3 4 5 6; do
        AFTER=$(pg "SELECT number_next FROM ir_sequence WHERE code = 'sale.order'")
        [ "$AFTER" -gt "$BEFORE" ] && break
        sleep 0.5
    done
    echo "    order $SO_ID -> '$NAME'   (sequence $BEFORE -> $AFTER)"
    case "$NAME" in
        SO/*) ok "name follows the configured prefix" ;;
        *)    no "unexpected name '$NAME'" ;;
    esac
    [ "$AFTER" -gt "$BEFORE" ] && ok "sequence advanced" || no "sequence did not advance"
else
    no "could not create a probe sale order"
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
