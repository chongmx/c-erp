#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Contacts: a company, and the people who work there.
#
# THE FEATURE. A customer is usually an organisation with individuals inside it.
# You create "Acme Sdn Bhd" once, then attach Jane and Ali to it, and from then
# on the company is a real thing you can navigate: open Acme and see its people,
# raise an invoice on Acme, rename Acme once and have every contact follow.
#
# WHAT THIS TEST FOUND (2026-09-01). None of that is possible yet, and the way
# it fails is the reason it feels broken rather than missing:
#
#   res_partner has NO parent_id. There is no partner->partner relation at all.
#   `create` with parent_id RETURNS A NEW ID AND REPORTS SUCCESS — the unknown
#   field is silently dropped. The caller is told it worked. Nothing is linked.
#
# That silent success is the defect worth fixing first. A hard error would have
# been discovered in a minute; a lie takes an afternoon.
#
#   company_name  a free-text VARCHAR, so "who works at Acme" is a string match,
#                 two spellings are two companies, and renaming updates nothing.
#   company_id    a Many2one to res.company — the MULTI-COMPANY OWNER of the row,
#                 not the customer's employer. Anyone reaching for "company" on a
#                 contact finds this first and links the wrong thing.
#
# HOW TO READ A FAILURE. This is an acceptance test for the feature as it should
# behave, so it is expected to fail until the relation exists. Each check names
# the capability it is asserting; the set that fails is the specification of the
# work remaining. Section 1 is the schema gate and explains the rest.
#
# THE FIX THIS TEST IS WRITTEN AGAINST:
#   parent_id INTEGER REFERENCES res_partner(id) ON DELETE SET NULL
#   + registered as Many2one("res.partner"), exposed on the form,
#   + create/write must REJECT unknown fields instead of dropping them.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZCC'                    # every row this test makes carries it
# Clean up on EXIT, but do NOT print the verdict from the trap: auth_or_die
# prints its own "*** FAILURES ***" and exits, and a trap-fired verdict then
# printed "All checks passed." after it — two verdict lines, the wrong one last.
# The runner scores on that line, so a dead test would have been scored green.
cleanup() { pg "DELETE FROM res_partner WHERE name LIKE '${PFX} %'" >/dev/null 2>&1; }
trap cleanup EXIT

auth_or_die

# -------------------------------------------------------------------------
sec "1. the schema can express 'this person works at that company'"
# -------------------------------------------------------------------------
# Everything below depends on this. If parent_id is absent, the feature cannot
# work however good the UI is, so say that once here rather than eleven times.
HAVE_PARENT=0
if column_exists res_partner parent_id; then
    ok "res_partner.parent_id exists"
    HAVE_PARENT=1
else
    no "res_partner.parent_id is MISSING — there is no partner→partner link, so a"
    echo "          contact cannot belong to a company. Everything below fails from this."
fi

t_eq "1" "$(pg "SELECT count(*) FROM information_schema.columns
                 WHERE table_name='res_partner' AND column_name='is_company'")" \
     "res_partner.is_company exists (a partner can BE a company)"

# company_id is not the employer, and confusing the two silently mis-files data.
REL=$(call res.partner get_views '[[],["form"]]' | grep -o '"relation":"res\.company"' | head -1)
if [ -n "$REL" ]; then
    ok "company_id is a res.company link — the multi-company OWNER, not the employer"
    echo "          (so it must never be offered as 'the customer's company')"
fi

# -------------------------------------------------------------------------
sec "2. creating the company"
# -------------------------------------------------------------------------
CO=$(call res.partner create "[{\"name\":\"${PFX} Acme Sdn Bhd\",\"is_company\":true,
     \"email\":\"info@acme.test\",\"phone\":\"+60 3 1234 5678\",
     \"street\":\"12 Jalan Sultan\",\"city\":\"Kuala Lumpur\",\"zip\":\"50000\"}]" | rid)
t_nonempty "$CO" "company partner created"

if [ -n "$CO" ]; then
    t_eq "t" "$(pg "SELECT is_company FROM res_partner WHERE id=$CO")" \
         "it is stored as a company (is_company = true)"
    t_eq "${PFX} Acme Sdn Bhd" "$(pgv "SELECT name FROM res_partner WHERE id=$CO")" \
         "its name round-trips"
