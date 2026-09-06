#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# "Create Invoice" on a rental contract, pressed by a person.
#
# Asked for: "for each rental contract created and active, I want a method of
# creating invoice, similar to how sales order let me create invoice."
#
# sale.order has a statusbar of workflow buttons. A rental contract had none —
# it could be edited and deleted and nothing else — so the only way to invoice
# one was to wait for the cron, which skips one-off and on-demand contracts by
# design. Those could not be invoiced at all.
#
# The integration test beside this one pins the METHOD: scope, idempotency, the
# period gate. This pins the thing a person meets: that the button is on the
# form, that it only appears on an active contract, and that pressing it twice
# says "already invoiced" rather than silently doing nothing or claiming a
# second invoice. A button that no-ops quietly reads as broken.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZIV'
cleanup() {
    pg "DELETE FROM rental_invoice_link WHERE contract_id IN
          (SELECT id FROM rental_contract WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
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

CHROME=${CHROME_PATH:-/usr/bin/google-chrome}

# -------------------------------------------------------------------------
sec "1. the browser tooling"
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

# -------------------------------------------------------------------------
sec "2. create the contract and invoice it, on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/contract_invoice_test BASE="$BASE" DBN="$DBN" \
      timeout 420 node tests/lib/render_contract_invoice.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "a contract can be invoiced from its own form"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the same facts, checked independently of the driver"
# -------------------------------------------------------------------------
CT=$(pg "SELECT id FROM rental_contract WHERE name='${PFX}-CT'")
t_nonempty "$CT" "the contract was created through the form"
t_eq "active" "$(pg "SELECT state FROM rental_contract WHERE id=${CT:-0}")" \
     "and it is active — the state the button is gated on"

t_eq "1" "$(pg "SELECT count(*) FROM rental_invoice_link WHERE contract_id=${CT:-0}")" \
     "exactly ONE invoice exists after two presses"

MV=$(pg "SELECT move_id FROM rental_invoice_link WHERE contract_id=${CT:-0} LIMIT 1")
t_nonempty "$MV" "it is linked to a real account.move"
t_eq "out_invoice" "$(pg "SELECT move_type FROM account_move WHERE id=${MV:-0}")" \
     "raised as a customer invoice"

P=$(pg "SELECT id FROM res_partner WHERE name='${PFX} Renter Sdn Bhd'")
t_eq "$P" "$(pg "SELECT partner_id FROM account_move WHERE id=${MV:-0}")" \
     "against the contract's customer"

# The invoice must carry the rent, not a zero line — the amount is the whole
# point of raising it.
t_ge "$(pg "SELECT COALESCE(amount_total,0)::bigint FROM account_move WHERE id=${MV:-0}")" "1" \
     "and it is for a non-zero amount"

verdict
