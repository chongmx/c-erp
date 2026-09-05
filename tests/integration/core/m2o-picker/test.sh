#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The many-to-one picker's contract with the server (M2OSelect.js).
#
# Reported: "now I added a company, I want to start a rental contract for that
# company, I cannot see the company being listed at the combobox menu."
#
# Every m2o combobox in this app used to be a <select> filled once on form open
# with
#
#     search_read([[]], fields:['id','name'], limit: 200)      -- no order
#
# which has THREE separate defects, and a user meets all three as the same
# sentence: "my record is not in the list".
#
#   1. TRUNCATION — no ORDER BY means the server's default id ASC, so the
#      dropdown holds the OLDEST 200 rows. The company you just created is the
#      newest, so it is precisely the one that is missing.
#   2. STALENESS — the fetch happens once, so a company created in another tab
#      never appears until the screen is reloaded.
#   3. SILENT VALUE LOSS — a <select> whose current value is not among its
#      options falls back to the first option. Open such a record, press Save,
#      and the link is quietly cleared. No error. This is the dangerous one.
#
# The fix never holds the whole table: the widget SEARCHES, and resolves the
# current value BY ID. This file asserts the four server calls it relies on,
# and reproduces the old failure first so the test is known to be able to fail.
#
# It is an API test on purpose — it pins the RPC contract. That the widget
# actually renders is checked by tests/functional/core/form-pickers, which drives
# a real browser, because an OWL template error is invisible from here.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}

