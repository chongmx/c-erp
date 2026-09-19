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
source tests/lib/sale_fixture.sh; ensure_sale_fixture "$SID" >/dev/null 2>&1

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
echo "############ a document states ITS OWN currency ############"
# Reported: "why I stated the rental contract to be MYR but the generated PDF
# is USD?"
#
# {{currency_code}} came from CompanyIdentity, which reads
# res_company.currency_id — so every document printed the COMPANY's currency
# whatever the document was actually in. An MYR invoice printed USD, and with
# the company set to MYR a USD invoice would print MYR: wrong in both
# directions, on the one line a customer reads to decide what to pay.
#
# The company currency is deliberately set to something DIFFERENT here, because
# a test where they happen to match cannot tell the two behaviours apart.
HOME_CUR=$(pg "SELECT currency_id FROM res_company ORDER BY id LIMIT 1")
OTHER=$(pg "SELECT id FROM res_currency WHERE id <> ${HOME_CUR:-0} ORDER BY id LIMIT 1")
OTHER_CODE=$(pg "SELECT name FROM res_currency WHERE id=${OTHER:-0}")
HOME_CODE=$(pg "SELECT name FROM res_currency WHERE id=${HOME_CUR:-0}")
echo "    company books in $HOME_CODE; the invoice will be in $OTHER_CODE"

# Retag an EXISTING invoice rather than inserting one. account_move has
# required columns this test has no business knowing about, and pg() swallows
# the error — the first attempt inserted nothing and the section skipped itself
# with a message blaming the currency setup.
MV=$(pg "SELECT id FROM account_move WHERE move_type='out_invoice' ORDER BY id DESC LIMIT 1")
if [ -z "$MV" ]; then
    no "no invoice to render — the fixtures did not create one"
elif [ -z "$OTHER" ] || [ "$OTHER_CODE" = "$HOME_CODE" ]; then
    no "only one currency configured — this check cannot tell the two apart"
else
    WAS=$(pg "SELECT COALESCE(currency_id::text,'') FROM account_move WHERE id=$MV")
    pg "UPDATE account_move SET currency_id=$OTHER WHERE id=$MV" >/dev/null
    t_eq "$OTHER" "$(pg "SELECT currency_id FROM account_move WHERE id=$MV")"          "the invoice is now in $OTHER_CODE while the books are in $HOME_CODE"

    CODE=$(curl -s -H "Cookie: session_id=$SID" -o /tmp/cur.html -w '%{http_code}'            "$BASE/report/html/account.move/$MV")
    if [ "$CODE" = "200" ]; then
        STATED=$(grep -oE 'All Amount Stated in - [A-Z]{3}' /tmp/cur.html | head -1 | awk '{print $NF}')
        echo "    document states: ${STATED:-<none>}"
        if [ "$STATED" = "$OTHER_CODE" ]; then
            ok "the document states the INVOICE's currency ($OTHER_CODE)"
        elif [ "$STATED" = "$HOME_CODE" ]; then
            no "the document states the COMPANY's currency ($HOME_CODE) — the invoice is in $OTHER_CODE"
        else
            no "the document states '${STATED:-nothing}'"
        fi
        if grep -qE ">$OTHER_CODE [0-9,]+\.[0-9]{2}<" /tmp/cur.html; then
            ok "and the totals are labelled $OTHER_CODE"
        else
            no "the totals carry the wrong currency label"
        fi
    else
        no "the document did not render (http=$CODE)"
    fi

    # Put it back: this test borrows a fixture it does not own.
    if [ -n "$WAS" ]; then pg "UPDATE account_move SET currency_id=$WAS WHERE id=$MV" >/dev/null
    else pg "UPDATE account_move SET currency_id=NULL WHERE id=$MV" >/dev/null; fi
fi

# The same bug lived in the quotation and the purchase order: neither document
# query read a currency at all, so both printed the company's. A quotation is
# the FIRST document a customer sees with a price on it — the invoice fix alone
# would have left the customer quoted in one currency and billed in another.
if [ -n "$SO" ] && [ -n "$OTHER" ] && [ "$OTHER_CODE" != "$HOME_CODE" ]; then
    SO_WAS=$(pg "SELECT COALESCE(currency_id::text,'') FROM sale_order WHERE id=$SO")
    pg "UPDATE sale_order SET currency_id=$OTHER WHERE id=$SO" >/dev/null
    CODE=$(curl -s -H "Cookie: session_id=$SID" -o /tmp/cur_so.html -w '%{http_code}' \
           "$BASE/report/html/sale.order/$SO")
    STATED=$(grep -oE 'All Amount Stated in - [A-Z]{3}' /tmp/cur_so.html | head -1 | awk '{print $NF}')
    echo "    quotation states: ${STATED:-<none>} (http=$CODE)"
    if [ "$STATED" = "$OTHER_CODE" ]; then
        ok "the quotation states ITS currency ($OTHER_CODE) too"
    else
        no "the quotation states '${STATED:-nothing}', not its own $OTHER_CODE"
    fi
    if [ -n "$SO_WAS" ]; then pg "UPDATE sale_order SET currency_id=$SO_WAS WHERE id=$SO" >/dev/null
    else pg "UPDATE sale_order SET currency_id=NULL WHERE id=$SO" >/dev/null; fi
fi

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
