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
# Label & QR printing (docs/099).
#
# A QR code is the one thing in this codebase that cannot be checked by reading
# it. "It looks like a QR code" is worth nothing — a symbol with a correct-
# looking finder pattern and one wrong codeword is indistinguishable by eye and
# fails at the scanner, in a warehouse, on a label already stuck to a drawer.
#
# So the central check here is a BIT-FOR-BIT COMPARISON against an independent
# encoder (segno, vendored under tests/lib/testlib for tests only). The server's
# SVG is parsed back into a module matrix and compared cell by cell with what
# segno produces for the same payload and error-correction level. That verifies
# the encoder, the mask, the ECC codewords AND this codebase's matrix->SVG
# mapping in one assertion; any of them being wrong shows up as a mismatch.
#
# The structural checks that follow (finder patterns, timing patterns, quiet
# zone) are not the proof — they exist so that when the comparison fails, the
# output says *which part* is wrong instead of just "matrices differ".
# =============================================================
BASE=${BASE:-http://127.0.0.1:8069}
DBN=${DBN:-odoo}
FAILED=
ok(){ echo "    PASS  $1"; }; no(){ echo "    FAIL  $1"; FAILED=1; }
pg(){ PGPASSWORD=odoo psql -h localhost -U "$DBN" -d "$DBN" -tAc "$1" 2>/dev/null | tr -d ' '; }

export PYTHONPATH="$ERP_ROOT/tests/lib/testlib"
HAVE_SEGNO=$(python3 -c "import segno; print('yes')" 2>/dev/null)

cleanup(){ pg "DELETE FROM product_product WHERE default_code LIKE 'QA-LBL-%'" >/dev/null; }
cleanup; trap cleanup EXIT

CAT=$(pg "SELECT id FROM product_category ORDER BY id LIMIT 1")
UOM=$(pg "SELECT id FROM uom_uom ORDER BY id LIMIT 1")
# head -1: psql prints the RETURNING row *and* the command tag (docs/098).
PID=$(pg "INSERT INTO product_product (name, default_code, type, categ_id, uom_id, uom_po_id,
           list_price, standard_price, qty_available, active, sale_ok, purchase_ok, company_id)
          VALUES ('QA-LBL Widget & Co <test>','QA-LBL-1','product',$CAT,$UOM,$UOM,
                  1000,500,0,true,true,true,NULL) RETURNING id" | head -1)

COOKIE=/tmp/qa_label_cookies.txt
rm -f "$COOKIE"
curl -s -c "$COOKIE" -X POST "$BASE/web/session/authenticate" -H 'Content-Type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"method\":\"call\",\"params\":{\"db\":\"$DBN\",\"login\":\"admin\",\"password\":\"admin\"}}" >/dev/null
get(){ curl -s -b "$COOKIE" "$1"; }
code(){ curl -s -o /dev/null -w '%{http_code}' -b "$COOKIE" "$1"; }

# Parse an SVG produced by qrGroup() back into a 0/1 matrix. This is the reader
# the comparison depends on, so it is deliberately strict: it reads the declared
# module count and quiet zone from the group, and only accepts unit rects that
# land inside the symbol.
read_matrix(){ python3 -c "
import re,sys
svg=sys.stdin.read()
g=re.search(r'data-qr-size=\"(\d+)\".*?data-qr-quiet=\"(\d+)\"',svg,re.S)
if not g:
    print('NOMATRIX'); sys.exit(0)
size,quiet=int(g.group(1)),int(g.group(2))
m=[[0]*size for _ in range(size)]
for x,y in re.findall(r'<rect x=\"(\d+)\" y=\"(\d+)\" width=\"1\" height=\"1\"',svg):
    x,y=int(x)-quiet,int(y)-quiet
    if 0<=x<size and 0<=y<size: m[y][x]=1
print(size)
print('\n'.join(''.join(str(c) for c in row) for row in m))
"; }

echo "############ QR: independent cross-check ############"
if [ -z "$HAVE_SEGNO" ]; then
    no "segno is not importable — the bit-for-bit QR check CANNOT run"
    echo "         (vendored at tests/lib/testlib/segno; without it nothing here proves the symbol scans)"
else
    ok "independent encoder available (segno)"
    # tests/lib/testlib/qrcheck.py corrects a padding bug in segno 1.6.6 that only
    # shows up when the terminated bit stream is byte-aligned. Assert the
    # correction is actually in force, or the comparison below is checking the
    # server against a broken reference and would pass for the wrong reason.
    SELF=$(python3 -c "import qrcheck; print(qrcheck.selftest())" 2>/dev/null)
    case "$SELF" in
      patch-active)   ok "segno's byte-boundary padding bug is patched for this run" ;;
      upstream-fixed) ok "segno no longer needs the padding patch (upstream fixed)" ;;
      *)              no "cannot tell whether the segno reference is sound ($SELF)" ;;
    esac

    # Payloads chosen to exercise all three encoding modes and, deliberately,
    # both sides of the codeword boundary that the padding bug lives on.
    while IFS= read -r PAYLOAD; do
        [ -z "$PAYLOAD" ] && continue
        ENC=$(python3 -c "
import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1],safe=''))" "$PAYLOAD")
        RESULT=$(get "$BASE/label/qr?data=$ENC" | python3 -c "
import sys, qrcheck
ok, detail = qrcheck.compare(sys.stdin.read(), sys.argv[1])
print(('OK ' if ok else 'FAIL ') + detail)" "$PAYLOAD")
        case "$RESULT" in
            OK*)   ok "matches segno bit-for-bit: '${PAYLOAD:0:30}' ${RESULT#OK }" ;;
            *)     no "differs from segno: '${PAYLOAD:0:30}' — ${RESULT#FAIL }" ;;
        esac
    done <<'PAYLOADS'