PFX='ZZM2O'
cleanup() {
    pg "UPDATE res_partner SET parent_id=NULL WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup
auth_or_die

# -------------------------------------------------------------------------
sec "1. build a table big enough for the bug to exist"
# -------------------------------------------------------------------------
# 260 companies, then the one the user just created. Under 200 rows the old
# code looked fine, which is why this only ever bit customers with real data.
pg "INSERT INTO res_partner (name, is_company, company_id)
    SELECT '${PFX} Bulk ' || lpad(g::text, 4, '0'), TRUE, 1
      FROM generate_series(1, 260) g" >/dev/null
NEWCO=$(call res.partner create "[{\"name\":\"${PFX} Zebra Newest Bhd\",
        \"is_company\":true,\"customer_rank\":1}]" | rid)
t_nonempty "$NEWCO" "the company the user just created exists"

TOTAL=$(pg "SELECT count(*) FROM res_partner WHERE is_company")
t_ge "${TOTAL:-0}" "260" "there are more companies than the old dropdown fetched"

# -------------------------------------------------------------------------
sec "2. reproduce the ORIGINAL bug — the old call cannot see it"
# -------------------------------------------------------------------------
# A regression test that has never seen the failure it prevents is asserting
# that today's code does what today's code does.
OLD=$(call_k res.partner search_read '[[]]' '"fields":["id","name"],"limit":200')
t_lacks "$OLD" "${PFX} Zebra Newest Bhd" \
    "the OLD call (limit 200, no order) does NOT contain the new company"

# -------------------------------------------------------------------------
sec "3. what the widget sends instead"
# -------------------------------------------------------------------------
# Typing 'zebra' searches the server. The record is found because it MATCHES,
# not because it happened to fall inside a window.
FOUND=$(call_k res.partner search_read \
        "[[[\"is_company\",\"=\",true],[\"name\",\"ilike\",\"Zebra Newest\"]]]" \
        '"fields":["id","name"],"limit":20,"offset":0,"order":"name ASC"')
t_contains "$FOUND" "${PFX} Zebra Newest Bhd" \
    "typing part of the name FINDS it — defect 1 and 2 are both gone"

# Case-insensitive, because a user types how they think, not how it was keyed.
LOWER=$(call_k res.partner search_read \
        "[[[\"name\",\"ilike\",\"zebra newest\"]]]" '"fields":["id","name"],"limit":20')
t_contains "$LOWER" "${PFX} Zebra Newest Bhd" "the search is case-insensitive"

# -------------------------------------------------------------------------
sec "4. the honest 'N more…' count"
# -------------------------------------------------------------------------
# The dropdown shows 20 rows and states how many it is not showing. The old
# control said nothing at all, which is how truncation stayed invisible.
CNT=$(call res.partner search_count "[[[\"is_company\",\"=\",true]]]" | rid)
t_ge "${CNT:-0}" "260" "search_count reports the full total, not the page size"

PAGE=$(call_k res.partner search_read "[[[\"is_company\",\"=\",true]]]" \
       '"fields":["id","name"],"limit":20,"offset":0,"order":"name ASC"')
# Count "name": and not "id": — the JSON-RPC envelope carries its own
# top-level "id":null, so counting ids reports one row more than came back.
ROWS=$(printf '%s' "$PAGE" | grep -o '"name":' | wc -l)
t_eq "20" "$ROWS" "a page is the 20 rows the dropdown shows"

# -------------------------------------------------------------------------
sec "5. paging through the browse dialog"
# -------------------------------------------------------------------------
# 'Browse all' pages with offset. Two pages must not return the same rows —
# an offset that is ignored looks like a working dialog that never advances.
P1=$(call_k res.partner search_read "[[[\"name\",\"like\",\"${PFX} Bulk\"]]]" \
     '"fields":["id","name"],"limit":50,"offset":0,"order":"name ASC"')
P2=$(call_k res.partner search_read "[[[\"name\",\"like\",\"${PFX} Bulk\"]]]" \
     '"fields":["id","name"],"limit":50,"offset":50,"order":"name ASC"')
t_contains "$P1" "${PFX} Bulk 0001" "page 1 starts at the first name in order"
t_lacks    "$P2" "${PFX} Bulk 0001" "page 2 does not repeat page 1"
t_contains "$P2" "${PFX} Bulk 0051" "page 2 continues where page 1 stopped"

# ORDER BY name is what makes paging stable. Without it the server's row order
# is undefined and a record can appear on two pages or on none.
# grep -o, not sed: sed's .* is greedy and would capture the LAST name on the
# line, which is the opposite of the first row and passes for the wrong reason.
FIRST=$(printf '%s' "$P1" | grep -o '"name":"[^"]*"' | head -1)
t_contains "$FIRST" "${PFX} Bulk 0001" "results are ordered by name, so paging is stable"

# -------------------------------------------------------------------------
sec "6. DEFECT 3 — a value outside the window still resolves"
# -------------------------------------------------------------------------
# The widget never looks its current value up in a page of search results; it
# reads it by id. That is the whole defence against a save quietly clearing a
# link the user never touched.
READBACK=$(call res.partner read "[[$NEWCO],[\"name\"]]")
t_contains "$READBACK" "${PFX} Zebra Newest Bhd" \
    "read([id]) resolves a record that no page of results contains"

# And the round trip: point a record at it, read it back, and the link holds.
JANE=$(call res.partner create "[{\"name\":\"${PFX} Jane\",\"parent_id\":$NEWCO}]" | rid)
t_eq "$NEWCO" "$(pg "SELECT parent_id FROM res_partner WHERE id=${JANE:-0}")" \
     "a record can be LINKED to a company outside the old window"

# Saving an unrelated field must not disturb the link. This is the exact shape
# of the silent-loss bug: a form re-save that clears a field nobody edited.
call res.partner write "[[$JANE],{\"phone\":\"012-3456789\"}]" >/dev/null
t_eq "$NEWCO" "$(pg "SELECT parent_id FROM res_partner WHERE id=${JANE:-0}")" \
     "and saving another field does NOT clear it"

# -------------------------------------------------------------------------
sec "7. the domain narrows the search, not just the first fetch"
# -------------------------------------------------------------------------
# A company picker must not offer people. The old control applied its domain to
# one initial fetch; the widget ANDs it into every keystroke.
PERSON=$(call res.partner create "[{\"name\":\"${PFX} Zebra Person\",\"is_company\":false}]" | rid)
t_nonempty "$PERSON" "an individual with a matching name exists"

SCOPED=$(call_k res.partner search_read \
         "[[[\"is_company\",\"=\",true],[\"name\",\"ilike\",\"Zebra\"]]]" \
         '"fields":["id","name"],"limit":20,"order":"name ASC"')
t_contains "$SCOPED" "${PFX} Zebra Newest Bhd" "the company matches"
t_lacks    "$SCOPED" "${PFX} Zebra Person"     "the individual is excluded by the domain"

# -------------------------------------------------------------------------
sec "8. an empty search is still bounded"
# -------------------------------------------------------------------------
# Focusing the box with nothing typed lists the first page. It must not stream
# the whole table back — that is the cost the old prefetch paid on every open.
OPEN=$(call_k res.partner search_read '[[]]' \
       '"fields":["id","name"],"limit":20,"offset":0,"order":"name ASC"')
OROWS=$(printf '%s' "$OPEN" | grep -o '"name":' | wc -l)
t_eq "20" "$OROWS" "opening the dropdown costs 20 rows, not the table"

verdict
