#!/bin/bash
# =============================================================
# Demo facility — create or remove.
#
#   ./scripts/seed.sh rental             create
#   ./scripts/seed.sh rental --clear     remove
#   ./scripts/seed.sh rental --status    what exists right now
#
# This is a THIN WRAPPER over /rental/demo/*. The SQL lives in
# modules/rental/RentalDemo.cpp and nowhere else.
#
# It used to carry its own copy of the seed data. Two definitions of
# "what the demo facility is" drift the moment a column changes, and the
# shell copy is the one nobody remembers to update — it had already
# fallen behind once, creating every tenancy as a walk-in after
# billing_mode was added, which showed as zero MRR on a visibly
# 40%-occupied facility.
#
# The same operations are in the UI at Settings -> Technical -> Demo Data.
# =============================================================
set -uo pipefail

BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
LOGIN=${LOGIN:-admin}
PASSWORD=${PASSWORD:-admin}

ACTION=seed
case "${1:-}" in
    --clear)  ACTION=clear  ;;
    --status) ACTION=status ;;
    "")       ACTION=seed   ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
esac

# The endpoints authenticate, like every other route that touches
# business data.
cat > /tmp/seed_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"$LOGIN","password":"$PASSWORD"}}
EOF
if ! curl -sf -o /dev/null --max-time 5 "$BASE/healthz"; then
    echo "ERROR: no server at $BASE — start it with ./scripts/server.sh --start" >&2
    exit 1
fi
curl -s -c /tmp/seed_cookie.txt -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/seed_auth.json > /tmp/seed_auth_out.json
if ! grep -q '"session_id"' /tmp/seed_auth_out.json; then
    echo "ERROR: could not authenticate as '$LOGIN'." >&2
    head -c 200 /tmp/seed_auth_out.json >&2; echo >&2
    exit 1
fi

case "$ACTION" in
    status) OUT=$(curl -s -b /tmp/seed_cookie.txt "$BASE/rental/demo/status") ;;
    seed)   OUT=$(curl -s -b /tmp/seed_cookie.txt -X POST "$BASE/rental/demo/seed") ;;
    clear)  OUT=$(curl -s -b /tmp/seed_cookie.txt -X POST "$BASE/rental/demo/clear") ;;
esac

printf '%s' "$OUT" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('unexpected response:'); print(sys.stdin.read()[:300]); sys.exit(1)
if 'error' in d:
    print('ERROR:', d['error']); sys.exit(1)

if 'created' in d:
    c = d['created']
    if any(c.values()):
        print('Created: %d unit(s), %d tenancy(ies), %d expense budget(s)'
              % (c.get('units',0), c.get('tenancies',0), c.get('expense_templates',0)))
    else:
        print('Nothing to create — the demo facility was already complete.')
if 'removed' in d:
    r = d['removed']
    print('Removed: %d unit(s), %d tenancy(ies), %d contract(s), %d expense row(s)'
          % (r.get('units',0), r.get('tenancies',0), r.get('contracts',0),
             r.get('expense_templates',0) + r.get('expense_entries',0)))

print()
print('Now in the database:')
for k, label in [('units','units'), ('tenancies','tenancies'),
                 ('contracts','contracts'),
                 ('expense_templates','recurring expense budgets'),
                 ('expense_entries','expense entries generated'),
                 ('invoices','invoices generated (kept)')]:
    print('  %-28s %s' % (label, d.get(k, 0)))
"
rc=$?
[ "$rc" -eq 0 ] && [ "$ACTION" = seed ] && \
    echo && echo "Open Rental -> Operations -> Dashboard."
rm -f /tmp/seed_auth.json /tmp/seed_auth_out.json /tmp/seed_cookie.txt
exit $rc
