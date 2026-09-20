#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The company record as Settings edits it: letterhead, bank details and the
# home currency (docs/094).
#
# Reported: "how to configure the company currency? I should be able to
# configure the company currency without any manual sql editing".
#
# Looking for where to put that exposed a worse fault. ERP Settings → General
# and Banking still read and wrote ir_config_parameter rows. Since docs/094
# every document reads res_company instead, and startup DELETES those rows —
# so the screen showed blank fields, and whatever was typed there never
# reached an invoice. res.company did not even register the letterhead
# columns, so there was no way to write them through the API at all.
#
# What this file pins down:
#   1. res.company exposes the letterhead, bank and currency fields;
#   2. a write reaches the printed document, and a rename reaches the
#      company's own contact;
#   3. the home currency can be changed through the API — the production case
#      (company left on USD, books all kept in MYR) — and the rates are
#      re-expressed against the new home currency;
#   4. the change is REFUSED while a posted entry is in another currency,
#      because that would relabel the ledger's numbers, not convert them.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

CID=$(pg "SELECT id FROM res_company ORDER BY id LIMIT 1")
MYR=$(pg "SELECT id FROM res_currency WHERE name='MYR'")
USD=$(pg "SELECT id FROM res_currency WHERE name='USD'")
EUR=$(pg "SELECT id FROM res_currency WHERE name='EUR'")
INV=$(pg "SELECT id FROM account_move WHERE name='FX-INV-1'")

# Everything this test touches is put back exactly — it borrows the company,
# the shared rate table and the fixture invoice, and owns none of them.
SNAP_CO=$(pgv "SELECT format('UPDATE res_company SET name=%L, reg_number=%L, street=%L,
                 bank_account_no=%L, payment_term_days=%s, currency_id=%s WHERE id=%s',
                 name, reg_number, street, bank_account_no,
                 COALESCE(payment_term_days::text,'NULL'), COALESCE(currency_id::text,'NULL'), id)
               FROM res_company WHERE id=$CID" | sed 's/^ *//')
