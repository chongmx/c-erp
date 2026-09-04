# 124 — The website media library

---

## 1. What was missing

The editor could place an image block. It could not place an **image**.

The only field was a URL typed by hand, so every picture on the site had to be
hosted somewhere else. There was an upload route and an `ir_attachment` table,
but neither helped:

* `GET /web/content/{id}` **requires a session** — a visitor asking for it gets
  401, so an uploaded file could never appear on a public page;
* `ir_attachment.public` existed as a column and was **never read by anything**.

So the CMS had a picture-shaped hole in it, and the column that was supposed to
close it was dead.

---

## 2. What it is now

| Route | Who |
|---|---|
| `POST /site/api/media` | upload — Settings / Configuration group |
| `GET /site/api/media` | list the library — same group |
| `GET /site/media/{id}` | **public** |

The body of the POST *is* the file; the display name rides in `?name=`. The
Customize tab gains a media control for the image block: a preview, an
**Upload…** button, and a **Library** grid to pick something already uploaded.

Uploads are stored as `ir_attachment` rows with `res_model='website'` and
`public=TRUE`, so they travel with a `pg_dump` like everything else and are
visible to the rest of the ERP as ordinary attachments.

---

## 3. Security

This is the only route in the module that hands **attacker-supplied bytes** to
an anonymous visitor, so it is built around two ideas.

### The type comes from the bytes

A `Content-Type` header, a file extension and a filename are all supplied by
whoever is uploading. Only the leading bytes say what a file is. `sniff()`
accepts PNG, JPEG, GIF and WebP by signature and returns "" for everything
else, and a file that returns "" is never stored.

The recorded filename is rebuilt rather than echoed: basename only,
charset-restricted, length-capped, and given the extension the **bytes** imply.
A GIF uploaded as `shell.php` is stored as `shell.gif`. Every dot is stripped
from the base, so the only dot in the result is the one before the real
extension — a double extension has nothing to smuggle with. This lands in a
`Content-Disposition` header, so S-39 applies: no quote, no CRLF, no semicolon
survives.

### SVG is refused outright

It is the one "image" format that is really a document: it is XML, it can carry
`<script>`, and served from our own origin it would run with our origin's
privileges — a stored XSS with an `<img>` tag for a delivery mechanism. There
is no sanitiser here good enough to make that safe, so the format is simply not
accepted, and the refusal says why rather than failing mysteriously.

### The serve route re-checks everything

It does not trust that the upload route did its job:

1. the row must be `public = TRUE` **and** `res_model = 'website'` — without
   the second condition a public route over `ir_attachment` with an id in the
   URL would serve invoices, payslips and expense receipts;
2. the stored mimetype must still be one of the four — a row that acquired
   `text/html` by any route is refused **on the way out**;
3. the bytes on disk are sniffed again and must match the row.

Plus `X-Content-Type-Options: nosniff`, so a browser cannot re-type the
response into something executable.

---

## 4. A defect found while testing

Drogon's default client body limit is about **1 MB**. The handler's own 8 MB cap
was therefore unreachable: every image over ~1 MB — which is to say every
ordinary phone photo — died with a bare `413` before the handler ran, so the
size check never fired and its explanation was never shown.

Found by probing 0.5 / 1.2 / 3 / 7 MB uploads rather than by assuming the cap
worked because the small fixture passed.

`setClientMaxBodySize(12 MB)` in `HttpServer.hpp` gives the route room to
enforce its own limit and say why. The 3 MB case is now an assertion, so this
cannot silently come back.

---

## 5. Tests

**Unit — `tests/unit/website/test_media.cpp`, 49 assertions.** The signature
catalogue, where each case costs microseconds: four accepted formats including
both JPEG variants and both GIF versions; SVG in four disguises (bare, with an
XML declaration, behind a BOM, behind whitespace); HTML, PDF, ZIP, shell
scripts, ELF; and the near misses — a truncated PNG signature, two of JPEG's
three bytes, `RIFF`/`WAVE` which is a `.wav` and not a WebP.

Then the filename rules: traversal in both slash directions, header-breaking
characters, the empty and punctuation-only cases, and the 500-character cap.

**Integration — `tests/integration/website/media/`, 34 checks.** Upload →
database → public URL, with real files carrying real signatures. A visitor can
fetch a published image; a public attachment belonging to `hr.payslip` is
**404** from this route; a website row claiming `text/html` is 404; clearing
`public` takes an image off the internet and restoring it puts it back; an
ordinary employee gets 403 on upload and on the library, but can still *view* a
published image, because that is what published means.

### Two test bugs worth recording

* The opening `cleanup` call deleted `$TMP` before the fixtures were written
  into it, so every upload sent a missing file and curl returned nothing. The
  temp directory is now created *after* that call.
* A unit assertion compared `begin()` of one `safeName()` temporary with
  `end()` of a **different** temporary — iterators into two different objects,
  which is undefined behaviour and happened to compare unequal. One call, one
  variable.
