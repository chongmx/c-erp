#!/bin/bash
# --- harness ---------------------------------------------------------------
# Walk up for CMakeLists.txt rather than counting `../`, so this test behaves
# the same whether the runner invoked it or you ran it directly, and so it can
# be nested a folder deeper without a preamble edit.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# Document templates: shipped baseline, auto-upgrade, reset (docs/090).
#
# ir_report_template.template_html is owned by the database — the Document
# Templates editor writes to it. default_html is owned by the source tree and is
# refreshed on every start. The pair is what removes the need for hand-written
# SQL migrations whenever a shipped template improves:
#
#   * an untouched template follows the source tree automatically;
#   * a customised one keeps its edits, is flagged, and can be reset.
#
# The load-bearing assertions are the two directions of that rule — an edited
# template must NOT be silently overwritten, and an unedited one MUST follow.
# Those two run against a throwaway row, so a failure here can never leave the
# real Invoice or Sales Order template in a broken state.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }

echo "############ every shipped template has a baseline ############"
MISSING=$(pg "SELECT count(*) FROM ir_report_template WHERE COALESCE(default_html,'') = ''")
[ "$MISSING" = "0" ] && ok "all templates carry a shipped baseline (default_html)" \
                     || no "$MISSING template(s) have no baseline"
CNT=$(pg "SELECT count(*) FROM ir_report_template")
[ "${CNT:-0}" -ge 4 ] && ok "$CNT templates seeded" || no "expected >= 4 templates, got $CNT"

echo "############ is_customized is reported over RPC ############"
TID=$(pg "SELECT id FROM ir_report_template WHERE model='sale.order' ORDER BY id LIMIT 1")
[ -z "$TID" ] && { no "no sale.order template"; echo "*** FAILURES ***"; exit 1; }
R=$(call ir.report.template read "[[$TID]]" "{$CTX}")
echo "$R" | grep -q '"default_html"' && ok "read() exposes default_html" || no "default_html not in read()"
echo "$R" | grep -q '"is_customized"' && ok "read() exposes is_customized" || no "is_customized not in read()"
S=$(call ir.report.template search_read "[[[\"model\",\"=\",\"sale.order\"]]]" "{$CTX}")
echo "$S" | grep -q '"is_customized"' && ok "search_read() exposes is_customized" || no "is_customized not in search_read()"

echo "############ an edit marks the template customized ############"
BASELINE=$(pg "SELECT md5(default_html) FROM ir_report_template WHERE id=$TID")
call ir.report.template write "[[$TID],{\"template_html\":\"<html>QA EDIT</html>\"}]" "{$CTX}" >/dev/null
[ "$(pg "SELECT (template_html IS DISTINCT FROM default_html) FROM ir_report_template WHERE id=$TID")" = "t" ] \
    && ok "edited template flags as customized" || no "edit did not flag as customized"
# The baseline must survive the edit — it is what Reset restores.
[ "$(pg "SELECT md5(default_html) FROM ir_report_template WHERE id=$TID")" = "$BASELINE" ] \
    && ok "the shipped baseline is untouched by an edit" || no "editing clobbered default_html"

echo "############ reset restores the shipped template ############"
# Give the editor a saved block layout, which regenerates template_html on its
# next save; reset must drop it or the customisation comes straight back.
call ir.config.parameter create "[{\"key\":\"layout.blocks.sale.order\",\"value\":\"[]\"}]" "{$CTX}" >/dev/null
call ir.report.template action_reset_default "[[$TID]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT (template_html IS DISTINCT FROM default_html) FROM ir_report_template WHERE id=$TID")" = "f" ] \
    && ok "reset restores template_html from the shipped baseline" || no "reset did not restore the template"
[ "$(pg "SELECT count(*) FROM ir_config_parameter WHERE key='layout.blocks.sale.order'")" = "0" ] \
    && ok "reset drops the saved block layout" || no "block layout survived the reset"
[ "$(pg "SELECT template_html LIKE '%{{product_name}}%' FROM ir_report_template WHERE id=$TID")" = "t" ] \
    && ok "the restored template is the real shipped one" || no "restored template does not look like the shipped one"
# The sale-order template must still carry the display_type class, or sections
# and notes stop rendering on the PDF (docs/079).
[ "$(pg "SELECT template_html LIKE '%row-{{line_type}}%' FROM ir_report_template WHERE id=$TID")" = "t" ] \
    && ok "the shipped sale.order template renders sections/notes" || no "row-{{line_type}} missing from the shipped template"

echo "############ the upgrade rule, on a throwaway template ############"
# Both directions are exercised on a scratch row so the real templates are
# never at risk. The two UPDATEs below are exactly what seedTemplates_() runs.
pg "DELETE FROM ir_report_template WHERE model='qa.reset.probe'" >/dev/null
QID=$(pg "INSERT INTO ir_report_template (name,model,template_html,default_html) VALUES ('QA Probe','qa.reset.probe','<html>V1</html>','<html>V1</html>') RETURNING id" | head -1)
[ -n "$QID" ] && ok "scratch template created ($QID)" || { no "could not create scratch template"; echo "*** FAILURES ***"; exit 1; }

upgrade(){   # simulate a start where the source tree now ships $1
    pg "UPDATE ir_report_template SET default_html=\$\$$1\$\$ WHERE id=$QID AND default_html=''" >/dev/null
    pg "UPDATE ir_report_template SET template_html=\$\$$1\$\$, default_html=\$\$$1\$\$ WHERE id=$QID AND template_html=default_html AND default_html<>\$\$$1\$\$" >/dev/null
    pg "UPDATE ir_report_template SET default_html=\$\$$1\$\$ WHERE id=$QID AND default_html<>\$\$$1\$\$" >/dev/null
}

# (a) untouched → follows the source tree
upgrade '<html>V2</html>'
[ "$(pg "SELECT template_html='<html>V2</html>' FROM ir_report_template WHERE id=$QID")" = "t" ] \
    && ok "an unedited template picks up the newer shipped version" || no "unedited template did not follow"

# (b) customised → keeps its edits, and is flagged
call ir.report.template write "[[$QID],{\"template_html\":\"<html>MINE</html>\"}]" "{$CTX}" >/dev/null
upgrade '<html>V3</html>'
[ "$(pg "SELECT template_html='<html>MINE</html>' FROM ir_report_template WHERE id=$QID")" = "t" ] \
    && ok "a customised template survives a newer shipped version" || no "customisation was overwritten"
[ "$(pg "SELECT default_html='<html>V3</html>' FROM ir_report_template WHERE id=$QID")" = "t" ] \
    && ok "its baseline still tracks the newest shipped version" || no "baseline went stale"

# (c) reset then lands on the NEWEST shipped version, not the one it forked from
call ir.report.template action_reset_default "[[$QID]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT template_html='<html>V3</html>' FROM ir_report_template WHERE id=$QID")" = "t" ] \
    && ok "reset lands on the newest shipped version" || no "reset restored a stale version"

# (d) a row with no baseline recorded refuses to reset rather than blanking
pg "UPDATE ir_report_template SET default_html='' WHERE id=$QID" >/dev/null
E=$(call ir.report.template action_reset_default "[[$QID]]" "{$CTX}")
echo "$E" | grep -qi 'no shipped default' && ok "reset without a baseline is refused" \
                                          || no "reset without a baseline was not refused: $(echo "$E" | head -c 120)"

pg "DELETE FROM ir_report_template WHERE id=$QID" >/dev/null
ok "scratch template removed"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
