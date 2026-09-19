#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------

# =============================================================
# The issue tracker: project.task as a ticket (migrations 1100-1101).
#
# Asked for: "proper project management with issues tracker, project tasks
# managements, issues update with image upload etc... assignments, similar to
# a jira system ... I want to use it to manage the development of this c-erp."
#
# What this file pins down, at the API:
#   1. every project has a ticket-key prefix — typed, or derived from the name —
#      unique, and in a shape a person can type (CERP);
#   2. every ticket gets the next key in its project (CERP-12), from the
#      database, uniquely, even when many are created at the same instant; a
#      client can neither set nor edit a key;
#   3. type and priority are a closed vocabulary;
#   4. every change is written to the ticket's history as words
#      ("Status: New → In Progress"), from the form and from a board drag;
#   5. comments are authored by the SESSION, never by whoever the client
#      names, and only their author (or an admin) can edit or delete them;
#   6. labels are created by typing a name, matched ignoring case;
#   7. watching, the board's card fields and its filters.
#
# The click-driven journey is tests/functional/project/issue-tracker.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
PFX='ZZIT'
TMP=$(mktemp -d)

py(){ python3 -c "$1" 2>/dev/null; }
export PYTHONIOENCODING=utf-8
# jget <json> <python expression over r, the "result">
jget(){ printf '%s' "$1" | python3 -c "import json,sys
d=json.load(sys.stdin); r=d.get('result')
try:
    v=($2)
except Exception as e:
    v='<err:'+str(e)+'>'
print(v if not isinstance(v,(dict,list)) else json.dumps(v))" 2>/dev/null; }

cleanup() {
    pg "DELETE FROM mail_message WHERE res_model='project.task' AND res_id IN
          (SELECT t.id FROM project_task t JOIN project_project p ON p.id=t.project_id
            WHERE p.name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM ir_attachment WHERE res_model='project.task' AND res_id IN
          (SELECT t.id FROM project_task t JOIN project_project p ON p.id=t.project_id
            WHERE p.name LIKE '${PFX}%')" >/dev/null 2>&1
    pg "DELETE FROM project_project WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    pg "DELETE FROM project_tag WHERE lower(name) LIKE 'zzit%'" >/dev/null 2>&1
    pg "DELETE FROM res_users WHERE login LIKE 'zzit_%'" >/dev/null 2>&1
    pg "DELETE FROM res_partner WHERE name LIKE '${PFX}%'" >/dev/null 2>&1
    rm -rf "$TMP"
}
cleanup
TMP=$(mktemp -d)
trap cleanup EXIT
auth_or_die
ADMIN=$(pg "SELECT id FROM res_users WHERE login='admin'")

# -------------------------------------------------------------------------
sec "1. every project has a ticket-key prefix"
# -------------------------------------------------------------------------
P1=$(call project.project create "[{\"name\":\"${PFX} Tracker\",\"task_prefix\":\"zzit\"}]" | rid)
t_nonempty "$P1" "a project is created with a prefix typed in lower case"
t_eq "ZZIT" "$(pg "SELECT task_prefix FROM project_project WHERE id=${P1:-0}")" "the prefix is stored in upper case"

P2=$(call project.project create "[{\"name\":\"${PFX} Other Board\"}]" | rid)
PFX2=$(pg "SELECT task_prefix FROM project_project WHERE id=${P2:-0}")
echo "    derived prefix for '${PFX} Other Board': $PFX2"
if [[ "$PFX2" =~ ^[A-Z][A-Z0-9]{1,9}$ ]]; then ok "a project without one is given a prefix from its name ($PFX2)"
else no "no usable prefix was derived: '$PFX2'"; fi

R=$(call project.project create "[{\"name\":\"${PFX} Clash\",\"task_prefix\":\"ZZIT\"}]")
has_error "$R" && ok "a prefix already in use is refused" || no "a duplicate prefix was accepted"
t_contains "$R" "already used" "and the refusal names the clash"
R=$(call project.project create "[{\"name\":\"${PFX} Bad\",\"task_prefix\":\"1AB\"}]")
has_error "$R" && ok "a prefix not starting with a letter is refused" || no "prefix 1AB was accepted"
R=$(call project.project write "[[${P2}], {\"task_prefix\":\"ZZIT\"}]")
has_error "$R" && ok "renaming a prefix onto another project's is refused" || no "a rename onto ZZIT was accepted"

# -------------------------------------------------------------------------
sec "2. keys come from the database, in order, uniquely"
# -------------------------------------------------------------------------
T1=$(call project.task create "[{\"name\":\"${PFX} first ticket\",\"project_id\":${P1}}]" | rid)
T2=$(call project.task create "[{\"name\":\"${PFX} second ticket\",\"project_id\":${P1}}]" | rid)
t_eq "ZZIT-1" "$(pg "SELECT key FROM project_task WHERE id=${T1:-0}")" "the first ticket is ZZIT-1"
t_eq "ZZIT-2" "$(pg "SELECT key FROM project_task WHERE id=${T2:-0}")" "the second is ZZIT-2"
DN=$(pgv "SELECT display_name FROM project_task WHERE id=${T2:-0}" | sed 's/^ *//;s/ *$//')
t_eq "ZZIT-2 ${PFX} second ticket" "$DN" "display_name carries the key, so a picker finds it by key"

NEW_STAGE=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND NOT is_closed ORDER BY sequence, id LIMIT 1")
t_eq "$NEW_STAGE" "$(pg "SELECT stage_id FROM project_task WHERE id=${T1:-0}")" \
     "a ticket created without a stage lands in the first open column (not on no column at all)"
t_eq "$ADMIN" "$(pg "SELECT reporter_id FROM project_task WHERE id=${T1:-0}")" "the reporter is whoever filed it"
t_eq "1" "$(pg "SELECT count(*) FROM project_task_watcher_rel WHERE task_id=${T1:-0} AND user_id=$ADMIN")" \
     "and the reporter watches it"

R=$(call project.task write "[[${T1}], {\"key\":\"HACK-9\",\"number\":99,\"display_name\":\"x\"}]")
t_eq "ZZIT-1" "$(pg "SELECT key FROM project_task WHERE id=${T1}")" "a client cannot set a key through write"
pg "UPDATE project_task SET key='HACK-1', number=77 WHERE id=${T1}" >/dev/null
t_eq "ZZIT-1" "$(pg "SELECT key FROM project_task WHERE id=${T1}")" "nor even through SQL: the trigger re-derives it"

# Ten at once. number comes from UPDATE ... RETURNING on the project row, which
# row-locks it, so parallel creates queue rather than draw the same number.
for i in $(seq 1 10); do
    call project.task create "[{\"name\":\"${PFX} burst $i\",\"project_id\":${P1}}]" > "$TMP/burst.$i" &
done
wait
t_eq "10" "$(pg "SELECT count(DISTINCT number) FROM project_task WHERE project_id=$P1 AND name LIKE '${PFX} burst%'")" \
     "ten tickets created at the same instant got ten different numbers"
t_eq "12" "$(pg "SELECT max(number) FROM project_task WHERE project_id=$P1")" "and they are 3..12 — no gaps, no repeats"
t_eq "12" "$(pg "SELECT task_seq FROM project_project WHERE id=$P1")" "the project's counter agrees"

R=$(call project.task write "[[${T2}], {\"project_id\":${P2}}]")
t_eq "${PFX2}-1" "$(pg "SELECT key FROM project_task WHERE id=${T2}")" \
     "a ticket moved to another project takes that project's next key"

call project.project write "[[${P1}], {\"task_prefix\":\"ZZIX\"}]" >/dev/null
t_eq "ZZIX-1" "$(pg "SELECT key FROM project_task WHERE id=${T1}")" "renaming a prefix re-keys the project's tickets"
call project.project write "[[${P1}], {\"task_prefix\":\"ZZIT\"}]" >/dev/null
t_eq "ZZIT-1" "$(pg "SELECT key FROM project_task WHERE id=${T1}")" "and renaming it back restores them"

# -------------------------------------------------------------------------
sec "3. type and priority are a closed vocabulary"
# -------------------------------------------------------------------------
R=$(call project.task write "[[${T1}], {\"issue_type\":\"bug\",\"priority\":1}]")
has_error "$R" && no "setting type bug, priority High failed: $R" || ok "type Bug and priority High are accepted"
R=$(call project.task write "[[${T1}], {\"issue_type\":\"epic\"}]")
has_error "$R" && ok "an unknown type is refused" || no "type 'epic' was accepted"
t_contains "$R" "Type must be" "with a readable reason"
R=$(call project.task write "[[${T1}], {\"priority\":5}]")
has_error "$R" && ok "an out-of-range priority is refused" || no "priority 5 was accepted"
R=$(call project.task create "[{\"name\":\"${PFX} typed\",\"project_id\":${P1},\"issue_type\":\"feature\",\"priority\":\"2\"}]")
TF=$(echo "$R" | rid)
t_eq "feature|2" "$(pg "SELECT issue_type || '|' || priority FROM project_task WHERE id=${TF:-0}")" \
     "a priority sent as the string \"2\" (a selection value) is stored as 2"

# -------------------------------------------------------------------------
sec "4. every change is written to the history, as words"
# -------------------------------------------------------------------------
PROG=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND name='In Progress' LIMIT 1")
DONE=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND is_closed ORDER BY sequence LIMIT 1")
REVIEW=$(pg "SELECT id FROM project_task_type WHERE project_id IS NULL AND name='Review' LIMIT 1")
call project.task write "[[${T1}], {\"stage_id\":${PROG},\"priority\":2,\"user_id\":${ADMIN}}]" >/dev/null
ACT=$(call project.task activity "[{\"task_id\":${T1}}]")
LAST=$(jget "$ACT" "[m['body'] for m in r if m['subtype']=='tracking'][-1]")
echo "    last history entry: $(echo "$LAST" | tr '\n' '|')"
t_contains "$LAST" "Status: New → In Progress" "the status change is recorded by name"
t_contains "$LAST" "Priority: High → Urgent"   "the priority change is recorded as words, not numbers"
t_contains "$LAST" "Assignee: — → "            "the assignment is recorded"
FIRST=$(jget "$ACT" "r[0]['body']")
t_eq "Created this task" "$FIRST" "the history starts with the ticket being created"

call project.task move_stage "[{\"task_id\":${T1},\"stage_id\":${REVIEW},\"index\":0}]" >/dev/null
ACT=$(call project.task activity "[{\"task_id\":${T1}}]")
t_contains "$(jget "$ACT" "[m['body'] for m in r if m['subtype']=='tracking'][-1]")" \
     "Status: In Progress → Review" "a drag on the board is recorded too"

call project.task write "[[${T1}], {\"stage_id\":${DONE}}]" >/dev/null
t_nonempty "$(pg "SELECT date_end FROM project_task WHERE id=${T1}")" "closing from the form stamps the close date"
call project.task write "[[${T1}], {\"stage_id\":${PROG}}]" >/dev/null
t_eq "" "$(pg "SELECT date_end FROM project_task WHERE id=${T1}")" "and reopening clears it"

N0=$(pg "SELECT count(*) FROM mail_message WHERE res_model='project.task' AND res_id=${T1}")
call project.task write "[[${T1}], {\"priority\":2}]" >/dev/null
t_eq "$N0" "$(pg "SELECT count(*) FROM mail_message WHERE res_model='project.task' AND res_id=${T1}")" \
     "a write that changes nothing adds no history"

# -------------------------------------------------------------------------
sec "5. comments belong to whoever is signed in"
# -------------------------------------------------------------------------
C1=$(call project.task post_comment "[{\"task_id\":${T1},\"body\":\"  Looks wrong on mobile ![shot](/web/content/1)  \"}]" | rid)
t_nonempty "$C1" "a comment is posted"
t_eq "Looks wrong on mobile ![shot](/web/content/1)" \
     "$(pgv "SELECT body FROM mail_message WHERE id=${C1:-0}" | sed 's/^ *//;s/ *$//')" \
     "stored trimmed, image reference intact"
R=$(call project.task post_comment "[{\"task_id\":${T1},\"body\":\"   \"}]")
has_error "$R" && ok "an empty comment is refused" || no "an empty comment was accepted"

PART=$(call res.partner create "[{\"name\":\"${PFX} Dev\",\"email\":\"zzit_dev@t.test\"}]" | rid)
U2=$(call res.users create "[{\"login\":\"zzit_dev@t.test\",\"password\":\"Zzit-Pass-1\",\"partner_id\":${PART},\"active\":true}]" | rid)
# An ordinary employee: Internal User (2), nothing more. res.users create does
# not add it, and without it every call below is refused for the WRONG reason
# ("internal login required") — which three of these checks once passed on.
pg "INSERT INTO res_groups_users_rel (gid, uid) VALUES (2, ${U2:-0}) ON CONFLICT DO NOTHING" >/dev/null
# ...working in the ticket's company. Without it the ORM rightly hides the
# ticket (docs/094) and the answer is "No such task".
CO=$(pg "SELECT company_id FROM project_task WHERE id=${T1}")
pg "UPDATE res_users SET company_id=${CO:-1} WHERE id=${U2:-0}" >/dev/null
pg "INSERT INTO res_company_users_rel (company_id, user_id) VALUES (${CO:-1}, ${U2:-0}) ON CONFLICT DO NOTHING" >/dev/null
S2=$(login 'zzit_dev@t.test' 'Zzit-Pass-1')
t_nonempty "$S2" "a second, non-admin user can sign in"
# They name the admin as the author. The server must ignore it.
C2=$(call_as "$S2" project.task post_comment "[{\"task_id\":${T1},\"body\":\"mine\",\"author_id\":${ADMIN}}]" | rid)
t_nonempty "$C2" "the second user can comment on the ticket"
t_eq "${U2}" "$(pg "SELECT author_id FROM mail_message WHERE id=${C2:-0}")" \
     "the author is the session's user, whatever the client claims"
t_eq "1" "$(pg "SELECT count(*) FROM project_task_watcher_rel WHERE task_id=${T1} AND user_id=${U2:-0}")" \
     "commenting makes you a watcher"

R=$(call_as "$S2" project.task edit_comment "[{\"message_id\":${C1},\"body\":\"defaced\"}]")
has_error "$R" && ok "another user cannot edit your comment" || no "user 2 edited the admin's comment"
t_contains "$R" "Only the author" "and is told why"
R=$(call_as "$S2" project.task delete_comment "[{\"message_id\":${C1}}]")
has_error "$R" && ok "nor delete it" || no "user 2 deleted the admin's comment"
R=$(call_as "$S2" project.task edit_comment "[{\"message_id\":${C2},\"body\":\"mine, edited\"}]")
has_error "$R" && no "editing your own comment failed: $R" || ok "you can edit your own comment"
ACT=$(call project.task activity "[{\"task_id\":${T1}}]")
t_eq "True" "$(jget "$ACT" "[m['edited'] for m in r if m['id']==${C2}][0]")" "and it is marked as edited"
t_eq "True" "$(jget "$ACT" "[m for m in r if m['id']==${C1}][0]['can_edit']")" \
     "the admin's own comment is editable by the admin"
R=$(call project.task delete_comment "[{\"message_id\":${C2}}]")
has_error "$R" && no "an admin could not remove a comment: $R" || ok "an admin can remove any comment"
R=$(call project.task edit_comment "[{\"message_id\":$(pg "SELECT id FROM mail_message WHERE res_id=${T1} AND res_model='project.task' AND subtype='tracking' LIMIT 1"),\"body\":\"rewritten history\"}]")
has_error "$R" && ok "a history entry is not a comment and cannot be edited" || no "a tracking entry was edited"

# -------------------------------------------------------------------------
sec "6. labels: type a name, matched ignoring case"
# -------------------------------------------------------------------------
call project.task set_tags "[{\"task_id\":${T1},\"tags\":[\"zzit-ui\",\"ZZIT-Regression\"]}]" >/dev/null
t_eq "2" "$(pg "SELECT count(*) FROM project_task_tag_rel WHERE task_id=${T1}")" "two labels are attached, created on the fly"
call project.task set_tags "[{\"task_id\":${T1},\"tags\":[\"ZZIT-UI\"]}]" >/dev/null
t_eq "1" "$(pg "SELECT count(*) FROM project_tag WHERE lower(name)='zzit-ui'")" "typing it in another case reuses the label"
t_eq "zzit-ui" "$(pg "SELECT g.name FROM project_task_tag_rel x JOIN project_tag g ON g.id=x.tag_id WHERE x.task_id=${T1}")" \
     "and the other label is removed"
ACT=$(call project.task activity "[{\"task_id\":${T1}}]")
t_contains "$(jget "$ACT" "[m['body'] for m in r if m['subtype']=='tracking'][-1]")" \
     "Labels: ZZIT-Regression, zzit-ui → zzit-ui" "the label change is in the history (alphabetical, ignoring case)"
TAG=$(pg "SELECT id FROM project_tag WHERE lower(name)='zzit-ui'")

# -------------------------------------------------------------------------
sec "7. watching"
# -------------------------------------------------------------------------
call project.task watch "[{\"task_id\":${T1},\"watch\":false}]" >/dev/null
D=$(call project.task task_detail "[{\"id\":${T1}}]")
t_eq "False" "$(jget "$D" "r['watching']")" "you can stop watching"
call project.task watch "[{\"task_id\":${T1},\"watch\":true}]" >/dev/null
D=$(call project.task task_detail "[{\"id\":${T1}}]")
t_eq "True" "$(jget "$D" "r['watching']")" "and start again"

# -------------------------------------------------------------------------
sec "8. the ticket screen and the board"
# -------------------------------------------------------------------------
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > "$TMP/shot.png"
UP=$(curl -s -H "Cookie: session_id=$SID" -F "file=@$TMP/shot.png;type=image/png" \
          -F "res_model=project.task" -F "res_id=${T1}" "$BASE/web/attachment/upload")
t_contains "$UP" '"id"' "a screenshot is uploaded onto the ticket"

D=$(call project.task task_detail "[{\"id\":${T1}}]")
t_eq "ZZIT-1"  "$(jget "$D" "r['key']")"             "task_detail: the key"
t_eq "bug"     "$(jget "$D" "r['issue_type']")"      "the type"
t_eq "2"       "$(jget "$D" "r['priority']")"        "the priority"
t_eq "1"       "$(jget "$D" "r['attachment_count']")" "the attachment count"
t_eq "1"       "$(jget "$D" "r['comment_count']")"   "the comment count (the deleted one is gone)"
t_eq "zzit-ui" "$(jget "$D" "','.join(t['name'] for t in r['tags'])")" "the labels"
t_ge "$(jget "$D" "len(r['stages'])")" "2" "the status choices (the board's columns)"
t_eq "ZZIT" "$(jget "$D" "r['project_prefix']")" "the project's prefix"

B=$(call project.task board "[{\"project_id\":${P1}}]")
t_eq "ZZIT-1|bug|1|1|zzit-ui" \
     "$(jget "$B" "'|'.join(str(x) for x in next([c['key'], c['issue_type'], c['comment_count'], c['attachment_count'], ','.join(t['name'] for t in c['tags'])] for c in r['tasks'] if c['id']==${T1}))")" \
     "a board card carries key, type, comment and file counts, labels"
B=$(call project.task board "[{\"project_id\":${P1},\"issue_type\":\"bug\"}]")
t_eq "ZZIT-1" "$(jget "$B" "','.join(c['key'] for c in r['tasks'])")" "filter by type: only the bug"
B=$(call project.task board "[{\"project_id\":${P1},\"tag_id\":${TAG}}]")
t_eq "ZZIT-1" "$(jget "$B" "','.join(c['key'] for c in r['tasks'])")" "filter by label: only the labelled ticket"
B=$(call project.task board "[{\"project_id\":${P1},\"q\":\"typed\"}]")
t_eq "${PFX} typed" "$(jget "$B" "','.join(c['name'] for c in r['tasks'])")" "search by words in the title"
B=$(call project.task board "[{\"project_id\":${P1},\"q\":\"zzit-1\"}]")
t_contains "$(jget "$B" "','.join(c['key'] for c in r['tasks'])")" "ZZIT-1" "search by key, any case"
B=$(call project.task board "[{\"project_id\":${P1},\"q\":\"%\"}]")
t_eq "0" "$(jget "$B" "len(r['tasks'])")" "a typed % is a character, not a wildcard"

# -------------------------------------------------------------------------
sec "9. access and clean-up"
# -------------------------------------------------------------------------
R=$(call_as '' project.task task_detail "[{\"id\":${T1}}]")
has_error "$R" && ok "an anonymous caller cannot read a ticket" || no "task_detail answered without a session"
R=$(call_as '' project.task post_comment "[{\"task_id\":${T1},\"body\":\"spam\"}]")
has_error "$R" && ok "nor comment on one" || no "an anonymous comment was accepted"
R=$(call project.task task_detail "[{\"id\":99999999}]")
has_error "$R" && ok "a ticket that does not exist is refused" || no "task_detail invented a ticket"

call project.task unlink "[[${T1}]]" >/dev/null
t_eq "0" "$(pg "SELECT count(*) FROM mail_message WHERE res_model='project.task' AND res_id=${T1}")" \
     "deleting a ticket deletes its comments and history with it"

verdict
