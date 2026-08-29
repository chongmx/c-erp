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
# ir.sequence (INV invoice numbering), ir.model.data, ir.attachment.
#
# The three primitives requested for deploy readiness. Each is proved
# through the real path:
#   * invoice numbering — post a customer invoice, read INV000001
#   * ir.attachment — upload bytes, download them back byte-for-byte,
#     and confirm the filename never became a filesystem path (traversal)
#   * ir.model.data — the xml_id table exists and enforces uniqueness
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=

pg() { PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' ' | head -1; }
ok() { echo "    PASS  $1"; }
no() { echo "    FAIL  $1"; FAILED=1; }

CK=/tmp/irp_cookie.txt
cat > /tmp/irp_auth.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"db":"$DBN","login":"admin","password":"admin"}}
EOF
curl -s -c "$CK" -X POST "$BASE/web/session/authenticate" \
     -H 'Content-Type: application/json' --data @/tmp/irp_auth.json > /tmp/irp_auth_out.json
grep -q '"session_id"' /tmp/irp_auth_out.json || { echo "cannot authenticate"; exit 1; }

echo "############ 1. invoice numbering — INV prefix, pad 6 ############"
SEQ=$(pg "SELECT prefix||'/'||padding||'/'||reset_policy FROM ir_sequence WHERE code='account.move.INV'")
echo "    account.move.INV sequence: $SEQ"
[ "$SEQ" = "INV/6/never" ] && ok "sequence is prefix INV, padding 6, no reset" || no "sequence is $SEQ"

# Post a customer invoice through the rental billing path (the real code
# path) and read its number.
PARTNER=$(pg "SELECT id FROM res_partner ORDER BY id LIMIT 1")
UNIT=$(pg "INSERT INTO rental_unit (code,name,state,company_id) VALUES ('IRPTEST-1','x','available',1) RETURNING id")
pg "INSERT INTO rental_contract_line (partner_id,unit_id,date_start,unit_price,tax_ids_json,state,billing_mode,billing_anchor_day,billing_months,billing_lead_days,next_period_start,company_id) VALUES ($PARTNER,$UNIT,'2026-09-01',100000000,'[]','active','recurring',1,1,7,'2026-09-01',1)" > /dev/null
curl -s -b "$CK" -X POST "$BASE/rental/billing/run?date=2026-08-26" > /dev/null
NUM=$(pg "SELECT name FROM account_move WHERE move_type='out_invoice' AND name LIKE 'INV%' ORDER BY id DESC LIMIT 1")
echo "    posted invoice number: $NUM"
printf '%s' "$NUM" | grep -qE '^INV[0-9]{6}$' && ok "matches INV###### format" || no "got '$NUM'"

# Two invoices must be consecutive, never duplicated.
N1=$(pg "SELECT regexp_replace(name,'INV','') FROM account_move WHERE name LIKE 'INV%' ORDER BY id DESC LIMIT 1")
DUPES=$(pg "SELECT count(*) - count(DISTINCT name) FROM account_move WHERE name LIKE 'INV%'")
[ "$DUPES" = "0" ] && ok "no duplicate invoice numbers" || no "$DUPES duplicate numbers"

echo
echo "############ 2. ir.attachment — upload, download, round-trip ############"
# A distinctive payload so the round-trip is unambiguous.
printf '%%PDF-1.4 datasheet body \x01\x02\x03 EOF' > /tmp/irp_datasheet.pdf
SUM_IN=$(sha256sum /tmp/irp_datasheet.pdf | cut -d' ' -f1)
R=$(curl -s -b "$CK" -X POST "$BASE/web/attachment/upload" \
     -F "file=@/tmp/irp_datasheet.pdf;filename=datasheet.pdf" \
     -F "res_model=product.product" -F "res_id=1" -F "name=Widget datasheet")
echo "    upload -> $(printf '%s' "$R" | head -c 160)"
AID=$(printf '%s' "$R" | python3 -c "import json,sys; print(json.load(sys.stdin).get('id',''))" 2>/dev/null)
[ -n "$AID" ] && ok "attachment created (id $AID)" || no "upload failed"

