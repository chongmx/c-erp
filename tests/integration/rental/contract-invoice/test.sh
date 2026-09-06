#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# rental.contract.action_create_invoice — invoicing ONE contract on demand.
#
# Asked for: "for each rental contract created and active, I want a method of
# creating invoice, similar to how sales order let me create invoice."
#
# It runs RentalBilling scoped to the contract rather than a second
# implementation, because a manual path that drifts from the scheduled one is
# how double-billing is found in production (RentalBilling.hpp).
#
# Asking for one contract also relaxes two filters, and this file pins exactly
# how far:
#
#   * a One off / On demand CONTRACT bills here, and only here. The cron skips
#     them by design — "on demand" means nothing until somebody demands it, and
#     until now there was nowhere to do the demanding.
#   * a line written billing_mode='oneoff' bills here. The Booking calendar
#     writes dated bookings that way so the recurring engine leaves them alone;
#     nothing else billed them either, so a booking could never be invoiced.
#
# What must NOT be relaxed, and is checked below: the period gate, idempotency,
# and the scope itself — invoicing contract A must not touch contract B.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZCI'
cleanup() {
    pg "DELETE FROM rental_invoice_link WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM account_move_line WHERE move_id IN
          (SELECT move_id FROM rental_invoice_link WHERE contract_id IN
             (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%'))" >/dev/null 2>&1
    pg "DELETE FROM rental_contract_line WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_event WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM rental_unit WHERE code LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

TODAY=$(date +%F)

# -------------------------------------------------------------------------
sec "1. fixtures — one recurring contract, one on-demand"
# -------------------------------------------------------------------------
PA=$(call res.partner create "[{\"name\":\"${PFX} Tenant\"}]" | rid)
U1=$(call rental.unit create "[{\"code\":\"${PFX}-U1\",\"name\":\"${PFX} One\"}]" | rid)
U2=$(call rental.unit create "[{\"code\":\"${PFX}-U2\",\"name\":\"${PFX} Two\"}]" | rid)
t_nonempty "$PA" "a tenant"
t_nonempty "$U2" "two units"

# A: an ordinary monthly contract, due today.
CA=$(call rental.contract create \
     "[{\"name\":\"${PFX}-A\",\"partner_id\":$PA,\"state\":\"active\",
        \"date_start\":\"$TODAY\",\"billing_period\":\"monthly\"}]" | rid)
pg "INSERT INTO rental_contract_line
      (contract_id, unit_id, partner_id, date_start, unit_price, billing_mode,
       state, next_period_start, company_id)
    VALUES ($CA, $U1, $PA, '$TODAY', 250000000, 'recurring', 'active', '$TODAY', 1)" >/dev/null
t_nonempty "$CA" "contract A, monthly, due today"

# B: an ON DEMAND contract with a oneoff line — the combination the cron will
# never touch, and the whole reason this action exists.
CB=$(call rental.contract create \
     "[{\"name\":\"${PFX}-B\",\"partner_id\":$PA,\"state\":\"active\",
        \"date_start\":\"$TODAY\",\"billing_period\":\"ondemand\"}]" | rid)
pg "INSERT INTO rental_contract_line
      (contract_id, unit_id, partner_id, date_start, date_end, unit_price,
       billing_mode, state, company_id)
    VALUES ($CB, $U2, $PA, '$TODAY', '$TODAY'::date + 3, 90000000,
            'oneoff', 'active', 1)" >/dev/null
t_nonempty "$CB" "contract B, on demand, with a one-off line"

# -------------------------------------------------------------------------
sec "2. the scheduled run leaves the on-demand contract alone"
# -------------------------------------------------------------------------
# Proving the baseline first: if the cron already billed B, the action below
# would look like it worked when it had nothing to do.
#
# POST, not http_get. The mutation routes are POST deliberately — a GET that
# changes data can be triggered by a link, a prefetch or a crawler — and a GET
# here silently does nothing, which made this whole section pass by accident.
curl -s -X POST -H "Cookie: session_id=${SID}" \
     "$BASE/rental/billing/run?date=$TODAY" > /dev/null 2>&1
t_eq "0" "$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CB:-0}")" \
     "the scheduled run did NOT invoice the on-demand contract"
t_ge "$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CA:-0}")" "1" \
     "…but it did invoice the ordinary monthly one"

# -------------------------------------------------------------------------
sec "3. the action invoices the on-demand contract"
# -------------------------------------------------------------------------
OUT=$(call rental.contract action_create_invoice "[[$CB]]")
t_contains "$OUT" '"invoices":1' "one invoice was created"
t_contains "$OUT" "Invoice created" "and it says so"
t_eq "1" "$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CB:-0}")" \
     "the invoice is linked to that contract"

