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
# Database Tools — Settings ▸ Database Tools (docs/093).
#
# The screen hands an admin a SQL prompt against the live company database, so
# the assertions that matter here are the ones about what it REFUSES.
#
# The load-bearing one is the read-only transaction. A keyword filter can be
# out-thought — `WITH x AS (DELETE ...) SELECT ...` starts with WITH and writes
# anyway — so the real defence is that every statement runs inside a PostgreSQL
# READ ONLY transaction that is never committed. `SELECT nextval(...)` is the
# clean probe for it: perfectly ordinary SQL, blocked by nothing except a
# genuine READ ONLY transaction. If that check ever goes green-by-accident the
# whole screen is unsafe, so it is asserted on the error text, not just on
# "did it fail".
#
# The second class is identifier handling (S-49): a table or column name from
# the client is resolved against pg_catalog before it is quoted into SQL, so
# naming pg_authid or smuggling a quote into an ORDER BY gets a flat refusal
# rather than a query.
#
# The third is that none of this is reachable without being an admin.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; echo "*** FAILURES ***"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"

# tool <json-params-without-braces>
tool(){ curl -s -X POST "$BASE/web/dbtool" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{$1,$CTX}}"; }
# q <sql> — run one statement through the console
q(){ tool "\"op\":\"query\",\"sql\":$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$/"/')"; }

has(){ echo "$2" | grep -q "$1"; }

echo "############ the endpoint answers at all ############"
R=$(tool '"op":"overview"')
has '"ok":true' "$R" && ok "overview responds" || { no "overview failed: $(echo "$R" | head -c 160)"; echo "*** FAILURES ***"; exit 1; }
DBNAME=$(echo "$R" | sed -n 's/.*"database":"\([^"]*\)".*/\1/p')
[ "$DBNAME" = "$DBN" ] && ok "it reports the caller's own database ($DBNAME)" || no "wrong database: $DBNAME"
has '"fk_count"' "$R" && ok "overview counts foreign keys" || no "no fk_count"
has '"modules"' "$R"  && ok "overview rolls storage up by module" || no "no module rollup"

echo "############ writes are refused by the transaction, not by a keyword list ############"
# nextval() is ordinary read-looking SQL that only a READ ONLY transaction stops.
R=$(q "SELECT nextval('res_users_id_seq')")
has 'read-only transaction' "$R" \
    && ok "nextval() refused: the transaction really is READ ONLY" \
    || no "nextval() was NOT refused by a read-only transaction: $(echo "$R" | head -c 200)"

# A data-modifying CTE passes any check that only looks at the first keyword.
BEFORE=$(pg "SELECT count(*) FROM ir_attachment")
R=$(q "WITH d AS (DELETE FROM ir_attachment RETURNING id) SELECT count(*) FROM d")
has '"error"' "$R" && ok "data-modifying CTE refused" || no "data-modifying CTE ran: $R"
AFTER=$(pg "SELECT count(*) FROM ir_attachment")
[ "$BEFORE" = "$AFTER" ] && ok "and it deleted nothing ($BEFORE rows before and after)" \
                         || no "rows changed: $BEFORE -> $AFTER"

for stmt in "UPDATE res_partner SET name='x'" \
            "DELETE FROM res_partner" \
            "DROP TABLE res_users" \
            "CREATE TABLE qa_dbtool (id int)" \
            "ALTER TABLE res_users ADD COLUMN qa int" \
            "GRANT ALL ON res_users TO PUBLIC"; do
    R=$(q "$stmt")
    has '"error"' "$R" && ok "refused: ${stmt:0:38}" || no "ACCEPTED: $stmt"
done
[ -z "$(pg "SELECT to_regclass('public.qa_dbtool')")" ] && ok "no table was created" || no "qa_dbtool exists"

echo "############ one statement per box ############"
R=$(q "SELECT 1; DROP TABLE res_users")
has 'One statement' "$R" && ok "a second statement is rejected" || no "multi-statement allowed: $R"
R=$(q "SELECT 'a;b' AS v")
has '"ok":true' "$R" && ok "but a semicolon inside a string literal is fine" || no "literal semicolon rejected: $R"
R=$(q "SELECT 1 AS v;")
has '"ok":true' "$R" && ok "and so is a trailing semicolon" || no "trailing semicolon rejected: $R"
R=$(q "SELECT 1 -- ; not a statement")
has '"ok":true' "$R" && ok "and so is one inside a comment" || no "comment semicolon rejected: $R"

echo "############ the role catalogues stay shut ############"
for t in pg_authid pg_shadow; do
    R=$(q "SELECT * FROM $t")
    has '"error"' "$R" && ok "$t is not queryable" || no "$t was readable!"
done
R=$(tool '"op":"rows","table":"pg_authid"')
has 'No such table' "$R" && ok "and it is not browsable either" || no "pg_authid browsable: $R"

echo "############ identifiers are resolved, not just filtered (S-49) ############"
R=$(tool '"op":"rows","table":"res_partner","order":"id; DROP TABLE x"')
has 'No such column' "$R" && ok "an ORDER BY that is not a real column is refused" || no "bad order accepted: $R"
R=$(tool '"op":"rows","table":"res_partner","filter":{"col":"x\" FROM res_users --","op":"eq","value":"1"}')
has 'No such column' "$R" && ok "so is a filter column carrying a quote" || no "bad filter column accepted: $R"
R=$(tool '"op":"rows","table":"no_such_table_here"')
has 'No such table' "$R" && ok "so is an unknown table" || no "unknown table accepted: $R"
R=$(tool '"op":"rows","table":"res_partner","filter":{"col":"id","op":"gt","value":"abc"}')
has 'not a valid' "$R" && ok "a mistyped filter value explains itself" || no "unhelpful type error: $R"

echo "############ credentials are masked on the way out ############"
R=$(tool '"op":"rows","table":"res_users"')
has '"masked":true' "$R" && ok "the password column is flagged masked" || no "password not flagged"
HASH=$(pg "SELECT substring(password,1,12) FROM res_users ORDER BY id LIMIT 1")
if [ -n "$HASH" ] && has "$HASH" "$R"; then no "the password hash was sent to the browser"; else ok "and its value never leaves the server"; fi
R=$(tool '"op":"profile","table":"res_users","column":"password"')
has 'credentials' "$R" && ok "profiling a credential column is refused" || no "password profiled: $R"

echo "############ the browser itself works ############"
R=$(tool '"op":"tables"')
has '"name":"account_move"' "$R" && ok "tables lists the schema" || no "tables missing account_move"
has '"kind":"view"' "$R"        && ok "and distinguishes views from tables" || no "no views reported"
R=$(tool '"op":"table","table":"account_move"')
has '"pk":true' "$R"          && ok "table detail marks the primary key" || no "no pk"
has '"referenced_by"' "$R"    && ok "and reports both directions of every FK" || no "no referenced_by"
has '"indexes"' "$R"          && ok "and lists indexes" || no "no indexes"
R=$(tool '"op":"rows","table":"account_move","limit":5,"order":"id","dir":"desc"')
has '"ok":true' "$R" && ok "rows pages and sorts" || no "rows failed: $R"
LIM=$(echo "$R" | sed -n 's/.*"limit":\([0-9]*\).*/\1/p')
[ "$LIM" = "5" ] && ok "and honours the page size" || no "limit was $LIM"
R=$(tool '"op":"rows","table":"account_move","limit":99999')
LIM=$(echo "$R" | sed -n 's/.*"limit":\([0-9]*\).*/\1/p')
[ "$LIM" = "500" ] && ok "and clamps an oversized one to 500" || no "limit clamp gave $LIM"
R=$(tool '"op":"graph"')
has '"nodes"' "$R" && has '"edges"' "$R" && ok "the schema map has nodes and edges" || no "graph incomplete"
R=$(tool '"op":"profile","table":"account_move","column":"state"')
has '"top_values"' "$R" && ok "column profiling returns a distribution" || no "no top_values"

echo "############ results are capped ############"
R=$(q "SELECT generate_series(1,5000) AS n")
has '"truncated":true' "$R" && ok "a huge result set is truncated, not streamed whole" || no "not truncated: $(echo "$R" | head -c 120)"

echo "############ none of it is reachable without admin ############"
R=$(curl -s -X POST "$BASE/web/dbtool" -H 'Content-Type: application/json' \
     --data '{"jsonrpc":"2.0","method":"call","params":{"op":"tables"}}')
has 'not authenticated' "$R" && ok "anonymous callers are turned away" || no "anonymous access: $R"
R=$(curl -s -X POST "$BASE/web/dbtool" -H 'Content-Type: application/json' \
     --data '{"jsonrpc":"2.0","method":"call","params":{"op":"query","sql":"SELECT 1","context":{"session_id":"deadbeef"}}}')
has 'not authenticated' "$R" && ok "so is a forged session id" || no "forged session accepted: $R"

echo "############ the browser client and the server agree on the envelope ############"
# THE CHECK THAT WAS MISSING, and the reason a completely blank screen shipped.
#
# Everything above drives the server with curl and reads the raw JSON. The screen
# does not: it goes through RpcService.dbTool(), which unwraps the response. The
# two disagreed — RpcService._dbPost already peels a top-level `result` (its
# JSON-RPC unwrap) and dbTool peeled it a second time — so every call returned
# {} and every panel rendered empty. Both sides were individually "correct" and
# no test ran them together.
#
# So: feed the REAL server response to the REAL client function, in node.
if command -v node >/dev/null 2>&1; then
    RESP=$(tool '"op":"overview"')
    OUT=$(cd "$ERP_ROOT" && node -e '
        const fs = require("fs"), vm = require("vm");
        const resp = process.argv[1];
        global.window   = { localStorage: { getItem: () => null, setItem: () => {} } };
        global.document = { cookie: "" };
        global.fetch    = async () => ({ ok: true, json: async () => JSON.parse(resp) });
        vm.runInThisContext(fs.readFileSync("web/static/src/services/rpc.js", "utf8")
                            + ";globalThis.__R = RpcService;");
        globalThis.__R.dbTool("overview")
            .then(r => console.log(r && r.database ? "OK" : "EMPTY:" + JSON.stringify(r).slice(0, 60)))
            .catch(e => console.log("THROW:" + e.message));
    ' "$RESP" 2>&1 | tail -1)
    [ "$OUT" = "OK" ] && ok "RpcService.dbTool() unwraps what the server actually sends" \
                      || no "client/server envelope mismatch -> $OUT"
else
    ok "(node unavailable — client envelope check skipped)"
fi

echo "############ the menu reaches the screen ############"
[ "$(pg "SELECT res_model FROM ir_act_window WHERE id=101")" = "db.studio" ] \
    && ok "action 101 opens db.studio" || no "action 101 wrong"
[ "$(pg "SELECT parent_id FROM ir_ui_menu WHERE id=74")" = "30" ] \
    && ok "menu 74 sits under Settings" || no "menu 74 not under Settings"
[ "$(pg "SELECT action_id FROM ir_ui_menu WHERE id=74")" = "101" ] \
    && ok "and points at action 101" || no "menu 74 points elsewhere"
curl -s "$BASE/src/components/DbStudio.js" | grep -q 'class DbStudio' \
    && ok "the component is served" || no "DbStudio.js not served"
curl -s "$BASE/src/components/dbstudio.css" | grep -q 'db-studio' \
    && ok "so is its stylesheet" || no "dbstudio.css not served"
# The application shell moved from "/" to "/login" — "/" is the public
# website now (docs/126).
curl -s "$BASE/login" | grep -q 'DbStudio.js' \
    && ok "and index.html loads it" || no "index.html does not load DbStudio.js"

if [ -n "$FAILED" ]; then echo; echo "*** FAILURES ***"; exit 1; fi
echo; echo "  All checks passed."