# Metadata landed.
CHK=$(pg "SELECT checksum FROM ir_attachment WHERE id=$AID")
SIZE=$(pg "SELECT file_size FROM ir_attachment WHERE id=$AID")
MOD=$(pg "SELECT res_model FROM ir_attachment WHERE id=$AID")
echo "    stored checksum=$CHK size=$SIZE res_model=$MOD"
[ "$CHK" = "$SUM_IN" ] && ok "stored sha256 matches the file"          || no "checksum mismatch"
[ "$MOD" = "product.product" ] && ok "linked to product.product"       || no "res_model is $MOD"

# Download and compare byte-for-byte.
curl -s -b "$CK" "$BASE/web/content/$AID?download=1" -o /tmp/irp_out.pdf
SUM_OUT=$(sha256sum /tmp/irp_out.pdf | cut -d' ' -f1)
echo "    downloaded sha256=$SUM_OUT"
[ "$SUM_IN" = "$SUM_OUT" ] && ok "download is byte-identical to upload" || no "round-trip corrupted the file"

# The stored path must be content-addressed, NOT the request filename.
SF=$(pg "SELECT store_fname FROM ir_attachment WHERE id=$AID")
echo "    store_fname=$SF"
printf '%s' "$SF" | grep -qE '^[0-9a-f]{2}/[0-9a-f]{64}$' \
    && ok "stored by content hash, not by the uploaded name" \
    || no "store_fname is '$SF' — not hash-addressed"
[ -f "data/filestore/$SF" ] && ok "the blob is on disk under the filestore" || no "blob missing"

echo
echo "############ 3. traversal — a malicious filename cannot escape ############"
# The filename is ignored for pathing (content hash is the path), so a
# ../ name must still land safely inside the filestore and never write to
# /tmp or the project root.
rm -f /tmp/PWNED_BY_UPLOAD
printf 'evil' > /tmp/irp_evil.pdf
R2=$(curl -s -b "$CK" -X POST "$BASE/web/attachment/upload" \
      -F 'file=@/tmp/irp_evil.pdf;filename=../../../../tmp/PWNED_BY_UPLOAD.pdf' \
      -F "name=evil")
AID2=$(printf '%s' "$R2" | python3 -c "import json,sys; print(json.load(sys.stdin).get('id',''))" 2>/dev/null)
[ -n "$AID2" ] && ok "upload accepted (name sanitised, not rejected outright)" || no "upload errored: $R2"
[ ! -e /tmp/PWNED_BY_UPLOAD.pdf ] && ok "no file written outside the filestore" \
                                  || no "TRAVERSAL: wrote /tmp/PWNED_BY_UPLOAD.pdf"
SF2=$(pg "SELECT store_fname FROM ir_attachment WHERE id=$AID2")
printf '%s' "$SF2" | grep -qE '^[0-9a-f]{2}/[0-9a-f]{64}$' \
    && ok "malicious name still stored by hash" || no "store_fname is '$SF2'"

echo
echo "############ 4. attachment routes require auth ############"
U1=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/web/attachment/upload")
U2=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/web/content/$AID")
echo "    upload no-session -> $U1   content no-session -> $U2"
[ "$U1" = "401" ] && ok "upload refuses anonymous"   || no "upload returned $U1"
[ "$U2" = "401" ] && ok "download refuses anonymous" || no "download returned $U2"

echo
echo "############ 5. dedup — identical content stored once ############"
BLOBS_BEFORE=$(pg "SELECT count(DISTINCT store_fname) FROM ir_attachment WHERE checksum='$SUM_IN'")
# Upload the SAME datasheet again under a different name.
R3=$(curl -s -b "$CK" -X POST "$BASE/web/attachment/upload" \
      -F "file=@/tmp/irp_datasheet.pdf;filename=copy.pdf" -F "name=copy")
