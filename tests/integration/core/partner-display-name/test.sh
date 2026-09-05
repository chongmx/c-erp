#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# res_partner.display_name — "Carol, Big Carrots" (docs/130 §4, migration 15).
#
# Reported: "an Individual, if connected to a company, should be displayed as
# Carol, Big Carrots. a Company will stay as it is."
#
# "Carol" does not identify anyone in a customer list. There are three of them
# and the one that matters is the one at Big Carrots, so a person is labelled
# with their company and an organisation is labelled with itself.
#
# The rule is STORED, on the row, maintained by trigger. Two reasons, both
# learned the hard way in this codebase:
#
#   * Formatted at the client, it holds only where someone remembered to format
#     it. There are forty-odd pickers and several choose their model at runtime
#     (<M2OSelect model="f.relation"/>), so "update every call site" is not a
#     thing that can be finished.
#   * Computed at read, a picker cannot ORDER or `ilike` on it — and typing
#     "Big Carrots" to find the people who work there is the entire point.
#
# What this file pins down: the rule itself, the cascades that keep it true
# after a rename or a re-parent, that the value reaches a client through the
# ordinary read/search_read path, and that a client cannot write it.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZDN'
cleanup() {
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# The stored display_name, read from the database rather than from a reply.
# pgv keeps spaces — pg() strips them, which would turn "Carol, Big Carrots"
# into "Carol,BigCarrots" and make every assertion here a lie.
dn() { pgv "SELECT COALESCE(display_name,'<null>') FROM res_partner WHERE id=$1" | sed 's/^ *//;s/ *$//'; }

# -------------------------------------------------------------------------
sec "1. the column exists and is trigger-maintained"
# -------------------------------------------------------------------------
if column_exists res_partner display_name; then ok "res_partner.display_name exists"
else no "res_partner.display_name is missing — migration 15 did not run"; fi

T=$(pg "SELECT count(*) FROM pg_trigger WHERE tgname='res_partner_display_name_trg'")
t_eq "$T" "1" "the BEFORE trigger is installed"
T=$(pg "SELECT count(*) FROM pg_trigger WHERE tgname='res_partner_cascade_display_name_trg'")
t_eq "$T" "1" "the cascade trigger is installed"

# -------------------------------------------------------------------------
sec "2. a company is displayed as itself"
# -------------------------------------------------------------------------
CO=$(call res.partner create "[{\"name\":\"${PFX} Big Carrots\",\"is_company\":true}]" | rid)
t_nonempty "$CO" "the company was created"
t_eq "$(dn "$CO")" "${PFX} Big Carrots" "a company keeps its bare name"

# -------------------------------------------------------------------------
sec "3. a person connected to a company carries it"
# -------------------------------------------------------------------------
CAROL=$(call res.partner create \
    "[{\"name\":\"${PFX} Carol\",\"is_individual\":true,\"parent_id\":$CO}]" | rid)
t_nonempty "$CAROL" "the contact was created"
t_eq "$(dn "$CAROL")" "${PFX} Carol, ${PFX} Big Carrots" "reads \"Carol, Big Carrots\""

# -------------------------------------------------------------------------
sec "4. a person with no company is displayed as themselves"
# -------------------------------------------------------------------------
LONE=$(call res.partner create "[{\"name\":\"${PFX} Walk In\",\"is_individual\":true}]" | rid)
t_eq "$(dn "$LONE")" "${PFX} Walk In" "no company, no suffix"

# -------------------------------------------------------------------------
sec "5. free-text company_name counts as connected"
# -------------------------------------------------------------------------
# An imported contact that never had a company RECORD reads the same way, so
# the rule does not fork on how the data arrived. Same source migration 14 uses
# for the contact list's Company column.
IMP=$(call res.partner create \
    "[{\"name\":\"${PFX} Dave\",\"is_individual\":true,\"company_name\":\"${PFX} Imported Co\"}]" | rid)
t_eq "$(dn "$IMP")" "${PFX} Dave, ${PFX} Imported Co" "free text is used when there is no parent"

# -------------------------------------------------------------------------
sec "6. the suffix is dropped when it would say the same thing twice"
# -------------------------------------------------------------------------
SAME=$(call res.partner create \
    "[{\"name\":\"${PFX} Big Carrots\",\"parent_id\":$CO}]" | rid)
t_eq "$(dn "$SAME")" "${PFX} Big Carrots" "not \"Big Carrots, Big Carrots\""

# -------------------------------------------------------------------------
sec "7. renaming the company reaches everyone beneath it"
# -------------------------------------------------------------------------
OUT=$(call res.partner write "[[$CO],{\"name\":\"${PFX} Giant Carrots\"}]")
if has_error "$OUT"; then no "renaming the company failed"; else ok "the company was renamed"; fi
t_eq "$(dn "$CO")"    "${PFX} Giant Carrots"               "the company shows its new name"
t_eq "$(dn "$CAROL")" "${PFX} Carol, ${PFX} Giant Carrots" "and so does its contact"

# -------------------------------------------------------------------------
sec "8. moving a contact out of a company drops the suffix"
# -------------------------------------------------------------------------
call res.partner write "[[$CAROL],{\"parent_id\":null}]" >/dev/null
t_eq "$(dn "$CAROL")" "${PFX} Carol" "detached, she is just Carol again"
call res.partner write "[[$CAROL],{\"parent_id\":$CO}]" >/dev/null
t_eq "$(dn "$CAROL")" "${PFX} Carol, ${PFX} Giant Carrots" "re-attached, the company is back"

# -------------------------------------------------------------------------
sec "9. a contact promoted to a company loses its suffix"
# -------------------------------------------------------------------------
PROMO=$(call res.partner create "[{\"name\":\"${PFX} Spinoff\",\"parent_id\":$CO}]" | rid)
t_eq "$(dn "$PROMO")" "${PFX} Spinoff, ${PFX} Giant Carrots" "a plain contact under a company"
call res.partner write "[[$PROMO],{\"is_company\":true}]" >/dev/null
t_eq "$(dn "$PROMO")" "${PFX} Spinoff" "as a company it is displayed as itself"

# -------------------------------------------------------------------------
sec "10. the value reaches a client"
# -------------------------------------------------------------------------
# rowsToJson_ projects COLUMNS, so a label that exists only in serializeFields
# never leaves the server. This is the assertion that catches that regression.
REC=$(call res.partner read "[[$CAROL],[\"id\",\"name\",\"display_name\"]]")
t_contains "$REC" "${PFX} Carol, ${PFX} Giant Carrots" "read returns display_name"

# call_k inlines its 4th argument beside the context, so it is passed WITHOUT
# the surrounding braces — wrapping it yields {{…}} and a bare "Bad request",
# which has_error does not match and which therefore reads as a pass.
SR=$(call_k res.partner search_read "[[[\"id\",\"=\",$CAROL]]]" \
     "\"fields\":[\"id\",\"name\",\"display_name\"]")
t_contains "$SR" "${PFX} Carol, ${PFX} Giant Carrots" "search_read returns it too"

# -------------------------------------------------------------------------
sec "11. a picker can search on it"
# -------------------------------------------------------------------------
# S-49: a domain naming an unregistered column is rejected outright, so this
# also proves display_name is a REGISTERED field and not merely a column.
HITS=$(call_k res.partner search_read "[[[\"display_name\",\"ilike\",\"${PFX} Giant Carrots\"]]]" \
       "\"fields\":[\"id\",\"display_name\"],\"limit\":20")
t_contains "$HITS" '"result":' "display_name is filterable"
t_contains "$HITS" "${PFX} Carol, ${PFX} Giant Carrots" "typing the company finds its people"

CNT=$(call res.partner search_count "[[[\"display_name\",\"ilike\",\"${PFX} Giant Carrots\"]]]")
t_contains "$CNT" '"result":' "search_count accepts the same domain"

# -------------------------------------------------------------------------
sec "12. a client cannot write it"
# -------------------------------------------------------------------------
# Accepting display_name from a client would let a contact present itself under
# a company it does not belong to — precisely the confusion the stored value
# exists to remove.
call res.partner write "[[$CAROL],{\"display_name\":\"${PFX} Carol, Somewhere Else\"}]" >/dev/null
t_eq "$(dn "$CAROL")" "${PFX} Carol, ${PFX} Giant Carrots" "the database overrode the client's value"

SPOOF=$(call res.partner create \
    "[{\"name\":\"${PFX} Mallory\",\"display_name\":\"${PFX} Mallory, ${PFX} Giant Carrots\"}]" | rid)
t_eq "$(dn "$SPOOF")" "${PFX} Mallory" "and on create too"

verdict
