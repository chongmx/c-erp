#!/bin/bash
# --- harness ---------------------------------------------------------------
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1
source tests/lib/harness.sh
# ---------------------------------------------------------------------------
# =============================================================
# The website media library (docs/124).
#
# This is the only route in the module that hands ATTACKER-SUPPLIED BYTES to an
# anonymous visitor, so most of this file is about what must not come back out.
#
#   §3  the upload gate — the bytes decide the type, not the header, not the
#       extension, not the filename. SVG is refused outright.
#   §5  the serve gate — a public route that must never serve an attachment
#       belonging to the rest of the ERP, and never serve anything a browser
#       would execute.
#   §6  permission, same as every other editing route.
#
# The unit tier (tests/unit/website/test_media.cpp) owns the signature
# catalogue. This tier owns the wiring: upload → database → public URL.
# =============================================================
auth_or_die
ADMIN_SID="$SID"

TMP=""
cleanup() {
    pg "DELETE FROM ir_attachment WHERE res_model='website'" >/dev/null
    pg "DELETE FROM res_groups_users_rel WHERE uid IN
          (SELECT id FROM res_users WHERE login LIKE 'md_%')" >/dev/null
    pg "DELETE FROM res_users   WHERE login LIKE 'md_%'" >/dev/null
    pg "DELETE FROM res_partner WHERE name LIKE 'MD %'" >/dev/null
    [ -n "$TMP" ] && rm -rf "$TMP"
}
cleanup
# AFTER the opening cleanup: that call removes $TMP, so creating it earlier
# would delete the directory the fixtures are about to be written into.
TMP=$(mktemp -d)
trap 'cleanup' EXIT

up() {   # up <sid> <file> [name] -> body
    curl -s -X POST -H "Cookie: session_id=${1:-}" \
         --data-binary "@$2" "$BASE/site/api/media?name=${3:-test.png}"
}
upcode() {
    curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: session_id=${1:-}" \
         --data-binary "@$2" "$BASE/site/api/media?name=${3:-test.png}"
}

# --- fixtures: real files with real signatures ---
python3 - "$TMP" <<'PY'
import sys, os, zlib, struct
d = sys.argv[1]
def w(n, b): open(os.path.join(d, n), 'wb').write(b)
# A genuine 1x1 PNG, not a stub — the serve route re-sniffs what it reads back.
def png():
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    raw = b'\x00\xff\x00\x00'
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw))
            + chunk(b'IEND', b''))
w('ok.png', png())
w('ok.gif', b'GIF89a' + b'\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00!'
            b'\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00'
            b'\x02\x02D\x01\x00;')
w('evil.svg', b'<svg xmlns="http://www.w3.org/2000/svg" onload="alert(1)">'
              b'<script>alert(document.cookie)</script></svg>')
w('evil.html', b'<!DOCTYPE html><script>alert(1)</script>')
w('wav.webp', b'RIFF' + b'\x20\x20\x20\x20' + b'WAVE' + b'\x00' * 32)
w('big.png',   png() + b'\x00' * (9 * 1024 * 1024))
w('photo.png', png() + b'\x00' * (3 * 1024 * 1024))
PY