fi

# -------------------------------------------------------------------------
sec "3. attaching an individual to that company"
# -------------------------------------------------------------------------
# THE CORE OF THE FEATURE, and where the reported problem lives.
RESP=$(call res.partner create "[{\"name\":\"${PFX} Jane Tan\",\"is_company\":false,
       \"email\":\"jane@acme.test\",\"job_position\":\"Finance Manager\",
       \"parent_id\":${CO:-0}}]")
IND=$(echo "$RESP" | rid)

if [ "$HAVE_PARENT" = "1" ]; then
    t_nonempty "$IND" "individual created with parent_id"
    t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=$IND")" \
         "the link is STORED — Jane's parent_id points at Acme"
else
    # The failure mode that makes this feel broken instead of missing.
    if [ -n "$IND" ]; then
        no "create() accepted parent_id and returned id=$IND — but the column does not"
        echo "          exist, so NOTHING WAS LINKED. The API reported success for a write"
        echo "          it silently discarded. This is the defect to fix first: unknown"
        echo "          fields must be rejected, not dropped."
    elif has_error "$RESP"; then
        ok "create() REJECTED the unknown field parent_id instead of pretending"
        echo "          (correct behaviour once the field is genuinely unsupported)"
    else
        no "create() with parent_id neither created a row nor reported an error"
    fi
fi

# -------------------------------------------------------------------------
sec "4. the link is readable back through the API, not just in SQL"
# -------------------------------------------------------------------------
# A relation the UI cannot read is not a relation the user has.
if [ "$HAVE_PARENT" = "1" ] && [ -n "$IND" ]; then
    READ=$(call res.partner read "[[$IND],[\"name\",\"parent_id\"]]")
    t_contains "$READ" "parent_id" "read() returns parent_id"
    t_contains "$READ" "\"parent_id\":$CO" "it carries the company's id"
    # This codebase returns a bare id for every Many2one (country_id, company_id
    # behave the same); the name is fetched separately. Assert the convention the
    # app actually has, not Odoo's [id, display_name] tuple.
    NAMED=$(call res.partner search_read "[[[\"id\",\"=\",$CO]],[\"name\"]]")
    t_contains "$NAMED" "Acme" "the company's name is one search_read away, as the form does it"
else
    no "cannot read the link back — it was never stored"
fi

# -------------------------------------------------------------------------
sec "5. navigating the company to its people"
# -------------------------------------------------------------------------
# "Who works at Acme?" is the question a company record exists to answer.
call res.partner create "[{\"name\":\"${PFX} Ali bin Osman\",\"is_company\":false,
     \"parent_id\":${CO:-0},\"company_name\":\"${PFX} Acme Sdn Bhd\"}]" >/dev/null

if [ "$HAVE_PARENT" = "1" ]; then
    KIDS=$(call res.partner search_count "[[[\"parent_id\",\"=\",$CO]]]" | rid)
    t_eq "2" "${KIDS:-0}" "searching parent_id = Acme finds both contacts"
else
    no "no relational way to ask 'who works at Acme'"
    STR=$(pg "SELECT count(*) FROM res_partner WHERE company_name='${PFX} Acme Sdn Bhd'")
    echo "          the only answer available today is a STRING match on company_name,"
    echo "          which finds $STR of 2 — it misses anyone whose spelling differs."
fi