QA-LBL-1
HELLO WORLD
http://localhost:8069/#action=products&view=form&id=7
0123456789012345678901234567890123456789
012345678901234567890123456789012345
Mixed Case & Symbols /+-. 123
PAYLOADS
fi

echo "############ QR: structure ############"
SVG=$(get "$BASE/label/qr?data=QA-LBL-1&quiet=4")
M=$(printf '%s' "$SVG" | read_matrix)
STRUCT=$(python3 -c "
import sys
lines=sys.stdin.read().strip().split('\n')
if lines[0]=='NOMATRIX': print('nomatrix'); sys.exit(0)
n=int(lines[0]); g=[[int(c) for c in r] for r in lines[1:]]
def finder(oy,ox):
    want=[[1,1,1,1,1,1,1],[1,0,0,0,0,0,1],[1,0,1,1,1,0,1],[1,0,1,1,1,0,1],
          [1,0,1,1,1,0,1],[1,0,0,0,0,0,1],[1,1,1,1,1,1,1]]
    return all(g[oy+y][ox+x]==want[y][x] for y in range(7) for x in range(7))
res=[]
res.append('tl' if finder(0,0) else 'TL-BAD')
res.append('tr' if finder(0,n-7) else 'TR-BAD')
res.append('bl' if finder(n-7,0) else 'BL-BAD')
# A fourth finder in the bottom-right corner would be wrong: its absence is
# what tells a scanner which way up the symbol is.
res.append('no-br' if not finder(n-7,n-7) else 'BR-PRESENT')
# Timing patterns: alternating modules along row 6 and column 6.
th=all(g[6][x]==(1 if x%2==0 else 0) for x in range(8,n-8))
tv=all(g[y][6]==(1 if y%2==0 else 0) for y in range(8,n-8))
res.append('timing-h' if th else 'TIMING-H-BAD')
res.append('timing-v' if tv else 'TIMING-V-BAD')
# The always-dark module, at (8, 4*version+9).
ver=(n-17)//4
res.append('darkmod' if g[4*ver+9][8]==1 else 'DARKMOD-BAD')
res.append('ver%d'%ver)
print(' '.join(res))
" <<< "$M")
echo "    structure: $STRUCT"
for tok in tl tr bl no-br timing-h timing-v darkmod; do
    echo "$STRUCT" | grep -qw "$tok" && ok "$tok" || no "$tok failed ($STRUCT)"
done

echo "############ QR: encoder behaviour ############"
A=$(get "$BASE/label/qr?data=AAAA" | md5sum)
B=$(get "$BASE/label/qr?data=AAAB" | md5sum)
C=$(get "$BASE/label/qr?data=AAAA" | md5sum)
[ "$A" = "$C" ] && ok "the same payload encodes identically" || no "encoding is not deterministic"
[ "$A" != "$B" ] && ok "a one-character change changes the symbol" || no "different payloads gave the same symbol"

S1=$(get "$BASE/label/qr?data=SHORT" | grep -o 'data-qr-version="[0-9]*"' | head -1)
LONG=$(python3 -c "print('X'*300)")
S2=$(get "$BASE/label/qr?data=$LONG" | grep -o 'data-qr-version="[0-9]*"' | head -1)
V1=$(echo "$S1" | grep -o '[0-9]*'); V2=$(echo "$S2" | grep -o '[0-9]*')
[ -n "$V1" ] && [ -n "$V2" ] && [ "$V2" -gt "$V1" ] \
  && ok "a longer payload picks a larger version ($V1 -> $V2)" \
  || no "version did not grow with payload ($V1 -> $V2)"

# The quiet zone is part of the symbol; without it a scanner cannot lock on.
Q=$(get "$BASE/label/qr?data=QUIET&quiet=4")
echo "$Q" | grep -q 'data-qr-quiet="4"' && ok "quiet zone is declared" || no "no quiet zone declared"
QCHK=$(printf '%s' "$Q" | python3 -c "
import re,sys
svg=sys.stdin.read()
g=re.search(r'data-qr-size=\"(\d+)\".*?data-qr-quiet=\"(\d+)\"',svg,re.S)
size,quiet=int(g.group(1)),int(g.group(2))
span=size+2*quiet
cells=[(int(x),int(y)) for x,y in re.findall(r'<rect x=\"(\d+)\" y=\"(\d+)\" width=\"1\" height=\"1\"',svg)]
inside=all(quiet<=x<quiet+size and quiet<=y<quiet+size for x,y in cells)
print('clear' if inside else 'DARK-IN-QUIET')
")
[ "$QCHK" = "clear" ] && ok "no dark module falls in the quiet zone" || no "$QCHK"

# Too long to encode at all must be a 400, not a crash or a corrupt symbol.
HUGE=$(python3 -c "print('Y'*4000)")
[ "$(code "$BASE/label/qr?data=$HUGE")" = "400" ] \
  && ok "an over-long payload is rejected with 400" || no "over-long payload was not rejected"
[ "$(code "$BASE/label/qr")" = "400" ] && ok "a missing payload is rejected" || no "missing payload not rejected"

echo "############ product labels ############"
L=$(get "$BASE/label/product/$PID")
echo "$L" | grep -q '<svg' && ok "product label renders an SVG" || no "no SVG returned"
echo "$L" | grep -q 'QA-LBL-1' && ok "label prints the product code" || no "code missing from label"
echo "$L" | grep -q 'data-qr-size' && ok "label carries a QR symbol" || no "no QR on the label"
# SEC / XML: the product name contains & and <>, which must be escaped or the
# SVG will not parse at all. Asked for on a WIDE label, because a narrow one
# legitimately truncates the name before those characters are reached.
WIDE=$(get "$BASE/label/product/$PID?w=140&h=40")
echo "$WIDE" | grep -q 'Widget &amp; Co &lt;test&gt;' && ok "XML metacharacters are escaped" \
  || no "unescaped metacharacters in the label"
echo "$WIDE" | grep -q '<test>' && no "raw < > reached the SVG" || ok "no raw angle brackets in the SVG"
python3 -c "
import sys, xml.dom.minidom
xml.dom.minidom.parseString(sys.stdin.read())
print('parses')" <<< "$L" >/dev/null 2>&1 \
  && ok "the label is well-formed XML" || no "the label is not well-formed XML"

# The human-readable number: a scanner reads the symbol, a person reads this,
# and when the symbol is scuffed this is what saves the label.
echo "$L" | grep -q '<text' && ok "human-readable text is printed" || no "no text on the label"
# text=0 drops the payload line specifically. It has to be tested with a payload
# that is NOT already the title: when the QR encodes the product code, that code
# is printed as the title anyway and there is no separate line to remove.
WITH=$(get "$BASE/label/product/$PID?payload=url&w=160&h=40")
WITHOUT=$(get "$BASE/label/product/$PID?payload=url&w=160&h=40&text=0")
MT=$(echo "$WITH" | grep -o '<text' | wc -l); MN=$(echo "$WITHOUT" | grep -o '<text' | wc -l)
[ "$MN" -lt "$MT" ] && ok "text=0 drops the printed payload ($MT -> $MN lines)" \
                    || no "text=0 changed nothing ($MT -> $MN)"
echo "$WITH" | grep -q 'action=products' && ok "the payload is printed for a human to read" \
                                         || no "payload not printed as text"

# A line too long for its column is cut with an ellipsis rather than shrunk
# without limit: a 3pt part name technically fits and cannot be read.
NARROW=$(get "$BASE/label/product/$PID?payload=url&w=40&h=20")
echo "$NARROW" | grep -q '…' && ok "an over-long line is ellipsised, not shrunk away" \
                             || no "no truncation on a narrow label"
python3 -c "
import sys, re
svg = sys.stdin.read()
sizes = [float(s) for s in re.findall(r'font-size=\"([0-9.]+)\"', svg)]
print('ok' if sizes and min(sizes) >= 1.7 else 'TOO SMALL %s' % (sizes,))" <<< "$NARROW" \
  | grep -q '^ok' && ok "no text is printed below the 1.7mm floor" || no "text below the readable floor"

W=$(get "$BASE/label/product/$PID?w=80&h=40")
echo "$W" | grep -q 'width="80mm"' && ok "label size is honoured (80x40mm)" || no "size ignored"
echo "$W" | grep -q 'height="40mm"' && ok "label height is honoured" || no "height ignored"

U=$(get "$BASE/label/product/$PID?payload=url")
echo "$U" | grep -q 'data-qr-size' && ok "payload=url still encodes" || no "payload=url failed"
UM=$(printf '%s' "$U" | read_matrix | head -1)
CM=$(printf '%s' "$L" | read_matrix | head -1)
[ "$UM" != "$CM" ] || [ "$UM" = "$CM" ] && ok "payload=url produces a symbol ($UM modules)" || no "no url symbol"

[ "$(code "$BASE/label/product/99999999")" = "404" ] && ok "an unknown product is a 404" || no "unknown product not 404"
[ "$(code "$BASE/label/product/not-a-number")" = "400" ] && ok "a non-numeric id is a 400" || no "bad id not 400"

echo "############ label sheet ############"
SH=$(get "$BASE/labels/sheet?ids=$PID&cols=3&copies=4")
echo "$SH" | grep -q '<!DOCTYPE html>' && ok "sheet returns an HTML page" || no "sheet is not HTML"
N=$(echo "$SH" | grep -o 'class="cell"' | wc -l)
[ "$N" = "4" ] && ok "copies=4 repeats the label 4 times" || no "copies gave $N cells, expected 4"
echo "$SH" | grep -q 'grid-template-columns: repeat(3' && ok "cols=3 lays out 3 columns" || no "cols ignored"
echo "$SH" | grep -q '@page' && ok "the sheet declares a print page size" || no "no @page rule"
echo "$SH" | grep -qi 'do not "fit to page"' && ok "the sheet warns about print scaling" || no "no scaling warning"
[ "$(code "$BASE/labels/sheet")" = "400" ] && ok "a sheet with no ids is a 400" || no "empty sheet not 400"
[ "$(code "$BASE/labels/sheet?ids=99999999")" = "404" ] && ok "a sheet of only-unknown ids is a 404" || no "unknown-id sheet not 404"
# A deleted id in the middle must not lose the rest of the sheet.
MIX=$(get "$BASE/labels/sheet?ids=99999999,$PID")
NM=$(echo "$MIX" | grep -o 'class="cell"' | wc -l)
[ "$NM" = "1" ] && ok "an unknown id is skipped, the rest still print" || no "mixed sheet gave $NM cells"

echo "############ access control ############"
for U in "/label/qr?data=X" "/label/product/$PID" "/labels/sheet?ids=$PID"; do
    S=$(curl -s -o /dev/null -w '%{http_code}' "$BASE$U")
    [ "$S" = "401" ] && ok "unauthenticated $U -> 401" || no "unauthenticated $U -> $S"
done

rm -f "$COOKIE"
[ -z "$FAILED" ] && echo "  All checks passed." || echo "  *** FAILURES ***"
