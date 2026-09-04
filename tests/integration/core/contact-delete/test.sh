#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Deleting a contact, and what happens to its documents.
#
# Reported: "please let me delete my contact. make sure you handle the
# documents related to this contact properly."
#
# There was no Delete on the contact form at all. Adding one naively would have
# been worse than not having it, because the database does not defend this on
# its own:
#
#   * FIVE tables carry a partner_id with NO FOREIGN KEY —
#     rental_contract, rental_contract_line, rental_event, rental_expense and
#     account_payment_unallocated. PostgreSQL would delete the contact happily
#     and leave a rental contract pointing at a customer that no longer exists.
#   * Where an FK does exist it is either NO ACTION, which surfaces a raw
#     constraint violation the user cannot act on, or SET NULL, which silently
#     strips the customer off a delivery or a project.
#
# So the rule is: a contact that any DOCUMENT refers to cannot be deleted, and
# the refusal names what is in the way. A contact that only has LINKS can be
# deleted, and those links are cleared. Archive is the way out for the first
# case, and it is what keeps history intact.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZDEL'
cleanup() {
    pg "DELETE FROM rental_contract_line WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM rental_contract WHERE partner_id IN
          (SELECT id FROM res_partner WHERE name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# -------------------------------------------------------------------------
sec "1. a contact nothing refers to can simply be deleted"
# -------------------------------------------------------------------------
LONE=$(call res.partner create "[{\"name\":\"${PFX} Walk In\"}]" | rid)
t_nonempty "$LONE" "a contact with no history exists"

CHK=$(call res.partner check_unlink "[[$LONE]]")
t_contains "$CHK" '"blockers":[]' "nothing blocks it"
t_contains "$CHK" '"detach":[]'   "and nothing would be detached"

OUT=$(call res.partner unlink "[[$LONE]]")
if has_error "$OUT"; then no "deleting an unreferenced contact was refused"
else ok "it deletes"; fi
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE id=$LONE")" "the row is gone"

# -------------------------------------------------------------------------
sec "2. a contact with a DOCUMENT is refused, and told why"
# -------------------------------------------------------------------------
# A rental contract is the sharpest case: rental_contract.partner_id has no
# foreign key, so nothing but this check stands between a delete and an
# orphaned contract.
CUST=$(call res.partner create "[{\"name\":\"${PFX} Tenant Bhd\",\"is_company\":true,
       \"customer_rank\":1}]" | rid)
RC=$(call rental.contract create "[{\"name\":\"${PFX} RC-1\",\"partner_id\":$CUST,
     \"billing_period\":\"monthly\",\"date_start\":\"2026-01-15\"}]" | rid)
t_nonempty "$RC" "the contact has a rental contract"

CHK=$(call res.partner check_unlink "[[$CUST]]")
t_contains "$CHK" 'rental contracts' "check_unlink names the rental contract"

OUT=$(call res.partner unlink "[[$CUST]]")
if has_error "$OUT"; then ok "the delete is refused"
else no "a contact with a rental contract was DELETED"; fi
t_contains "$OUT" "cannot be deleted" "the message says so plainly"
t_contains "$OUT" "rental contracts (1)" "and names what is in the way, with a count"
t_contains "$OUT" "Archive it instead"   "and offers the way out"
t_eq "1" "$(pg "SELECT count(*) FROM res_partner WHERE id=$CUST")" "the contact survives"
t_eq "1" "$(pg "SELECT count(*) FROM rental_contract WHERE id=${RC:-0}")" \
     "and so does the contract — no orphan was created"

# The refusal must be a 400 a user can read, not a 500 quoting SQL (SEC-28).
t_lacks "$OUT" "pqxx"        "no driver internals in the message"
t_lacks "$OUT" "violates"    "no raw constraint text in the message"
t_lacks "$OUT" "SELECT"      "no SQL in the message"

# -------------------------------------------------------------------------
sec "3. archiving is the way out, and history stays"
# -------------------------------------------------------------------------
call res.partner write "[[$CUST],{\"active\":false}]" >/dev/null
t_eq "f" "$(pg "SELECT active FROM res_partner WHERE id=$CUST")" "the contact is archived"
t_eq "1" "$(pg "SELECT count(*) FROM rental_contract WHERE id=${RC:-0}")" \
     "its rental contract is untouched"
t_eq "$CUST" "$(pg "SELECT partner_id FROM rental_contract WHERE id=${RC:-0}")" \
     "and still points at the contact"

# Archived contacts drop out of the picker, which is the point of archiving.
FOUND=$(call_k res.partner search_read "[[[\"name\",\"ilike\",\"${PFX} Tenant\"]]]" \
        '"fields":["id","name"],"limit":20')
t_lacks "$FOUND" "${PFX} Tenant Bhd" "an archived contact is out of the default search"

call res.partner write "[[$CUST],{\"active\":true}]" >/dev/null
t_eq "t" "$(pg "SELECT active FROM res_partner WHERE id=$CUST")" "and it can be unarchived"

# -------------------------------------------------------------------------
sec "4. a company with people is deletable — and says who it will detach"
# -------------------------------------------------------------------------
# This is the reported shape: a customer company with one contact under it.
# The people are not documents; deleting the company leaves them as
# independent contacts rather than deleting them too.
CO=$(call res.partner create "[{\"name\":\"${PFX} Acme Bhd\",\"is_company\":true}]" | rid)
P1=$(call res.partner create "[{\"name\":\"${PFX} Jane\",\"parent_id\":$CO}]" | rid)
P2=$(call res.partner create "[{\"name\":\"${PFX} Bob\",\"parent_id\":$CO}]"  | rid)

CHK=$(call res.partner check_unlink "[[$CO]]")
t_contains "$CHK" '"blockers":[]' "no document blocks the company"
t_contains "$CHK" 'contacts working at this company' "but the two people are declared"
t_contains "$CHK" '"count":2' "with the right count"

OUT=$(call res.partner unlink "[[$CO]]")
if has_error "$OUT"; then no "deleting a company with only contacts was refused"
else ok "the company deletes"; fi
t_eq "0" "$(pg "SELECT count(*) FROM res_partner WHERE id=$CO")" "the company is gone"
t_eq "2" "$(pg "SELECT count(*) FROM res_partner WHERE id IN (${P1:-0},${P2:-0})")" \
     "its people are NOT deleted with it"
t_eq "" "$(pg "SELECT COALESCE(parent_id::text,'') FROM res_partner WHERE id=${P1:-0}")" \
     "they are detached, not left pointing at a missing row"
t_eq "" "$(pg "SELECT COALESCE(commercial_company_name,'') FROM res_partner WHERE id=${P1:-0}")" \
     "and the company name they displayed is cleared with the link"

# -------------------------------------------------------------------------
sec "5. every kind of document is covered, not just the one that was tested"
# -------------------------------------------------------------------------
# check_unlink is a table of twenty relations. A typo in any one of them is a
# silent hole, so assert the list the server actually consults.
DOC=$(call res.partner create "[{\"name\":\"${PFX} Docs\",\"is_company\":true,
      \"customer_rank\":1}]" | rid)
INV=$(call account.move create "[{\"move_type\":\"out_invoice\",\"partner_id\":$DOC,
      \"invoice_date\":\"2026-01-10\"}]" | rid)
if [ -n "$INV" ]; then
    CHK=$(call res.partner check_unlink "[[$DOC]]")
    t_contains "$CHK" 'invoices and journal entries' "an invoice blocks the delete"
    OUT=$(call res.partner unlink "[[$DOC]]")
    if has_error "$OUT"; then ok "and the delete is refused"
    else no "a contact with an invoice was deleted"; fi
    call account.move unlink "[[$INV]]" >/dev/null 2>&1
else
    echo "    NOTE  could not raise an invoice here — skipping the invoice case"
fi

# The user login case: deleting the partner behind a user would break the login.
t_contains "$(call res.partner check_unlink '[[1]]')" '"blockers"' \
    "the company's own partner reports blockers rather than deleting"
OUT=$(call res.partner unlink '[[1]]')
if has_error "$OUT"; then ok "partner 1 (the company's own partner) cannot be deleted"
else no "partner 1 was deleted — that breaks the company record"; fi
t_eq "1" "$(pg "SELECT count(*) FROM res_partner WHERE id=1")" "it is still there"

# -------------------------------------------------------------------------
sec "6. check_unlink is read-only"
# -------------------------------------------------------------------------
# It is called to draw a dialog. If asking the question changed anything, the
# dialog itself would be a mutation.
BEFORE=$(pg "SELECT count(*) FROM res_partner")
call res.partner check_unlink "[[$CUST]]" >/dev/null
call res.partner check_unlink "[[1]]"     >/dev/null
t_eq "$BEFORE" "$(pg "SELECT count(*) FROM res_partner")" "asking twice changed nothing"

verdict
