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
# ir.attachment end to end (docs/091).
#
# The model, the content-addressed filestore and both HTTP routes were built
# long ago and covered by verify_ir_primitives.sh; what was missing was any
# way for a user to reach them, and any cleanup when a file is removed.
#
# The load-bearing assertions here are the two that a naive delete gets wrong:
#
#   * removing an attachment must RECLAIM its bytes — Filestore::gc() existed
#     and nothing called it, so every delete leaked a blob on disk forever;
#   * but storage is content-addressed, so two attachments of the same file
#     SHARE one blob. Deleting one of them must NOT pull the file out from
#     under the other.
#
# Plus the round trip the panel depends on: upload against a record, list it
# back for that record only, and download it byte-for-byte.
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
STORE=/home/user/code/c-erp/data/filestore
FAILED=
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
SID=$(curl -s -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" \
      | sed -n 's/.*"session_id":"\([a-f0-9]*\)".*/\1/p')
[ -z "$SID" ] && { echo "cannot authenticate"; exit 1; }
CTX="\"context\":{\"session_id\":\"$SID\"}"
COOK="Cookie: session_id=$SID"
call(){ curl -s -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"model\":\"$1\",\"method\":\"$2\",\"args\":$3,\"kwargs\":$4}}"; }
rid(){ sed -n 's/.*"result":\([0-9]*\).*/\1/p'; }
aid(){ sed -n 's/.*"id":\([0-9]*\).*/\1/p'; }

echo "############ fixture ############"
pg "DELETE FROM hr_expense WHERE name LIKE 'QA-ATT%'" >/dev/null
pg "DELETE FROM hr_expense_sheet WHERE name LIKE 'QA Attachment%'" >/dev/null
EMP=$(pg "SELECT id FROM hr_employee ORDER BY id LIMIT 1")
SH=$(call hr.expense.sheet create "[{\"name\":\"QA Attachment Report\",\"employee_id\":$EMP}]" "{$CTX}" | rid)
[ -n "$SH" ] && ok "expense report to attach to ($SH)" || { no "sheet create failed"; echo "*** FAILURES ***"; exit 1; }
SH2=$(call hr.expense.sheet create "[{\"name\":\"QA Attachment Report 2\",\"employee_id\":$EMP}]" "{$CTX}" | rid)

TMP=$(mktemp -d)
printf 'QA receipt body %s\n' "$(date +%s%N)" > "$TMP/receipt.txt"
printf 'a different file %s\n' "$(date +%s%N)" > "$TMP/other.txt"

echo "############ upload against a record ############"
R=$(curl -s -X POST "$BASE/web/attachment/upload" -H "$COOK" \
      -F "file=@$TMP/receipt.txt" -F "res_model=hr.expense.sheet" -F "res_id=$SH" -F "name=receipt.txt")
A1=$(echo "$R" | aid)
[ -n "$A1" ] && ok "file uploaded ($A1)" || { no "upload failed: $(echo "$R" | head -c 140)"; echo "*** FAILURES ***"; exit 1; }
[ "$(pg "SELECT res_model FROM ir_attachment WHERE id=$A1")" = "hr.expense.sheet" ] \
    && ok "it is linked to the right model" || no "res_model wrong"
[ "$(pg "SELECT res_id FROM ir_attachment WHERE id=$A1")" = "$SH" ] \
    && ok "it is linked to the right record" || no "res_id wrong"
SF1=$(pg "SELECT store_fname FROM ir_attachment WHERE id=$A1")
[ -f "$STORE/$SF1" ] && ok "the bytes are in the filestore" || no "no blob at $STORE/$SF1"

echo "############ the panel's list query ############"
L=$(call ir.attachment search_read "[[[\"res_model\",\"=\",\"hr.expense.sheet\"],[\"res_id\",\"=\",$SH]]]" "{$CTX}")
echo "$L" | grep -q '"url":"/web/content/' && ok "the list carries a download URL" || no "no url in the list"
echo "$L" | grep -q '"size_human"'         && ok "the list carries a human size"   || no "no size_human in the list"
echo "$L" | grep -q '"created"'            && ok "the list carries a timestamp"    || no "no created in the list"
# [0-9][0-9]* not [0-9]*: the latter matches zero digits, so the JSON-RPC
# envelope's own "id":null counted as a result and every list looked one longer.
CNT=$(echo "$L" | grep -o '"id":[0-9][0-9]*' | wc -l)
[ "$CNT" = "1" ] && ok "exactly this record's file is listed" || no "listed $CNT files, expected 1"
# A different record must not see it.
L2=$(call ir.attachment search_read "[[[\"res_model\",\"=\",\"hr.expense.sheet\"],[\"res_id\",\"=\",$SH2]]]" "{$CTX}")
[ "$(echo "$L2" | grep -o '"id":[0-9][0-9]*' | wc -l)" = "0" ] \
    && ok "another record's panel shows nothing" || no "attachments leaked across records"

echo "############ download round-trip ############"
curl -s -H "$COOK" "$BASE/web/content/$A1" -o "$TMP/back.txt"
cmp -s "$TMP/receipt.txt" "$TMP/back.txt" && ok "downloaded byte-for-byte" || no "download differs from the upload"
DISP=$(curl -s -D - -o /dev/null -H "$COOK" "$BASE/web/content/$A1?download=1" | grep -i 'content-disposition')
echo "$DISP" | grep -qi 'attachment' && ok "?download=1 forces a download" || no "no attachment disposition"
UNAUTH=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/web/content/$A1")
[ "$UNAUTH" = "401" ] && ok "an unauthenticated download is refused" || no "unauthenticated download returned $UNAUTH"

echo "############ dedup: the same file twice shares one blob ############"
R2=$(curl -s -X POST "$BASE/web/attachment/upload" -H "$COOK" \
      -F "file=@$TMP/receipt.txt" -F "res_model=hr.expense.sheet" -F "res_id=$SH2" -F "name=copy.txt")
A2=$(echo "$R2" | aid)
[ -n "$A2" ] && ok "the same file uploaded to a second record ($A2)" || no "second upload failed"
[ "$(pg "SELECT store_fname FROM ir_attachment WHERE id=$A2")" = "$SF1" ] \
    && ok "both rows point at ONE stored blob" || no "the blob was duplicated"

echo "############ delete releases bytes — but only the last reference ############"
call ir.attachment unlink "[[$A1]]" "{$CTX}" >/dev/null
[ "$(pg "SELECT count(*) FROM ir_attachment WHERE id=$A1")" = "0" ] && ok "the row is gone" || no "the row survived"
# THE regression this guards: the other attachment still needs those bytes.
[ -f "$STORE/$SF1" ] && ok "the shared blob survives while another row references it" \
                     || no "deleting one attachment destroyed a file another record still points at"
curl -s -H "$COOK" "$BASE/web/content/$A2" -o "$TMP/back2.txt"
cmp -s "$TMP/receipt.txt" "$TMP/back2.txt" && ok "the surviving attachment still downloads" || no "the surviving file is broken"

call ir.attachment unlink "[[$A2]]" "{$CTX}" >/dev/null
[ ! -f "$STORE/$SF1" ] && ok "the blob is reclaimed once the last row goes" \
                       || no "orphaned blob left on disk at $STORE/$SF1"

echo "############ an unrelated blob is never touched ############"
R3=$(curl -s -X POST "$BASE/web/attachment/upload" -H "$COOK" \
      -F "file=@$TMP/other.txt" -F "res_model=hr.expense.sheet" -F "res_id=$SH" -F "name=other.txt")
A3=$(echo "$R3" | aid)
SF3=$(pg "SELECT store_fname FROM ir_attachment WHERE id=$A3")
R4=$(curl -s -X POST "$BASE/web/attachment/upload" -H "$COOK" \
      -F "file=@$TMP/receipt.txt" -F "res_model=hr.expense.sheet" -F "res_id=$SH" -F "name=receipt2.txt")
A4=$(echo "$R4" | aid)
call ir.attachment unlink "[[$A4]]" "{$CTX}" >/dev/null
[ -f "$STORE/$SF3" ] && ok "deleting one file leaves the others alone" || no "an unrelated blob was removed"
call ir.attachment unlink "[[$A3]]" "{$CTX}" >/dev/null

echo "############ upload guards ############"
printf 'MZ fake executable' > "$TMP/evil.exe"
E=$(curl -s -X POST "$BASE/web/attachment/upload" -H "$COOK" -F "file=@$TMP/evil.exe" -F "name=evil.exe")
echo "$E" | grep -qi 'not allowed' && ok "a disallowed extension is refused" || no "an .exe was accepted"
NOAUTH=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/web/attachment/upload" -F "file=@$TMP/other.txt")
[ "$NOAUTH" = "401" ] && ok "an unauthenticated upload is refused" || no "unauthenticated upload returned $NOAUTH"

echo "############ housekeeping ############"
pg "DELETE FROM ir_attachment WHERE res_model='hr.expense.sheet' AND res_id IN ($SH,$SH2)" >/dev/null
pg "DELETE FROM hr_expense_sheet WHERE id IN ($SH,$SH2)" >/dev/null
rm -rf "$TMP"
ok "fixtures cleaned up"

echo
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
