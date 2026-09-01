#!/bin/bash
# Diagnostic: how does call_kw resolve a session? (cookie vs body context)
BASE=http://127.0.0.1:8069

cat > /tmp/d_auth.json <<'EOF'
{"jsonrpc":"2.0","method":"call","params":{"db":"odoo","login":"admin","password":"admin"}}
EOF
A=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' --data @/tmp/d_auth.json)
SID=$(printf '%s' "$A" | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
echo "SID=[$SID]"
[ -z "$SID" ] && { echo "auth failed: $A"; exit 1; }

probe() {
    echo "--- $1 ---"
    shift
    printf '    %s\n' "$(curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' "$@" | head -c 180)"
}

cat > /tmp/d_plain.json <<'EOF'
{"jsonrpc":"2.0","method":"call","params":{"model":"product.category","method":"search_read","args":[[]],"kwargs":{"fields":["id","name"],"limit":1}}}
EOF

cat > /tmp/d_ctx.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"product.category","method":"search_read","args":[[]],"kwargs":{"fields":["id","name"],"limit":1,"context":{"session_id":"$SID"}}}}
EOF

probe "A. Cookie header, capital C"  -H "Cookie: session_id=$SID"      --data @/tmp/d_plain.json
probe "B. cookie header, lowercase"  -H "cookie: session_id=$SID"      --data @/tmp/d_plain.json
probe "C. body context.session_id"                                     --data @/tmp/d_ctx.json
probe "D. both cookie + context"     -H "Cookie: session_id=$SID"      --data @/tmp/d_ctx.json

echo "--- E. GET route with same cookie (control) ---"
printf '    http=%s\n' "$(curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=$SID" "$BASE/report/pdf/sale.order/2")"

echo "--- server log (sid seen per request) ---"
tail -8 /home/user/code/c-erp/log/system.log | sed 's/^/    /'
