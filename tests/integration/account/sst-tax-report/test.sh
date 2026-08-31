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
# Tax Report — Malaysian SST-02 (docs/083).
#
# Output tax collected per tax code (customer invoices + credit notes),
# grouped into Sales Tax / Service Tax over a taxable period. The load-bearing
# assertion is the cross-check against the ledger: the report's Total Tax
# Payable must equal the net of the posted output tax lines.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":{\"context\":{\"session_id\":\"$SID\"}}}}"; }
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }

echo "############ post a Service Tax 8% invoice (so the return is non-zero) ############"
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
JSALES=$(pg "SELECT id FROM account_journal WHERE type='sale' ORDER BY id LIMIT 1")
INCOME=$(pg "SELECT id FROM account_account WHERE account_type='income' ORDER BY id LIMIT 1")
AR=$(pg "SELECT id FROM account_account WHERE account_type='asset_receivable' ORDER BY id LIMIT 1")
STAX=$(pg "SELECT id FROM account_tax WHERE tax_group='service' AND active ORDER BY id LIMIT 1")
TODAY=$(date +%Y-%m-%d)
MV=$(call account.move create "[{\"move_type\":\"out_invoice\",\"partner_id\":$PARTNER,\"journal_id\":$JSALES,\"state\":\"draft\",\"date\":\"$TODAY\",\"invoice_date\":\"$TODAY\"}]" | rid)
if [ -n "$MV" ] && [ -n "$STAX" ]; then
  call account.move.line create "[{\"move_id\":$MV,\"account_id\":$INCOME,\"name\":\"Consulting service\",\"date\":\"$TODAY\",\"quantity\":1,\"price_unit\":1000,\"credit\":1000,\"debit\":0,\"tax_ids_json\":\"[$STAX]\",\"display_type\":\"\"}]" >/dev/null
  call account.move.line create "[{\"move_id\":$MV,\"account_id\":$AR,\"name\":\"Receivable\",\"date\":\"$TODAY\",\"quantity\":1,\"price_unit\":0,\"credit\":0,\"debit\":1000,\"display_type\":\"\"}]" >/dev/null
  call account.move recompute_totals "[[$MV]]" >/dev/null
  call account.move action_post "[[$MV]]" >/dev/null
  TAXLINE=$(pg "SELECT COALESCE(SUM(credit),0) FROM account_move_line WHERE move_id=$MV AND tax_line_id=$STAX")
  [ "$TAXLINE" = "80000000" ] && ok "invoice posts with RM80 service tax (8% of RM1,000)" || no "service tax line = $TAXLINE (expected 80000000)"
else
  echo "    (could not set up a taxed invoice — running the correctness check only)"
fi

echo "############ SST taxes are seeded & classified ############"
[ "$(pg "SELECT count(*) FROM account_tax WHERE tax_group='service'")" -ge 1 ] && ok "a Service Tax exists (tax_group=service)" || no "no service tax"
[ "$(pg "SELECT count(*) FROM account_tax WHERE tax_group='sales'")"   -ge 1 ] && ok "a Sales Tax exists (tax_group=sales)"     || no "no sales tax"

echo "############ report renders ############"
curl -s -H "Cookie: session_id=$SID" "$BASE/web/account/report?report=tax_report&date_from=2000-01-01&date_to=2099-12-31" > /tmp/sst.json
grep -q '"report":"tax_report"' /tmp/sst.json && ok "tax_report renders" || no "tax_report failed: $(head -c 120 /tmp/sst.json)"

echo "############ cross-check: report total == ledger output tax ############"
# Net output tax = SUM(credit-debit) on posted tax lines of customer invoices/credit notes.
LEDGER=$(pg "SELECT COALESCE(SUM(aml.credit - aml.debit),0) FROM account_move_line aml JOIN account_move m ON m.id=aml.move_id WHERE m.state='posted' AND m.move_type IN ('out_invoice','out_refund') AND aml.tax_line_id IS NOT NULL")
python3 - "$LEDGER" <<'PY'
import sys, json
ledger_micros = int(sys.argv[1] or 0)
d = json.load(open('/tmp/sst.json'))
tot = [r for r in d['rows'] if r['cells'][0] == 'Total Tax Payable']
report_tax = float(tot[0]['cells'][3].replace(',', '')) if tot else -1
ledger_major = ledger_micros / 1_000_000.0
print("    report Total Tax Payable = %.2f   ledger output tax = %.2f" % (report_tax, ledger_major))
if abs(report_tax - ledger_major) < 0.01:
    print("    PASS  report total matches the ledger")
else:
    print("    FAIL  report %.2f != ledger %.2f" % (report_tax, ledger_major)); sys.exit(1)
# base * rate == tax on each line (SST is single-stage)
bad = 0
for r in d['rows']:
    if r['type'] != 'line': continue
    rate = float(r['cells'][1].replace('%','').replace(',','') or 0)
    base = float(r['cells'][2].replace(',','') or 0)
    tax  = float(r['cells'][3].replace(',','') or 0)
    if rate and abs(base*rate/100.0 - tax) > 0.02:
        print("    FAIL  %s: base %.2f x %.2f%% = %.2f != tax %.2f" % (r['cells'][0],base,rate,base*rate/100.0,tax)); bad+=1
if bad==0: print("    PASS  every line reconciles: taxable amount x rate == tax")
sys.exit(1 if bad else 0)
PY
[ $? -ne 0 ] && FAILED=1
rm -f /tmp/sst.json

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