# -------------------------------------------------------------------------
sec "6. renaming the company"
# -------------------------------------------------------------------------
# The point of a relation rather than copied text: one edit, everyone follows.
if [ -n "$CO" ]; then
    call res.partner write "[[$CO],{\"name\":\"${PFX} Acme Holdings Bhd\"}]" >/dev/null
    if [ "$HAVE_PARENT" = "1" ] && [ -n "$IND" ]; then
        # The contact stores only the id, so it CANNOT hold a stale name — that
        # is the whole benefit over copied text. Assert the property that
        # actually delivers it: the link survives the rename, and resolving it
        # yields the NEW name. (read() returns bare ids for every Many2one in
        # this codebase; asking it for the name would test Odoo, not this app.)
        t_eq "$CO" "$(pg "SELECT parent_id FROM res_partner WHERE id=$IND")" \
             "the link survives the rename"
        t_eq "${PFX} Acme Holdings Bhd" \
             "$(pgv "SELECT p.name FROM res_partner c JOIN res_partner p ON p.id=c.parent_id
                      WHERE c.id=$IND")" \
             "resolving it gives the NEW name — one edit reached every contact"
        t_eq "0" "$(pg "SELECT count(*) FROM res_partner
                         WHERE id=$IND AND company_name='${PFX} Acme Sdn Bhd'")" \
             "no stale copy of the old name was left on the contact"
    else
        STALE=$(pg "SELECT count(*) FROM res_partner WHERE company_name LIKE '%Acme Sdn Bhd%'")
        [ "${STALE:-0}" -gt 0 ] \
          && no "after the rename, $STALE contact(s) still carry the OLD company_name text" \
          || ok "no stale company_name text left behind"
    fi
fi

# -------------------------------------------------------------------------
sec "7. a company cannot be its own parent, or its child's child"
# -------------------------------------------------------------------------
# A cycle here makes any recursive walk hang.
if [ "$HAVE_PARENT" = "1" ] && [ -n "$CO" ]; then
    SELF=$(call res.partner write "[[$CO],{\"parent_id\":$CO}]")
    has_error "$SELF" && ok "a partner cannot be its own parent" \
                      || no "a partner was allowed to become its own parent (cycle)"
    if [ -n "$IND" ]; then
        CYC=$(call res.partner write "[[$CO],{\"parent_id\":$IND}]")
        has_error "$CYC" && ok "a two-step cycle is refused" \
                         || no "Acme→Jane→Acme cycle was allowed"
    fi
else
    echo "    SKIP  cycle checks need parent_id"
fi

# -------------------------------------------------------------------------
sec "8. the form actually offers the field"
# -------------------------------------------------------------------------
# A column nobody can fill in from the UI is not a feature.
FORM=$(call res.partner get_views '[[],["form"]]')
if [ "$HAVE_PARENT" = "1" ]; then
    t_contains "$FORM" "parent_id" "the contact form exposes parent_id"
else
    no "the contact form has no company link to offer"
fi
t_contains "$FORM" "company_name" "the form exposes company_name"
t_contains "$FORM" "is_company"   "the form exposes is_company"

# -------------------------------------------------------------------------
sec "9. deleting a company must not orphan or destroy its people"
# -------------------------------------------------------------------------
if [ "$HAVE_PARENT" = "1" ] && [ -n "$CO" ] && [ -n "$IND" ]; then
    call res.partner unlink "[[$CO]]" >/dev/null
    ALIVE=$(pg "SELECT count(*) FROM res_partner WHERE id=$IND")
    t_eq "1" "${ALIVE:-0}" "deleting the company leaves its contacts alive"
    t_eq "" "$(pg "SELECT COALESCE(parent_id::text,'') FROM res_partner WHERE id=$IND")" \
         "their parent_id is cleared, not left dangling (ON DELETE SET NULL)"
else
    echo "    SKIP  delete behaviour needs parent_id"
fi

# -------------------------------------------------------------------------
sec "10. unknown fields must never be silently accepted"
# -------------------------------------------------------------------------
# The general form of the bug in section 3, stated as a rule. This one check
# would have surfaced the whole problem the first time anyone tried it.
GHOST=$(call res.partner create "[{\"name\":\"${PFX} Ghost\",\"totally_made_up_field\":42}]")
GID=$(echo "$GHOST" | rid)
if [ -n "$GID" ]; then
    no "create() accepted an invented field and returned id=$GID — writes that cannot"
    echo "          be honoured must fail loudly, or every typo becomes silent data loss"
else
    has_error "$GHOST" && ok "create() rejects fields the model does not have" \
                       || no "create() with an invented field neither created nor errored"
fi

# One verdict line, at the end, always reached unless the script dies early —
# in which case the runner treats the MISSING verdict as a failure, which is
# exactly right.
verdict
