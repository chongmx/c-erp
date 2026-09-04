# 099 — Label and QR printing

Status: **done**. `./scripts/run_tests.sh` → 72 passed, 0 failed
(`verify_labels.sh`, 44 checks).

---

## 1. The library

**Nayuki's QR-Code-generator** (MIT), vendored at `3rdparty/qrcodegen/` — two
files, no dependencies, and the de-facto reference implementation. Listed
explicitly in `CMakeLists.txt` rather than globbed, because `3rdparty/` is not a
source root and must not be swept up wholesale.

I did not write a QR encoder. Reed–Solomon over GF(256), mask penalty scoring and
format-info BCH are a lot of spec to get subtly wrong, and "subtly wrong" here
means a label that looks perfect and does not scan.

## 2. Output is SVG

A label is printed, and print is where resolution matters most. Vector output is
crisp at any DPI, needs no image library in the build, prints straight from the
browser — and, usefully, is inspectable as text.

That last property is load-bearing. The QR is emitted as **unit rects on an
integer module grid**, with the scale carried by the group transform:

```xml
<g class="qr" data-qr-size="25" data-qr-version="2" data-qr-quiet="4"
   transform="translate(1.5,1.5) scale(0.6)" shape-rendering="crispEdges">
  <rect x="4" y="4" width="1" height="1" fill="#000"/>
  …
```

so a test can read the symbol straight back out of the markup. Choosing that
representation is what made the verification below possible at all.

## 3. Routes

| route | purpose |
|---|---|
| `GET /label/product/{id}` | one label as SVG — `w`, `h`, `text=0`, `payload=code\|url` |
| `GET /labels/sheet?ids=…` | a print-ready HTML page — `cols`, `copies`, `gap`, `w`, `h` |
| `GET /label/qr?data=…` | a bare QR for anything that is not a product |

All three are session-gated (401 unauthenticated), gate `ex.what()` behind
devMode (SEC-28), and return 503 on pool exhaustion. `ids` is parsed to integers
before it goes anywhere near SQL.

`payload=code` encodes the barcode/internal reference — what an existing
warehouse scanner expects. `payload=url` encodes a link to the product form, so a
phone camera opens the record. Neither is right for everyone, so it is a
parameter rather than a decision.

**The human-readable number is printed too.** A scanner reads the symbol, a
person reads the text, and when a label has been scuffed in a parts drawer the
text is what saves it. `text=0` turns it off.

## 4. Verifying a thing you cannot read

This was the real problem. "It looks like a QR code" is worth nothing — a symbol
with correct-looking finder patterns and one wrong codeword is indistinguishable
by eye. No scanner, decoder or encoder was available locally (`zbar`, `qrencode`,
`PIL`, and even `pip` are all absent).

So `segno`, an unrelated pure-Python encoder, is vendored under
`scripts/testlib/` **for tests only**. Each label's SVG is parsed back into a
module matrix and compared **bit-for-bit** against segno's output for the same
payload. That single assertion covers the mode selection, the codewords, the
Reed–Solomon ECC, the mask, *and* this codebase's matrix→SVG mapping: any of them
being wrong shows up as a mismatch.

Two things had to be understood before the comparison could work:

- **qrcodegen boosts the error-correction level.** `encodeText` defaults to
  `boostEcl = true`, so it silently upgrades M → Q → H whenever the data still
  fits the chosen version. That is free robustness on a label and worth keeping,
  but it means the level on the wire is not the level requested. The test decodes
  the **format information** out of the symbol itself (15 bits, unmasked with
  `0x5412`) to learn the actual ECC and mask, then asks segno for exactly that.
- **segno 1.6.6 has a padding bug.** Its `write_padding_bits` is
  `buff.extend([0] * (8 - (length % 8)))`, which appends a spurious zero *byte*
  when the stream already ends on a codeword boundary. ISO/IEC 18004 §7.4.10 —
  quoted verbatim in segno's own docstring — requires padding only "if the bit
  stream length is such that it does not end at a codeword boundary".
  qrcodegen computes `(8 - size % 8) % 8` and is correct.

The bug only bites when the terminated stream is an exact multiple of 8 bits,
which is why 8 of 10 sweep payloads agreed and two did not. Rather than exclude
those payloads — which would drop the check precisely where encoders disagree —
`scripts/testlib/qrcheck.py` corrects the reference in memory, and
`qrcheck.selftest()` asserts the correction is actually in force so the
comparison can never pass against a broken reference. The vendored source is left
untouched so the discrepancy stays visible.

Six payloads now match bit-for-bit across versions 1–4, ECC levels M/Q/H, five
masks, and all three encoding modes.

The structural checks that follow — finder patterns at exactly three corners with
the 1:1:3:1:1 profile, alternating timing patterns, the always-dark module at
`(8, 4v+9)`, a clear quiet zone — are **not** the proof. They exist so that when
the comparison fails, the output says which part is wrong instead of just
"matrices differ".

## 5. Typography

Without font metrics, line widths are estimated (`0.62` advance/size for
Helvetica). Estimating is the honest option; the question is what to do when a
line does not fit.

Shrinking without limit produces a line that technically fits and cannot be read.
So text shrinks to a **1.7 mm floor** and is then **truncated with an ellipsis** —
a cut part name is still useful, a 3 pt one is not. The test asserts both: that
a narrow label ellipsises, and that no `font-size` on it drops below the floor.

Truncation is also why two assertions in the first version of the test were
wrong: they grepped for text that the renderer had correctly cut. They now ask
for a wide label when checking XML escaping and payload printing.

## 6. Not done

- **No linear barcode.** Code128 is the obvious next symbology for existing
  warehouse scanners, and it is straightforward to encode — but I have no way to
  verify one here beyond re-deriving it from the same table I would encode with,
  which proves nothing. It should be added alongside an independent reference,
  the same way the QR was.
- **No label-designer UI.** Sizes and content come from query parameters; there
  is no screen for laying out a custom label.
- Label stock is assumed to be a plain grid on A4. No support for named
  Avery-style templates or for printer-specific offsets.
