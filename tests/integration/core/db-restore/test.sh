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
# Snapshot and restore (docs/103).
#
# run_tests.sh now ends by restoring the database to its pre-suite state. That
# step is only worth having if it actually works, and a restore that fails is
# far worse than no restore — it leaves the database in whatever half-state the
# failure produced. So this proves the round trip end to end:
#
#   take a snapshot  ->  change the database  ->  restore  ->  the change is gone
#
# and, just as important, that everything ELSE came back: table count, a row
# count, and a specific pre-existing row are all compared across the cycle.
#
# NOTE: this script restores the database, so it necessarily restarts the
# server. It is safe to run at any time — it puts back exactly what was there.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

SNAP=/tmp/verify_restore_$$.dump
cleanup(){
    pg "DELETE FROM help_article WHERE slug='qa-restore-marker'" >/dev/null
    rm -f "$SNAP"
}
trap cleanup EXIT

echo "############ take ############"
bash scripts/db_snapshot.sh take "$SNAP" | sed 's/^/    /'
[ -s "$SNAP" ] && ok "a snapshot file was written" || { no "no snapshot produced"; echo "  *** FAILURES ***"; exit 1; }
bash scripts/db_snapshot.sh verify "$SNAP" >/dev/null 2>&1 \
  && ok "the snapshot is a readable archive" || no "the snapshot is not readable"

# What the database looked like before we broke it.
TABLES_BEFORE=$(pg "SELECT count(*) FROM pg_tables WHERE schemaname='public'")
ARTICLES_BEFORE=$(pg "SELECT count(*) FROM help_article")
MOVES_BEFORE=$(pg "SELECT count(*) FROM account_move")
SENTINEL=$(pg "SELECT title FROM help_article WHERE slug='project-overview'")
echo "    before: $TABLES_BEFORE tables, $ARTICLES_BEFORE articles, $MOVES_BEFORE moves"

echo "############ change the database ############"
pg "INSERT INTO help_article (book, slug, title, body, sequence, is_section, active)
    VALUES ('project','qa-restore-marker','QA restore marker','x',999,false,true)" >/dev/null
pg "DELETE FROM help_article WHERE slug='project-faq'" >/dev/null
MARKER=$(pg "SELECT count(*) FROM help_article WHERE slug='qa-restore-marker'")
GONE=$(pg "SELECT count(*) FROM help_article WHERE slug='project-faq'")
[ "$MARKER" = "1" ] && ok "an added row is present before the restore" || no "could not add the marker row"
[ "$GONE" = "0" ] && ok "a deleted row is absent before the restore" || no "could not delete a row"

echo "############ restore ############"
bash scripts/db_snapshot.sh restore "$SNAP" 2>&1 | sed 's/^/    /'

echo "############ the database came back ############"
# An added row must be gone...
[ "$(pg "SELECT count(*) FROM help_article WHERE slug='qa-restore-marker'")" = "0" ] \
  && ok "the added row was rolled back" || no "the added row survived the restore"
# ...and a deleted row must be back. Restoring only additions would be a wipe,
# not a restore.
[ "$(pg "SELECT count(*) FROM help_article WHERE slug='project-faq'")" = "1" ] \
  && ok "the deleted row came back" || no "the deleted row did not come back"

TABLES_AFTER=$(pg "SELECT count(*) FROM pg_tables WHERE schemaname='public'")
ARTICLES_AFTER=$(pg "SELECT count(*) FROM help_article")
MOVES_AFTER=$(pg "SELECT count(*) FROM account_move")
echo "    after:  $TABLES_AFTER tables, $ARTICLES_AFTER articles, $MOVES_AFTER moves"
[ "$TABLES_AFTER" = "$TABLES_BEFORE" ]     && ok "every table came back ($TABLES_AFTER)" || no "tables $TABLES_BEFORE -> $TABLES_AFTER"
[ "$ARTICLES_AFTER" = "$ARTICLES_BEFORE" ] && ok "help_article row count matches"        || no "articles $ARTICLES_BEFORE -> $ARTICLES_AFTER"
[ "$MOVES_AFTER" = "$MOVES_BEFORE" ]       && ok "account_move row count matches"        || no "moves $MOVES_BEFORE -> $MOVES_AFTER"
[ "$(pg "SELECT title FROM help_article WHERE slug='project-overview'")" = "$SENTINEL" ] \
  && ok "a specific pre-existing row is unchanged" || no "the sentinel row's content changed"

echo "############ the server is usable again ############"
# The restore stops the server to take locks and must bring it back, or the
# suite would end with a green summary and a dead application.
for _ in $(seq 1 20); do curl -sf -o /dev/null --max-time 2 "$BASE/healthz" && break; sleep 1; done
curl -sf -o /dev/null --max-time 3 "$BASE/healthz" && ok "the server is back up" || no "the server did not restart"
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -n "$SID" ] && ok "a user can still log in after the restore" || no "login failed after the restore"

echo "############ guards ############"
bash scripts/db_snapshot.sh restore /tmp/definitely_not_a_dump_$$ 2>&1 | grep -qi 'nothing to restore' \
  && ok "restoring a missing file is refused" || no "a missing snapshot was not refused"
echo "not a dump" > /tmp/bad_dump_$$
bash scripts/db_snapshot.sh restore /tmp/bad_dump_$$ 2>&1 | grep -qi 'unreadable' \
  && ok "restoring a corrupt file is refused" || no "a corrupt snapshot was not refused"
rm -f /tmp/bad_dump_$$

[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"
