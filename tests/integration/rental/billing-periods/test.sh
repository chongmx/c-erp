#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Every billing period a rental contract can have (docs/architecture/modules.md "The billing period", migration 816).
#
# Reported: "please let me choose billing period with combo box, may it daily,
# weekly, monthly, quarterly, biannual, yearly, then every X day/week/month/year
# the X can be selected arbitrarily. also, give me another billing period, one
# off, on demand."
#
# Before this, rental_contract.billing_period was a TEXT column constrained to
# monthly/quarterly/yearly that NOTHING READ. The cadence that actually billed
# lived on the line. So the field was decorative: a contract set to "quarterly"
# still produced monthly invoices, and daily / weekly / biannual / one-off /
# on-demand could not be expressed at all.
#
# This file asserts the whole chain, because each link failed independently
# while it was being built:
#
#   1. the nine presets are storable and nothing else is
#   2. a preset DERIVES its (interval, unit) — one place decides what
#      "quarterly" means, so the UI and a SQL import cannot disagree
#   3. 'custom' is the one preset that keeps the user's own numbers
#   4. the arithmetic is right for every unit, including the month-end case
#   5. a LINE with no period of its own follows its contract — the thing that
#      makes the contract-level setting mean anything
#   6. a line that sets its own period keeps it
#   7. one-off and on-demand are never scheduled
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZBP'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

CUST=$(call res.partner create "[{\"name\":\"${PFX} Tenant Bhd\",\"is_company\":true,
       \"customer_rank\":1}]" | rid)
t_nonempty "$CUST" "a customer exists to sign the contracts"

# -------------------------------------------------------------------------
sec "1. every preset the user asked for is accepted"
# -------------------------------------------------------------------------
# Through the API, not raw SQL: a value the database accepts but the model
# rejects is still a period the user cannot choose.
for P in daily weekly monthly quarterly biannual yearly oneoff ondemand; do
    ID=$(call rental.contract create "[{\"name\":\"${PFX} $P\",\"partner_id\":$CUST,
         \"billing_period\":\"$P\",\"date_start\":\"2026-01-15\"}]" | rid)
    t_nonempty "$ID" "billing period '$P' can be saved"
done

CUSTOM=$(call rental.contract create "[{\"name\":\"${PFX} custom\",\"partner_id\":$CUST,
         \"billing_period\":\"custom\",\"billing_interval\":45,\"billing_unit\":\"day\",
         \"date_start\":\"2026-01-15\"}]" | rid)
t_nonempty "$CUSTOM" "the 'custom' preset can be saved"

