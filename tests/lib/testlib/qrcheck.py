"""
qrcheck.py — an independent reference for the QR symbols this server prints.

A QR code is the one output in this codebase that cannot be checked by reading
it: a symbol with correct-looking finder patterns and one wrong codeword is
indistinguishable by eye and fails at the scanner, on a label already stuck to a
drawer. So the labels are verified by re-encoding the same payload with a second,
unrelated implementation (segno) and comparing module by module.

------------------------------------------------------------------------------
An upstream bug this cross-check found, and why the patch below exists
------------------------------------------------------------------------------
segno 1.6.6's encoder.write_padding_bits is:

    buff.extend([0] * (8 - (length % 8)))

which appends a whole spurious zero *byte* when the data stream already ends on
a codeword boundary. ISO/IEC 18004 §7.4.10 — quoted verbatim in segno's own
docstring — requires padding only "if the bit stream length is such that it does
not end at a codeword boundary". qrcodegen computes `(8 - size % 8) % 8` and is
correct.

It only bites when the terminated stream is an exact multiple of 8 bits, which
is why 8 of 10 sweep payloads agreed and two did not. Rather than exclude those
payloads — which would drop the check exactly where encoders disagree — the
reference is corrected here, at test time, in memory. The vendored source is
left untouched so the discrepancy stays visible.
"""
import re
import segno
import segno.encoder as _enc

_ORIGINAL_PADDING = _enc.write_padding_bits


def _spec_padding_bits(buff, version, length):
    """ISO/IEC 18004 §7.4.10 — pad to the codeword boundary, and no further."""
    if version not in (_enc.consts.VERSION_M1, _enc.consts.VERSION_M3):
        buff.extend([0] * ((8 - (length % 8)) % 8))


_enc.write_padding_bits = _spec_padding_bits


def parse_svg(svg):
    """Read a server-rendered QR back into a module matrix.

    Depends on renderLabelSvg emitting unit rects on an integer module grid,
    which is the property that makes the output machine-checkable at all.
    Returns (size, matrix) or (0, None) if the markup carries no symbol.
    """
    g = re.search(r'data-qr-size="(\d+)".*?data-qr-quiet="(\d+)"', svg, re.S)
    if not g:
        return 0, None
    size, quiet = int(g.group(1)), int(g.group(2))
    m = [[0] * size for _ in range(size)]
    for x, y in re.findall(r'<rect x="(\d+)" y="(\d+)" width="1" height="1"', svg):
        x, y = int(x) - quiet, int(y) - quiet
        if 0 <= x < size and 0 <= y < size:
            m[y][x] = 1
    return size, m


def read_format(m):
    """Decode the 15-bit format information around the top-left finder.

    Returns (ecc_letter, mask). This is how the comparison learns what the
    symbol *claims* to be — qrcodegen boosts the error-correction level to the
    highest that still fits the chosen version, so the level on the wire is not
    the level that was requested.
    """
    bits = [(i, 8) for i in range(6)] + [(7, 8), (8, 8), (8, 7)] \
         + [(8, 14 - i) for i in range(9, 15)]
    val = 0
    for i, (r, c) in enumerate(bits):
        if m[r][c]:
            val |= (1 << i)
    val ^= 0x5412
    ecc = {0b01: 'L', 0b00: 'M', 0b11: 'Q', 0b10: 'H'}[(val >> 13) & 0b11]
    return ecc, (val >> 10) & 0b111


def compare(svg, payload):
    """Compare a rendered symbol against segno. Returns (ok, detail)."""
    size, mine = parse_svg(svg)
    if not size:
        return False, "no QR matrix in the SVG"
    try:
        ecc, mask = read_format(mine)
    except (KeyError, IndexError) as e:
        return False, "unreadable format information (%s)" % e
    ref_code = segno.make(payload, error=ecc.lower(), micro=False,
                          boost_error=False, mask=mask)
    ref = [[1 if c else 0 for c in row] for row in ref_code.matrix]
    if len(ref) != size:
        return False, "size %d, segno %d (ecc=%s mask=%d)" % (size, len(ref), ecc, mask)
    bad = sum(1 for y in range(size) for x in range(size) if mine[y][x] != ref[y][x])
    if bad:
        return False, "%d/%d modules differ (ecc=%s mask=%d v%d)" % (
            bad, size * size, ecc, mask, ref_code.version)
    return True, "%dx%d ecc=%s mask=%d v%d" % (size, size, ecc, mask, ref_code.version)


def selftest():
    """Prove the padding correction is actually in force.

    Without it, a payload whose terminated bit stream is byte-aligned encodes
    differently in segno than in any correct implementation. If this ever starts
    passing unpatched, segno has been fixed and the patch can go.
    """
    payload = ''.join(str(i % 10) for i in range(40))   # 152 bits: byte-aligned
    patched = segno.make(payload, error='q', micro=False, boost_error=False, mask=6)
    _enc.write_padding_bits = _ORIGINAL_PADDING
    try:
        unpatched = segno.make(payload, error='q', micro=False, boost_error=False, mask=6)
    finally:
        _enc.write_padding_bits = _spec_padding_bits
    same = patched.matrix == unpatched.matrix
    return ("upstream-fixed" if same else "patch-active")
