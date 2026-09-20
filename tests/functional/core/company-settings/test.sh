#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Settings → ERP Settings edits the company: letterhead, bank details and the
# home currency, which is a combo box.
#
# Reported: "how to configure the company currency? I should be able to
# configure the company currency without any manual sql editing".
#
# General and Banking used to read and write ir_config_parameter rows that
# nothing had read since company identity moved onto res_company (docs/094),
# and that startup deleted. So the screen opened blank and an edit never
# reached an invoice — on production the company is "Easy Locker Space" and
# the Company Name box was empty.
#
# The journey is clicks and typing (tests/lib/render_company_settings.mjs);
# this script supplies what the screen should show and re-checks the DATABASE
# afterwards, so a driver that stopped early cannot pass by saying nothing.
#
# The refusal — no switch while posted entries are in another currency — is
# pinned at the API in tests/integration/core/company-settings. Here the switch
# must be ALLOWED, which needs a ledger with nothing posted. The meta names the
# baseline by its file path rather than `baseline`: the runner restores only
# when the scenario name changes, and by this point in a full run the
# integration tests have posted invoices into the shared baseline — which would
# make the switch refused, and this test pass or fail by suite order.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZCSF'

CID=$(pg "SELECT id FROM res_company ORDER BY id LIMIT 1")
SNAP_CO=$(pgv "SELECT format('UPDATE res_company SET reg_number=%L, bank_account_no=%L, currency_id=%s WHERE id=%s',
                 reg_number, bank_account_no, COALESCE(currency_id::text,'NULL'), id)
               FROM res_company WHERE id=$CID" | sed 's/^ *//')
SNAP_RATES=$(pgv "SELECT string_agg(format('UPDATE res_currency SET rate=%s WHERE id=%s', rate, id), '; ')
                  FROM res_currency" | sed 's/^ *//')
cleanup() {
    [ -n "$SNAP_RATES" ] && pg "$SNAP_RATES" >/dev/null 2>&1
    [ -n "$SNAP_CO" ]    && pg "$SNAP_CO"    >/dev/null 2>&1
    pg "DELETE FROM res_currency WHERE name = 'ZZK'" >/dev/null 2>&1
}
trap cleanup EXIT
auth_or_die

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}

# -------------------------------------------------------------------------
sec "1. the browser tooling, and what the screen should show"
# -------------------------------------------------------------------------
if [ ! -x "$CHROME" ]; then
    echo "    NOTE  no Chrome at $CHROME — skipping the on-screen journey"
    verdict; exit $?
fi
if [ ! -d node_modules/puppeteer-core ]; then
    echo "    NOTE  puppeteer-core is not installed — skipping the on-screen journey"
    verdict; exit $?
fi
ok "Chrome and puppeteer-core are present"

CO_NAME=$(pgv "SELECT name FROM res_company WHERE id=$CID" | sed 's/^ *//;s/ *$//')
FROM=$(pg "SELECT cur.name FROM res_company c JOIN res_currency cur ON cur.id=c.currency_id WHERE c.id=$CID")
TO=$(pg "SELECT name FROM res_currency WHERE active AND name <> '${FROM}' ORDER BY name LIMIT 1")
t_nonempty "$CO_NAME" "the company has a name to show"
t_nonempty "$FROM"    "the company has a home currency ($FROM)"
t_nonempty "$TO"      "there is another active currency to switch to ($TO)"
t_eq "0" "$(pg "SELECT count(*) FROM account_move WHERE company_id=$CID AND state='posted'")" \
     "no posted entries, so the switch is allowed"

# -------------------------------------------------------------------------
sec "2. the journey, on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/company_settings_test BASE="$BASE" DBN="$DBN" \
      timeout 300 node tests/lib/render_company_settings.mjs "$PFX" "$CO_NAME" "$FROM" "$TO" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "the company is edited from Settings, currency by combo box"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the database, checked independently of the driver"
# -------------------------------------------------------------------------
t_eq "$TO"             "$(pg "SELECT cur.name FROM res_company c JOIN res_currency cur ON cur.id=c.currency_id WHERE c.id=$CID")" \
     "res_company.currency_id is now $TO"
t_eq "${PFX}-REG-1"    "$(pg "SELECT reg_number FROM res_company WHERE id=$CID")"      "res_company.reg_number was written"
t_eq "${PFX}-ACCT-1"   "$(pg "SELECT bank_account_no FROM res_company WHERE id=$CID")" "res_company.bank_account_no was written"
t_eq "1000000"         "$(pg "SELECT rate FROM res_currency WHERE name='$TO'")"         "$TO, the new home currency, has rate 1.0"
# The old home: nothing typed on General or Banking may land in the config
# table again — that is the dead end this replaced.
t_eq "0" "$(pg "SELECT count(*) FROM ir_config_parameter
                WHERE key LIKE 'company.%' OR key IN ('report.reg_number','report.currency_code',
                      'report.bank.account_no','report.payment_term_days')")" \
     "no company identity was written to ir_config_parameter"
# The currency added through ＋ on the screen.
t_eq "ZZK|Z\$|1" "$(pg "SELECT name || '|' || symbol || '|' || active::int FROM res_currency WHERE name='ZZK'")" \
     "the currency added on screen exists, active, with its symbol"
t_eq "2500000" "$(pg "SELECT rate FROM res_currency WHERE name='ZZK'")" "at the rate that was typed (2.5)"

verdict
