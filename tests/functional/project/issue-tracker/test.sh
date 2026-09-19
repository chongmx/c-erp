#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# File a bug and work it, on screen (tests/lib/render_issue_tracker.mjs).
#
# Asked for: "proper project management with issues tracker, project tasks
# managements, issues update with image upload etc... assignments, similar to
# a jira system ... I want to use it to manage the development of this c-erp."
#
# Every record here is made by clicking: the project (with its ticket prefix),
# the ticket, the status change, the assignment, the label, the comment and
# both images. This script only re-checks the DATABASE afterwards, so a driver
# that stopped early cannot pass by saying nothing. The API contract — keys,
# history, who may edit a comment — is pinned in
# tests/integration/project/issue-tracker.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZIF'

cleanup() {
    pg "DELETE FROM mail_message WHERE res_model='project.task' AND res_id IN
          (SELECT t.id FROM project_task t JOIN project_project p ON p.id=t.project_id
            WHERE p.name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM ir_attachment WHERE (res_model='project.task' AND res_id IN
          (SELECT t.id FROM project_task t JOIN project_project p ON p.id=t.project_id
            WHERE p.name LIKE '${PFX}%')) OR name LIKE 'attach-me%'" >/dev/null 2>&1
    pg "DELETE FROM project_project WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM project_tag WHERE lower(name) LIKE 'zzif%'" >/dev/null 2>&1
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
sec "2. the journey, on screen"
# -------------------------------------------------------------------------
OUT=$(SHOTDIR=/tmp/issue_tracker_test BASE="$BASE" DBN="$DBN" \
      timeout 400 node tests/lib/render_issue_tracker.mjs "$PFX" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
if [ "$RC" -eq 0 ]; then ok "a bug is filed and worked entirely from the screen"
else no "the on-screen journey failed (see the report above)"; fi

# -------------------------------------------------------------------------
sec "3. the database, checked independently of the driver"
# -------------------------------------------------------------------------
P=$(pg "SELECT id FROM project_project WHERE name='${PFX}IssueTracker' OR name='${PFX} Issue Tracker' LIMIT 1")
t_nonempty "$P" "the project was created"
t_eq "${PFX}" "$(pg "SELECT task_prefix FROM project_project WHERE id=${P:-0}")" \
     "with the prefix typed in lower case, stored in upper"
T=$(pg "SELECT id FROM project_task WHERE project_id=${P:-0} AND key='${PFX}-1'")
t_nonempty "$T" "the ticket is ${PFX}-1"
t_eq "bug|1" "$(pg "SELECT issue_type || '|' || priority FROM project_task WHERE id=${T:-0}")" \
     "a Bug, priority High"
t_eq "InProgress" "$(pg "SELECT s.name FROM project_task t JOIN project_task_type s ON s.id=t.stage_id WHERE t.id=${T:-0}")" \
     "in In Progress"
t_eq "$(pg "SELECT id FROM res_users WHERE login='admin'")" "$(pg "SELECT user_id FROM project_task WHERE id=${T:-0}")" \
     "assigned to the person who clicked Assign to me"
t_eq "zzif-ui" "$(pg "SELECT g.name FROM project_task_tag_rel x JOIN project_tag g ON g.id=x.tag_id WHERE x.task_id=${T:-0}")" \
     "labelled"
t_eq "1" "$(pg "SELECT count(*) FROM project_task WHERE id=${T:-0} AND description ~ '/web/content/[0-9]+'")" \
     "the description references the pasted screenshot"
t_eq "2" "$(pg "SELECT count(*) FROM ir_attachment WHERE res_model='project.task' AND res_id=${T:-0}")" \
     "both images are attachments OF THE TICKET (the pasted one was linked on create)"
t_eq "1" "$(pg "SELECT count(*) FROM mail_message WHERE res_model='project.task' AND res_id=${T:-0}
                AND subtype='comment' AND body ~ '/web/content/[0-9]+'")" \
     "one comment, carrying its image"
t_ge "$(pg "SELECT count(*) FROM mail_message WHERE res_model='project.task' AND res_id=${T:-0} AND subtype='tracking'")" \
     "4" "the history holds creation, status, assignee and label"

verdict