MV=$(pg "SELECT move_id FROM rental_invoice_link WHERE contract_id=${CB:-0} LIMIT 1")
t_eq "$PA" "$(pg "SELECT partner_id FROM account_move WHERE id=${MV:-0}")" \
     "raised against the contract's customer"
t_eq "out_invoice" "$(pg "SELECT move_type FROM account_move WHERE id=${MV:-0}")" \
     "as a customer invoice"

# -------------------------------------------------------------------------
sec "4. pressing it twice does not bill twice"
# -------------------------------------------------------------------------
# UNIQUE (contract_line_id, period_start) is what makes this safe. The reply
# must SAY it was skipped rather than claim an invoice it did not create.
OUT=$(call rental.contract action_create_invoice "[[$CB]]")
t_contains "$OUT" '"invoices":0' "the second press creates nothing"
t_contains "$OUT" "Already invoiced" "and reports why"
t_eq "1" "$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CB:-0}")" \
     "still exactly one invoice for the contract"

# -------------------------------------------------------------------------
sec "5. it is scoped to the contract it was called on"
# -------------------------------------------------------------------------
CC=$(call rental.contract create \
     "[{\"name\":\"${PFX}-C\",\"partner_id\":$PA,\"state\":\"active\",
        \"date_start\":\"$TODAY\",\"billing_period\":\"ondemand\"}]" | rid)
U3=$(call rental.unit create "[{\"code\":\"${PFX}-U3\",\"name\":\"${PFX} Three\"}]" | rid)
pg "INSERT INTO rental_contract_line
      (contract_id, unit_id, partner_id, date_start, date_end, unit_price,
       billing_mode, state, company_id)
    VALUES ($CC, $U3, $PA, '$TODAY', '$TODAY'::date + 3, 70000000,
            'oneoff', 'active', 1)" >/dev/null
BEFORE=$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CC:-0}")
call rental.contract action_create_invoice "[[$CB]]" > /dev/null
t_eq "$BEFORE" "$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CC:-0}")" \
     "invoicing B left contract C untouched"

# -------------------------------------------------------------------------
sec "6. nothing due says so, rather than pretending"
# -------------------------------------------------------------------------
# A period beyond its lead days is NOT invoiced early, however it was asked
# for. The operator gets a sentence, not a silent zero.
CD=$(call rental.contract create \
     "[{\"name\":\"${PFX}-D\",\"partner_id\":$PA,\"state\":\"active\",
        \"date_start\":\"$TODAY\",\"billing_period\":\"ondemand\"}]" | rid)
U4=$(call rental.unit create "[{\"code\":\"${PFX}-U4\",\"name\":\"${PFX} Four\"}]" | rid)
pg "INSERT INTO rental_contract_line
      (contract_id, unit_id, partner_id, date_start, unit_price, billing_mode,
       state, billing_lead_days, company_id)
    VALUES ($CD, $U4, $PA, '$TODAY'::date + 400, 50000000, 'oneoff',
            'active', 7, 1)" >/dev/null
OUT=$(call rental.contract action_create_invoice "[[$CD]]")
t_contains "$OUT" '"invoices":0' "a period 400 days out is not invoiced now"
t_contains "$OUT" "Nothing is due" "and the reply explains that"

verdict
