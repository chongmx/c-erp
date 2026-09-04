#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# Every screen in the product opens and renders something (docs/129).
#
# This suite exists because of a specific miss. A one-line change to the
# `altViews` getter — which every generic list depends on — read a field MAP as
# a list. `{}.some` is undefined, the getter threw, OWL swallowed it, and every
# list view in the ERP went blank.
#
# The whole suite still passed. Every other test here drives the HTTP API, and
# the API was perfectly healthy; the damage was in the browser, on screens no
# test opened. A person found it by clicking around.
#
# So: walk the menu the way a person does and assert each screen is not empty.
# Shallow on purpose — it checks that something rendered, not what — because
# the failure it guards against is a white rectangle, and a deep assertion per
# screen would duplicate the rest of the suite and rot.
# =============================================================
auth_or_die

sec "1. every app, every menu entry"

if ! command -v node >/dev/null 2>&1; then
    echo "    SKIP  page sweep: node unavailable"
    verdict; exit 0
fi

REP=$(BASE="$BASE" DBN="${DBN:-odoo}" \
      node tests/integration/core/page-sweep/sweep.mjs 2>&1 | tail -1)

g(){ printf '%s' "$REP" | python3 -c \
      "import sys,json;d=json.loads(sys.stdin.read() or '{}');print(d.get('$1',''))" 2>/dev/null; }

SKIP=$(g skipped)
if [ -n "$SKIP" ]; then
    echo "    SKIP  page sweep: $SKIP"
    verdict; exit 0
fi

APPS=$(g apps)
SEEN=$(g visited)
BLANK=$(g blank)
ERRED=$(g errored)

t_ge "$APPS" "8"   "the app grid offers every module"
t_ge "$SEEN" "40"  "the sweep actually walked the menu ($SEEN screens)"

# The two that matter. A blank screen and a screen that logged an error are
# different failures: the first is a render that produced nothing, the second
# is a render that threw on the way.
t_eq "[]" "$BLANK" "no screen rendered blank"
t_eq "[]" "$ERRED" "no screen logged a console or page error"

[ "$BLANK" != "[]" ] && echo "    blank:   $BLANK"
[ "$ERRED" != "[]" ] && echo "    errored: $ERRED"

verdict