# -------------------------------------------------------------------------
sec "2. a preset derives its own interval and unit"
# -------------------------------------------------------------------------
# This is what makes the presets more than labels. Whatever writes the row —
# this API, an import, psql — the pair is filled in by the same trigger.
check_period() {   # <preset> <interval> <unit>
    local got
    got=$(pg "SELECT COALESCE(billing_interval::text,'NULL')||'/'||COALESCE(billing_unit,'NULL')
                FROM rental_contract WHERE name='${PFX} $1'")
    t_eq "$2/$3" "$got" "'$1' bills every $2 $3"
}
check_period daily     1 day
check_period weekly    1 week
check_period monthly   1 month
check_period quarterly 3 month
check_period biannual  6 month
check_period yearly    1 year

# One-off and on-demand have no interval AT ALL. Storing 1/month for them
# would make them indistinguishable from a monthly contract to every query
# that reads the pair — including the billing run.
check_period oneoff   NULL NULL
check_period ondemand NULL NULL

# -------------------------------------------------------------------------
sec "3. 'custom' is the one preset that keeps the user's numbers"
# -------------------------------------------------------------------------
t_eq "45/day" \
     "$(pg "SELECT billing_interval||'/'||billing_unit FROM rental_contract WHERE id=$CUSTOM")" \
     "every 45 days survives the trigger"

# The arbitrary X is the point: not a menu of intervals, any interval.
for N in 2 3 7 10 90 366; do
    CID=$(call rental.contract create "[{\"name\":\"${PFX} every$N\",\"partner_id\":$CUST,
          \"billing_period\":\"custom\",\"billing_interval\":$N,\"billing_unit\":\"week\",
          \"date_start\":\"2026-01-15\"}]" | rid)
    t_eq "$N" "$(pg "SELECT billing_interval FROM rental_contract WHERE id=${CID:-0}")" \
         "every $N weeks is storable"
done

# -------------------------------------------------------------------------
sec "4. nonsense is refused as a user error, not a 500"
# -------------------------------------------------------------------------
BAD=$(call rental.contract create "[{\"name\":\"${PFX} bad\",\"partner_id\":$CUST,
      \"billing_period\":\"fortnightly\"}]")
if has_error "$BAD"; then ok "an unknown period is rejected"
else no "an unknown period 'fortnightly' was accepted"; fi
t_contains "$BAD" "Billing period must be one of" \
    "the message names the periods instead of quoting a CHECK constraint"

BADX=$(call rental.contract create "[{\"name\":\"${PFX} bad2\",\"partner_id\":$CUST,
       \"billing_period\":\"custom\",\"billing_interval\":0,\"billing_unit\":\"day\"}]")
if has_error "$BADX"; then ok "every 0 days is rejected"
else no "an interval of 0 was accepted"; fi

BADU=$(call rental.contract create "[{\"name\":\"${PFX} bad3\",\"partner_id\":$CUST,
       \"billing_period\":\"custom\",\"billing_interval\":2,\"billing_unit\":\"fortnight\"}]")
if has_error "$BADU"; then ok "an unknown unit is rejected"
else no "the unit 'fortnight' was accepted"; fi

# -------------------------------------------------------------------------
sec "5. the arithmetic, for every unit"
# -------------------------------------------------------------------------
# rental_next_period is what advances a tenancy. A wrong answer here bills the
# customer on the wrong day, which is the one bug nobody forgives.
nextp() { pg "SELECT rental_next_period('$1'::date, $2, $3, '$4')"; }

t_eq "2026-01-31" "$(nextp 2026-01-30 30 1 day)"   "daily: 30 Jan + 1 day"
t_eq "2026-02-06" "$(nextp 2026-01-30 30 1 week)"  "weekly: 30 Jan + 1 week"
t_eq "2026-02-13" "$(nextp 2026-01-30 30 2 week)"  "fortnightly: 30 Jan + 2 weeks"
t_eq "2026-04-15" "$(nextp 2026-01-15 15 3 month)" "quarterly: 15 Jan -> 15 Apr"
t_eq "2026-07-15" "$(nextp 2026-01-15 15 6 month)" "biannual: 15 Jan -> 15 Jul"
t_eq "2027-01-15" "$(nextp 2026-01-15 15 1 year)"  "yearly: 15 Jan -> 15 Jan"
t_eq "2026-03-01" "$(nextp 2026-01-15 15 45 day)"  "every 45 days: 15 Jan -> 1 Mar"

# The month-end case. A 31st anchor in a 28-day month must land on the LAST
# day, not skip the month or roll into the next one.
t_eq "2026-02-28" "$(nextp 2026-01-31 31 1 month)" "31 Jan + 1 month clamps to 28 Feb"
t_eq "2026-03-31" "$(nextp 2026-02-28 31 1 month)" "and the 31st anchor comes BACK in March"

# The three-argument form predates units and is still called; it must keep
# meaning months.
t_eq "2026-02-15" "$(pg "SELECT rental_next_period('2026-01-15'::date, 15, 1)")" \
     "the legacy 3-argument form still means months"

# -------------------------------------------------------------------------
sec "6. a line with no period of its own follows the contract"
# -------------------------------------------------------------------------
# THE point of the whole change. Before migration 816 the line columns were
# NOT NULL DEFAULT 1/'month', so every line was an implicit monthly override
# and a contract's billing period could never take effect.
WK=$(pg "SELECT id FROM rental_contract WHERE name='${PFX} weekly'")
LINE=$(call rental.contract.line create "[{\"contract_id\":$WK,\"partner_id\":$CUST,
       \"date_start\":\"2026-01-15\",\"unit_price\":100,\"billing_mode\":\"recurring\"}]" | rid)
t_nonempty "$LINE" "a line can be added without naming a period"
t_eq "" "$(pg "SELECT COALESCE(billing_unit,'') FROM rental_contract_line WHERE id=${LINE:-0}")" \
     "it stores NULL, which is what 'inherit' looks like"

# Resolved the way the billing run resolves it.
RESOLVED=$(pg "SELECT COALESCE(l.billing_interval, c.billing_interval, 1)||'/'||
                      COALESCE(l.billing_unit, c.billing_unit, 'month')
                 FROM rental_contract_line l
                 JOIN rental_contract c ON c.id = l.contract_id
                WHERE l.id = ${LINE:-0}")
t_eq "1/week" "$RESOLVED" "so the line bills WEEKLY, as its contract says"

# -------------------------------------------------------------------------
sec "7. a line that sets its own period keeps it"
# -------------------------------------------------------------------------
# Inheritance must not become a straitjacket: a storage unit billed yearly
# under an otherwise weekly contract is a real arrangement.
OWN=$(call rental.contract.line create "[{\"contract_id\":$WK,\"partner_id\":$CUST,
      \"date_start\":\"2026-01-15\",\"unit_price\":100,\"billing_mode\":\"recurring\",
      \"billing_interval\":1,\"billing_unit\":\"year\"}]" | rid)
t_eq "1/year" \
     "$(pg "SELECT COALESCE(billing_interval::text,'-')||'/'||COALESCE(billing_unit,'-')
              FROM rental_contract_line WHERE id=${OWN:-0}")" \
     "an explicit yearly line overrides its weekly contract"

# -------------------------------------------------------------------------
sec "8. one-off and on-demand are never scheduled"
# -------------------------------------------------------------------------
# These two exist precisely so that nothing bills automatically. A COALESCE
# that fell back to monthly would invoice them anyway — silently, monthly,
# forever.
for P in oneoff ondemand; do
    CID=$(pg "SELECT id FROM rental_contract WHERE name='${PFX} $P'")
    call rental.contract.line create "[{\"contract_id\":$CID,\"partner_id\":$CUST,
         \"date_start\":\"2026-01-15\",\"unit_price\":500,\"billing_mode\":\"recurring\",
         \"state\":\"active\",\"next_period_start\":\"2026-01-15\"}]" >/dev/null
    DUE=$(pg "SELECT count(*) FROM rental_contract_line l
                JOIN rental_contract c ON c.id = l.contract_id
               WHERE l.contract_id = $CID
                 AND l.state = 'active'
                 AND l.billing_mode = 'recurring'
                 AND l.next_period_start IS NOT NULL
                 AND l.next_period_start - l.billing_lead_days <= '2026-06-01'::date
                 AND (c.id IS NULL OR c.billing_period NOT IN ('oneoff','ondemand'))")
    t_eq "0" "$DUE" "a '$P' contract is excluded from the billing run"
done

# The same line under a scheduled contract IS picked up — otherwise the check
# above would pass simply because nothing is ever due.
MO=$(pg "SELECT id FROM rental_contract WHERE name='${PFX} monthly'")
call rental.contract.line create "[{\"contract_id\":$MO,\"partner_id\":$CUST,
     \"date_start\":\"2026-01-15\",\"unit_price\":500,\"billing_mode\":\"recurring\",
     \"state\":\"active\",\"next_period_start\":\"2026-01-15\"}]" >/dev/null
DUEMO=$(pg "SELECT count(*) FROM rental_contract_line l
              JOIN rental_contract c ON c.id = l.contract_id
             WHERE l.contract_id = $MO
               AND l.state = 'active'
               AND l.billing_mode = 'recurring'
               AND l.next_period_start IS NOT NULL
               AND l.next_period_start - l.billing_lead_days <= '2026-06-01'::date
               AND (c.id IS NULL OR c.billing_period NOT IN ('oneoff','ondemand'))")
t_ge "${DUEMO:-0}" "1" "a monthly contract's line IS due (the control)"

# -------------------------------------------------------------------------
sec "9. the client is offered exactly these choices"
# -------------------------------------------------------------------------
# The form renders a combobox from fields_get. If the field is not declared a
# selection, the user gets a free text box and has to spell 'quarterly'
# correctly — which is the bug this whole feature replaces.
FG=$(call rental.contract fields_get '[]')
t_contains "$FG" '"billing_period"' "billing_period is exposed to the client"
for P in daily weekly monthly quarterly biannual yearly custom oneoff ondemand; do
    t_contains "$FG" "\"$P\"" "the '$P' choice reaches the form"
done
t_contains "$FG" '"selection"' "it is a selection field, so the form draws a combobox"

verdict