SNAP_PARTNER=$(pgv "SELECT format('UPDATE res_partner SET name=%L WHERE id=%s', p.name, p.id)
                    FROM res_company c JOIN res_partner p ON p.id=c.partner_id WHERE c.id=$CID" | sed 's/^ *//')
SNAP_RATES=$(pgv "SELECT string_agg(format('UPDATE res_currency SET rate=%s WHERE id=%s', rate, id), '; ')
                  FROM res_currency" | sed 's/^ *//')
SNAP_INV=$(pgv "SELECT format('UPDATE account_move SET state=%L, currency_id=%s WHERE id=%s',
                  state, COALESCE(currency_id::text,'NULL'), id)
                FROM account_move WHERE id=${INV:-0}" | sed 's/^ *//')
cleanup() {
    [ -n "$SNAP_INV" ]     && pg "$SNAP_INV"     >/dev/null 2>&1
    [ -n "$SNAP_RATES" ]   && pg "$SNAP_RATES"   >/dev/null 2>&1
    [ -n "$SNAP_CO" ]      && pg "$SNAP_CO"      >/dev/null 2>&1
    [ -n "$SNAP_PARTNER" ] && pg "$SNAP_PARTNER" >/dev/null 2>&1
}
trap cleanup EXIT
auth_or_die

t_nonempty "$CID" "there is a company"
t_nonempty "$SNAP_CO" "the company was snapshotted (the restore depends on it)"
t_nonempty "$SNAP_RATES" "the rates were snapshotted"

# -------------------------------------------------------------------------
sec "1. res.company exposes what Settings edits"
# -------------------------------------------------------------------------
FG=$(call res.company fields_get '[]')
for f in reg_number street street2 street3 city_country bank_name bank_account_name \
         bank_account_no bank_address bank_swift payment_term_days currency_id; do
    if printf '%s' "$FG" | grep -q "\"$f\""; then ok "field $f is registered"
    else no "field $f is NOT registered — Settings cannot write it"; fi
done
t_contains "$FG" '"res.currency"' "currency_id is a picker over res.currency"

# -------------------------------------------------------------------------
sec "2. a write reaches the document, and a rename reaches the contact"
# -------------------------------------------------------------------------
R=$(call res.company write "[[${CID}], {\"reg_number\":\"ZZCS-REG-7\", \"street\":\"ZZCS 1 Jalan Ujian\",
      \"bank_account_no\":\"ZZCS-ACCT-99\", \"payment_term_days\":45, \"name\":\"ZZCS Holdings\"}]")
has_error "$R" && no "the write was refused: $R" || ok "the write was accepted"
t_eq "ZZCS-REG-7"    "$(pg "SELECT reg_number FROM res_company WHERE id=$CID")"       "reg_number is stored on the company"
t_eq "ZZCS-ACCT-99"  "$(pg "SELECT bank_account_no FROM res_company WHERE id=$CID")"  "bank_account_no is stored on the company"
t_eq "45"            "$(pg "SELECT payment_term_days FROM res_company WHERE id=$CID")" "payment_term_days is stored on the company"

RD=$(call res.company read "[[${CID}], [\"reg_number\",\"street\",\"payment_term_days\"]]")
t_contains "$RD" 'ZZCS 1 Jalan Ujian' "read returns the letterhead line"

PN=$(pgv "SELECT p.name FROM res_company c JOIN res_partner p ON p.id=c.partner_id WHERE c.id=$CID" | sed 's/^ *//;s/ *$//')
t_eq "ZZCS Holdings" "$PN" "the company's own contact carries the new name straight away"

if [ -n "$INV" ]; then
    CODE=$(curl -s -H "Cookie: session_id=$SID" -o /tmp/cs_inv.html -w '%{http_code}' \
           "$BASE/report/html/account.move/$INV")
    t_eq "200" "$CODE" "the invoice renders"
    t_contains "$(cat /tmp/cs_inv.html)" 'ZZCS-REG-7'   "the printed invoice shows the new registration number"
    t_contains "$(cat /tmp/cs_inv.html)" 'ZZCS-ACCT-99' "and the new bank account number"
else
    no "no fixture invoice FX-INV-1 — the document check cannot run"
fi

# -------------------------------------------------------------------------
sec "3. the home currency changes through the API, and the rates follow"
# -------------------------------------------------------------------------
# The production case: company left on USD while every posted entry is MYR.
# Rates as they would stand with USD as home: 1 MYR = 0.25 USD, 1 EUR = 1.10.
t_eq "1" "$(pg "SELECT count(*) FROM res_company")" "one company (so the shared rates may be rebased)"
pg "UPDATE res_company SET currency_id=$USD WHERE id=$CID" >/dev/null
pg "UPDATE res_currency SET rate=1000000 WHERE id=$USD; UPDATE res_currency SET rate=250000 WHERE id=$MYR;
    UPDATE res_currency SET rate=1100000 WHERE id=$EUR" >/dev/null
[ -n "$INV" ] && pg "UPDATE account_move SET state='posted', currency_id=$MYR WHERE id=$INV" >/dev/null
t_eq "$USD" "$(pg "SELECT currency_id FROM res_company WHERE id=$CID")" "precondition: the company is on USD"

R=$(call res.company write "[[${CID}], {\"currency_id\": ${MYR}}]")
has_error "$R" && no "switching to MYR was refused: $R" || ok "switching to MYR was accepted — every posted entry is already MYR"
t_eq "$MYR" "$(pg "SELECT currency_id FROM res_company WHERE id=$CID")" "the company is now on MYR"
t_eq "1000000" "$(pg "SELECT rate FROM res_currency WHERE id=$MYR")" "MYR, the new home currency, is 1.0"
t_eq "4000000" "$(pg "SELECT rate FROM res_currency WHERE id=$USD")" "USD is re-expressed: 1 USD = 4.00 MYR"
t_eq "4400000" "$(pg "SELECT rate FROM res_currency WHERE id=$EUR")" "EUR is re-expressed: 1 EUR = 4.40 MYR"

# Writing the currency it already has is not a change: no guard, no rebase.
R=$(call res.company write "[[${CID}], {\"currency_id\": ${MYR}}]")
has_error "$R" && no "re-saving the same currency was refused: $R" || ok "re-saving the same currency is accepted"
t_eq "4000000" "$(pg "SELECT rate FROM res_currency WHERE id=$USD")" "and leaves the rates alone"

# -------------------------------------------------------------------------
sec "4. refused while a posted entry is in another currency"
# -------------------------------------------------------------------------
if [ -n "$INV" ]; then
    R=$(call res.company write "[[${CID}], {\"currency_id\": ${USD}}]")
    if has_error "$R"; then ok "switching to USD is refused — a posted entry is in MYR"
    else no "switching to USD was ACCEPTED with a posted MYR entry on the books"; fi
    t_contains "$R" 'posted entries' "the refusal says why"
    t_contains "$R" 'in MYR' "and names the currency the entries are in"
    t_eq "$MYR" "$(pg "SELECT currency_id FROM res_company WHERE id=$CID")" "the company is still on MYR"
    t_eq "4000000" "$(pg "SELECT rate FROM res_currency WHERE id=$USD")" "the rates are untouched"

    # A draft does not count: nothing is booked until it is posted.
    pg "UPDATE account_move SET state='draft' WHERE id=$INV" >/dev/null
    t_eq "0" "$(pg "SELECT count(*) FROM account_move WHERE company_id=$CID AND state='posted'
                    AND COALESCE(currency_id,$MYR) <> $USD")" "precondition: no posted non-USD entry remains"
    R=$(call res.company write "[[${CID}], {\"currency_id\": ${USD}}]")
    has_error "$R" && no "switching with only drafts was refused: $R" || ok "with only drafts on the books the switch is allowed"
else
    no "no fixture invoice — the refusal cannot be tested"
fi

R=$(call res.company write "[[${CID}], {\"currency_id\": 999999}]")
has_error "$R" && ok "a currency that does not exist is refused" || no "a nonexistent currency id was accepted"

# -------------------------------------------------------------------------
sec "4b. a currency can be added, and one not in use can be chosen"
# -------------------------------------------------------------------------
# "it should have a drop down ... similar to the customer picker. It displays a
# list of frequently used currency and then allow user to create new or search."
pg "DELETE FROM res_currency WHERE name IN ('ZZQ','ZZR')" >/dev/null
NEW=$(call res.currency create '[{"name":"zzq","symbol":"Z$","rate":2.5,"decimal_places":2,"active":true}]' | rid)
t_nonempty "$NEW" "a currency is created from the screen's ＋"
t_eq "ZZQ" "$(pg "SELECT name FROM res_currency WHERE id=${NEW:-0}")" "its code is stored in upper case"
R=$(call res.currency create '[{"name":"zz","symbol":"x","rate":1}]')
has_error "$R" && ok "a code that is not three letters is refused" || no "'zz' was accepted as a currency"
R=$(call res.currency create '[{"name":"ZZQ","symbol":"x","rate":1}]')
has_error "$R" && ok "a code that already exists is refused" || no "a duplicate currency was created"
t_contains "$R" "search for it instead" "and says to search for it instead"
R=$(call res.currency create '[{"name":"ZZR","symbol":"x","rate":0}]')
has_error "$R" && ok "a rate of zero is refused" || no "a currency with rate 0 was created"

# The picker offers currencies that are not switched on; choosing one switches
# it on, because the books cannot be kept in a currency the app hides.
pg "UPDATE res_currency SET active=false WHERE id=${NEW:-0}" >/dev/null
pg "UPDATE account_move SET state='draft' WHERE company_id=$CID AND state='posted'" >/dev/null
R=$(call res.company write "[[${CID}], {\"currency_id\": ${NEW:-0}}]")
has_error "$R" && no "choosing a currency that is not in use failed: $R" || ok "a currency not in use can be chosen as the home currency"
t_eq "1" "$(pg "SELECT active::int FROM res_currency WHERE id=${NEW:-0}")" "and choosing it switches it on"
pg "DELETE FROM res_currency WHERE name IN ('ZZQ','ZZR')" >/dev/null 2>&1

# -------------------------------------------------------------------------
sec "5. the whole settings page saves in one call"
# -------------------------------------------------------------------------
# Reported: "Settings -> ALL: After clicking 'Save Changes' the button got
# stuck at 'Saving'." It wrote one JSON-RPC call PER SETTING — twenty-six
# round trips, and not atomic: a failure halfway left the page half-saved,
# and a cached row id could write to a row startup had deleted.
K1="report.paper_format"
K2="mail.smtp_host"
WAS1=$(pgv "SELECT COALESCE(value,'') FROM ir_config_parameter WHERE key='$K1'" | sed 's/^ *//;s/ *$//')
WAS2=$(pgv "SELECT COALESCE(value,'') FROM ir_config_parameter WHERE key='$K2'" | sed 's/^ *//;s/ *$//')
pg "DELETE FROM ir_config_parameter WHERE key='ZZCS.made.up'" >/dev/null

R=$(call ir.config.parameter set_params "[{\"$K1\":\"Legal\",\"$K2\":\"smtp.zzcs.test\",\"ZZCS.made.up\":\"42\"}]")
has_error "$R" && no "set_params failed: $R" || ok "one call saves a page of settings"
t_eq "Legal"          "$(pg "SELECT value FROM ir_config_parameter WHERE key='$K1'")"        "an existing setting is updated"
t_eq "smtp.zzcs.test" "$(pg "SELECT value FROM ir_config_parameter WHERE key='$K2'")"        "and another beside it"
t_eq "42"             "$(pg "SELECT value FROM ir_config_parameter WHERE key='ZZCS.made.up'")" "a setting that did not exist is created"
t_eq "1" "$(pg "SELECT count(*) FROM ir_config_parameter WHERE key='ZZCS.made.up'")" "exactly once — the key is unique, so saving twice does not duplicate"
call ir.config.parameter set_params "[{\"ZZCS.made.up\":\"43\"}]" >/dev/null
t_eq "43" "$(pg "SELECT value FROM ir_config_parameter WHERE key='ZZCS.made.up'")" "saving again overwrites it"

R=$(call ir.config.parameter set_params '[[1,2]]')
has_error "$R" && ok "a list instead of key/value pairs is refused" || no "set_params took a list"
R=$(call ir.config.parameter set_params '[{"ZZCS.bad":{"a":1}}]')
has_error "$R" && ok "a value that is not text, a number or a boolean is refused" || no "an object value was stored"
t_eq "0" "$(pg "SELECT count(*) FROM ir_config_parameter WHERE key='ZZCS.bad'")" "and nothing was written"
call ir.config.parameter set_params "[{\"ZZCS.numeric\":7,\"ZZCS.flag\":true}]" >/dev/null
t_eq "7|True" "$(pg "SELECT (SELECT value FROM ir_config_parameter WHERE key='ZZCS.numeric') || '|' ||
                            (SELECT value FROM ir_config_parameter WHERE key='ZZCS.flag')")" \
     "a number and a boolean are stored as text"

pg "DELETE FROM ir_config_parameter WHERE key LIKE 'ZZCS.%'" >/dev/null
[ -n "$WAS1" ] && call ir.config.parameter set_params "[{\"$K1\":\"$WAS1\"}]" >/dev/null
[ -n "$WAS2" ] && call ir.config.parameter set_params "[{\"$K2\":\"$WAS2\"}]" >/dev/null

verdict