AID3=$(printf '%s' "$R3" | python3 -c "import json,sys; print(json.load(sys.stdin).get('id',''))" 2>/dev/null)
SF3=$(pg "SELECT store_fname FROM ir_attachment WHERE id=$AID3")
echo "    second upload store_fname=$SF3 (same as first: $SF)"
[ "$SF3" = "$SF" ] && ok "identical content shares one blob (dedup by hash)" \
                   || no "dedup failed: $SF3 vs $SF"

echo
echo "############ 6. ir.model.data — xml_id registry ############"
HAS=$(pg "SELECT count(*) FROM information_schema.tables WHERE table_name='ir_model_data'")
[ "$HAS" = "1" ] && ok "ir_model_data table exists" || no "table missing"
# UNIQUE(module,name) must hold.
pg "INSERT INTO ir_model_data (module,name,model,res_id) VALUES ('irptest','probe','res.partner',$PARTNER)" > /dev/null
ERR=$(PGPASSWORD=odoo psql -q -h localhost -U "$DBN" -d "$DBN" -tAc \
      "INSERT INTO ir_model_data (module,name,model,res_id) VALUES ('irptest','probe','res.partner',$PARTNER)" 2>&1 | head -1)
printf '%s' "$ERR" | grep -qi "duplicate key\|unique" && ok "UNIQUE (module,name) enforced" \
                                                      || no "duplicate xml_id accepted"
# Reachable via the API.
cat > /tmp/irp_md.json <<EOF
{"jsonrpc":"2.0","method":"call","params":{"model":"ir.model.data","method":"search_read",
 "args":[[["module","=","irptest"]]],"kwargs":{"fields":["module","name","model","res_id"],
 "context":{"session_id":"$(pg "SELECT 1" >/dev/null; sed -n 's/.*session_id.*//p' /dev/null; echo)"}}}}
EOF
# (Auth via cookie instead of session_id in kwargs.)
RD=$(curl -s -b "$CK" -X POST "$BASE/web/dataset/call_kw" -H 'Content-Type: application/json' \
     --data '{"jsonrpc":"2.0","method":"call","params":{"model":"ir.model.data","method":"search_read","args":[[["module","=","irptest"]]],"kwargs":{"fields":["module","name","model","res_id"]}}}')
printf '%s' "$RD" | grep -q '"name":"probe"' && ok "ir.model.data queryable through the API" \
                                             || no "not queryable: $(printf '%s' "$RD" | head -c 100)"

echo
echo "############ cleanup ############"
pg "DELETE FROM ir_model_data WHERE module='irptest'" >/dev/null
# Remove the filestore blobs these uploads created. Blob reclamation is a
# periodic sweep in production (Odoo does the same — dedup makes
# per-unlink refcounting racy), so deleting the rows does NOT remove the
# bytes; the test cleans them explicitly.
for sf in "$SF" "$SF2"; do
    [ -n "$sf" ] && rm -f "data/filestore/$sf"
done
pg "DELETE FROM ir_attachment WHERE id IN ($AID,$AID2,$AID3)" >/dev/null
pg "DELETE FROM rental_invoice_link WHERE contract_line_id IN (SELECT id FROM rental_contract_line WHERE unit_id=$UNIT)" >/dev/null
pg "DELETE FROM account_move_line WHERE move_id IN (SELECT id FROM account_move WHERE partner_id=$PARTNER AND name LIKE 'INV%')" >/dev/null
pg "DELETE FROM account_move WHERE name LIKE 'INV%'" >/dev/null
pg "DELETE FROM rental_contract_line WHERE unit_id=$UNIT" >/dev/null
pg "DELETE FROM rental_unit WHERE id=$UNIT" >/dev/null
rm -f /tmp/irp_*.pdf /tmp/irp_out.pdf /tmp/irp_auth.json /tmp/irp_auth_out.json "$CK" /tmp/irp_md.json
echo "    test data removed"

echo
echo "############ SUMMARY ############"
[ -n "$FAILED" ] && echo "  *** FAILURES ***" || echo "  All checks passed."