# ------------------------------------------------------------------
sec "1. an image can be uploaded and comes back with a public URL"
# ------------------------------------------------------------------
RESP=$(up "$ADMIN_SID" "$TMP/ok.png" "unit-50.png")
t_contains "$RESP" '"ok":true'       "a PNG is accepted"
t_contains "$RESP" '"image/png"'     "and typed from its bytes"
t_contains "$RESP" '/site/media/'    "and given a public URL"
ID=$(printf '%s' "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("id",""))')
t_nonempty "$ID" "with an id"
[ -z "$ID" ] && { verdict; exit 1; }
URL="/site/media/$ID"

t_eq "1" "$(pg "SELECT count(*) FROM ir_attachment WHERE id=$ID AND public=TRUE AND res_model='website'")" \
     "stored as a public website attachment"

# ------------------------------------------------------------------
sec "2. a VISITOR can fetch it — the whole point"
# ------------------------------------------------------------------
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$URL")" \
     "no session needed"
HDR=$(curl -s -D - -o /dev/null "$BASE$URL")
t_contains "$HDR" 'image/png'              "served as an image"
t_contains "$HDR" 'nosniff'                "with nosniff, so a browser cannot re-type it"
t_contains "$HDR" 'inline'                 "inline rather than as a download"
# S-39: the name in the header is derived, not echoed.
t_lacks "$HDR" '..'                        "no traversal in the disposition"

# ------------------------------------------------------------------
sec "3. THE UPLOAD GATE — the bytes decide, nothing else"
# ------------------------------------------------------------------
# An SVG is XML that can carry script. Served from our origin it would run
# with our origin's privileges: stored XSS with an <img> tag on the front.
t_eq "400" "$(upcode "$ADMIN_SID" "$TMP/evil.svg" "logo.svg")" "an SVG is refused"
t_contains "$(up "$ADMIN_SID" "$TMP/evil.svg" "logo.svg")" 'SVG' "and the refusal says why"

# Renaming it does not help, because the name was never consulted.
t_eq "400" "$(upcode "$ADMIN_SID" "$TMP/evil.svg" "harmless.png")" \
     "…nor does calling it a .png"
t_eq "400" "$(upcode "$ADMIN_SID" "$TMP/evil.html" "page.png")" \
     "an HTML document called .png is refused"
t_eq "400" "$(upcode "$ADMIN_SID" "$TMP/wav.webp" "sound.webp")" \
     "a RIFF container that is not WebP is refused"
t_eq "400" "$(upcode "$ADMIN_SID" "$TMP/big.png" "huge.png")" \
     "a 9 MB image is over the cap, and it is OUR cap that answers"
# The regression this guards: Drogon's own body limit defaults to about 1 MB,
# below the size of an ordinary phone photo. Every image over that died with a
# bare 413 before the handler ran, so the handler's cap was unreachable and its
# explanation never shown. A 3 MB upload has to work.
t_eq "200" "$(upcode "$ADMIN_SID" "$TMP/photo.png" "photo.png")" \
     "a 3 MB photo — an ordinary one — uploads"
t_eq "0" "$(pg "SELECT count(*) FROM ir_attachment WHERE res_model='website' AND mimetype NOT LIKE 'image/%'")" \
     "nothing that is not an image was stored"
t_eq "0" "$(pg "SELECT count(*) FROM ir_attachment WHERE res_model='website' AND mimetype='image/svg+xml'")" \
     "and no SVG row exists at all"

# The extension recorded is the one the BYTES imply.
G=$(up "$ADMIN_SID" "$TMP/ok.gif" "shell.php")
t_contains "$G" '"image/gif"'  "a GIF uploaded as shell.php is typed as a GIF"
t_contains "$G" 'shell.gif'    "…and stored under a .gif name"

# ------------------------------------------------------------------
sec "4. the library lists what was uploaded"
# ------------------------------------------------------------------
LIST=$(curl -s -H "Cookie: session_id=$ADMIN_SID" "$BASE/site/api/media")
t_contains "$LIST" '"images"'      "the library lists images"
t_contains "$LIST" "$URL"          "including the one just uploaded"
t_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/site/api/media")" \
     "but not to a visitor"

# ------------------------------------------------------------------
sec "5. THE SERVE GATE — it is not a reader for the rest of the ERP"
# ------------------------------------------------------------------
# ir_attachment holds invoices, expense receipts, datasheets. A public route
# over that table with only an id in the URL would be a document leak.
PRIV=$(pg "INSERT INTO ir_attachment (name,res_model,res_id,type,mimetype,file_size,public,company_id)
           VALUES ('payroll.png','hr.payslip',1,'binary','image/png',4,TRUE,1) RETURNING id" | tr -dc '0-9')
t_nonempty "$PRIV" "a PUBLIC attachment belonging to another model exists"
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/site/media/$PRIV")" \
     "…and this route will not serve it, public flag or not"

# A website row whose mimetype was changed to something executable.
BAD=$(pg "INSERT INTO ir_attachment (name,res_model,type,mimetype,file_size,public,company_id)
          VALUES ('x.html','website','binary','text/html',4,TRUE,1) RETURNING id" | tr -dc '0-9')
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/site/media/$BAD")" \
     "a website row claiming text/html is refused on the way out"

# Un-publishing takes it off the internet.
pg "UPDATE ir_attachment SET public=FALSE WHERE id=$ID" >/dev/null
t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$URL")" \
     "clearing the public flag removes it from the public route"
pg "UPDATE ir_attachment SET public=TRUE WHERE id=$ID" >/dev/null
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$URL")" "and restores it"

t_eq "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/site/media/99999999")" \
     "an id that does not exist is a 404, not an error"

# ------------------------------------------------------------------
sec "6. the same permission gate as every other editing route"
# ------------------------------------------------------------------
t_eq "401" "$(upcode "" "$TMP/ok.png")" "a visitor cannot upload"
t_eq "401" "$(upcode "deadbeefdeadbeefdeadbeef" "$TMP/ok.png")" \
     "a forged cookie cannot upload"

PART=$(call res.partner create '[{"name":"MD Plain","email":"md_plain@t.test"}]' | rid)
U=$(call res.users create "[{\"login\":\"md_plain@t.test\",\"password\":\"Plain-Pass-1\",\"partner_id\":$PART,\"active\":true}]" | rid)
PLAIN=$(login 'md_plain@t.test' 'Plain-Pass-1')
t_nonempty "$PLAIN" "an ordinary employee can sign in"
t_eq "0" "$(pg "SELECT count(*) FROM res_groups_users_rel WHERE uid=$U AND gid=4")" \
     "they are NOT in the configuration group"
t_eq "403" "$(upcode "$PLAIN" "$TMP/ok.png")" "and cannot upload to the public site"
t_eq "403" "$(curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=$PLAIN" \
             "$BASE/site/api/media")" "nor list the library"

# They can still SEE a published image, because that is what published means.
t_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' -H "Cookie: session_id=$PLAIN" "$BASE$URL")" \
     "…though a published image is public to them like anyone else"

verdict
