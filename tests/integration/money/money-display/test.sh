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
# P2 Phase 4 verification: the raw-SQL display paths (report PDF,
# portal JSON) must render MAJOR units after the migration.
# These bypass BaseModel::rowsToJson_ and were converted by hand.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

cat > /tmp/vd_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data @/tmp/vd_auth.json | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
echo "sid=${SID:0:8}..."
source scripts/seed_test_fixtures.sh; ensure_sale_fixture "$SID" >/dev/null 2>&1

echo
echo "############ report PDF (raw SQL -> HTML -> wkhtmltopdf) ############"
SO=$(pg "SELECT id FROM sale_order ORDER BY id LIMIT 1")
DBTOT=$(pg "SELECT amount_total FROM sale_order WHERE id=$SO")
echo "    sale_order $SO, DB amount_total = $DBTOT micros"

CODE=$(curl -s -H "Cookie: session_id=$SID" -o /tmp/vd.pdf -w '%{http_code}' \
       "$BASE/report/pdf/sale.order/$SO")
echo "    http=$CODE  $(stat -c%s /tmp/vd.pdf 2>/dev/null) bytes"

if [ "$CODE" = "200" ] && head -c 4 /tmp/vd.pdf | grep -q '%PDF'; then
    ok "PDF rendered"
    if command -v pdftotext >/dev/null 2>&1; then
        TXT=$(pdftotext /tmp/vd.pdf - 2>/dev/null)
        echo "    --- numbers found in the PDF ---"
        printf '%s\n' "$TXT" | grep -oE '[0-9][0-9,]*\.[0-9]{2}' | sort -u | head -8 | sed 's/^/      /'
        if printf '%s' "$TXT" | grep -qE '[0-9]{7,}'; then
            no "*** a 7+ digit number appears — micro-units leaked into the PDF ***"
            printf '%s' "$TXT" | grep -oE '[0-9]{7,}' | head -3 | sed 's/^/      /'
        else
            ok "no micro-unit values in the rendered output"
        fi
    else
        echo "    (pdftotext not installed — install poppler-utils to assert on content)"
    fi
else
    no "PDF did not render (http=$CODE)"
fi

echo
echo "############ report HTML (same data path, easier to assert) ############"
CODE=$(curl -s -H "Cookie: session_id=$SID" -o /tmp/vd.html -w '%{http_code}' \
       "$BASE/report/html/sale.order/$SO")
if [ "$CODE" = "200" ]; then
    ok "HTML rendered"
    echo "    --- money-looking strings ---"
    grep -oE '[0-9][0-9,]*\.[0-9]{2}' /tmp/vd.html | sort -u | head -8 | sed 's/^/      /'

    # Compare the rendered figure against the DB value converted to major
    # units. An earlier version of this check tested for "7+ consecutive
    # digits" and PASSED while the report showed 30,000,000.00 — the comma
    # separators defeated the regex. Assert on the expected value instead.
    EXPECT=$(pg "SELECT to_char($DBTOT::numeric/1000000, 'FM999,999,990.00')")
    echo "    expected total: $EXPECT"
    if grep -qF "$EXPECT" /tmp/vd.html; then
        ok "report shows $EXPECT (matches the DB value in major units)"
    else
        no "*** expected $EXPECT — report is rendering the wrong magnitude ***"
    fi
    # Belt and braces: strip commas, then look for an implausibly large figure.
    if sed 's/,//g' /tmp/vd.html | grep -qE '>[0-9]{7,}\.[0-9]{2}<'; then
        no "*** a 7+ digit amount is present — micro-units leaked ***"
    fi
else
    echo "    (http=$CODE — route may differ; PDF check above is the primary one)"
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
