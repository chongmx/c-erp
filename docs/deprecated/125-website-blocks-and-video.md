# 125 — Video, seven new blocks, and a test for a day's editing

---

## 1. Seven new blocks

The CMS had fourteen block types and no way to put a video, a picture gallery,
a customer quote, a number worth boasting about, or a comparison table on a
page. Now it has twenty-one.

| Block | What it is for |
|---|---|
| **video** | a YouTube / Vimeo embed, or a file we host, played inline |
| **gallery** | a responsive grid of pictures with captions |
| **quote** | a testimonial with attribution and a role |
| **stats** | numbers with labels — tabular figures, accent-coloured |
| **cta** | a mid-page call to action band, distinct from the hero |
| **table** | rows and cells, with an optional header row |
| **spacer** | small / medium / large vertical space |

Each renders from the same block model as everything else — the server builds
the HTML, the author supplies text — so none of them adds a route by which
markup can reach a page.

The table wraps itself in `overflow-x: auto`, so a wide one scrolls inside its
own box rather than making the whole page scroll sideways.

---

## 2. Video

### Embeds: the URL never survives

An embed is an `<iframe>` — somebody else's document, running in a frame on our
page. So the author's URL is not passed through. `parseVideo()`:

1. splits the URL and requires an explicit `http`/`https` scheme, which is how
   `javascript:` and `data:` end up with no host at all;
2. discards userinfo (`user@host`) and the port, then lowercases;
3. matches the host **exactly** against a short list;
4. extracts the id and charset-checks it — `[A-Za-z0-9_-]` for YouTube, digits
   for Vimeo;
5. the embed URL is then **built from the id**, on
   `youtube-nocookie.com`, which sets no tracking cookie until the visitor
   presses play.

Exact host matching is the whole game. A prefix test accepts
`youtube.com.evil.example`; a suffix test accepts `notyoutube.com`; a
"contains" test accepts both, and `evil.example/?host=youtube.com` as well.
All four are unit-asserted, along with `www.youtube.com@evil.example`, where
the real host is the one *after* the `@`.

### Self-hosted: a defect found by the test

The first version fell through to `<video src>` for *any* URL that parsed
safely. That meant an unrecognised provider link —
`https://youtube.com.evil.example/watch?v=x` — did not become an embed, but
**did** become a media request the visitor's browser made to an attacker's
host, handing over an IP address and a referrer. Not script execution, but not
nothing either.

`playableFile()` now accepts only a **same-origin path**, or an absolute URL
whose path ends `.mp4` / `.webm` — so a CDN still works and a web page does
not. The integration test asserts the attacker's host appears nowhere in the
rendered page.

### Uploaded video

`sniff()` gained MP4 (the `ftyp` box, with the brand checked rather than
assumed — an ISO-BMFF container that is not one of ours is refused) and
WebM/Matroska (the EBML header). The ceiling is **24 MB** for video against
8 MB for images, and the message on refusal says to use a provider link
instead, because that is the right answer for anything longer.

The size check now runs **after** the sniff, since the ceiling depends on what
the file turned out to be.

---

## 3. Testing a day's editing

Everything else in the website suites proves a *piece* works. None of it proved
that **what people actually do** works, because what they do is a sequence, and
each step happens in a DOM the previous step rebuilt — `draw()` re-renders the
page from data after every change, so a stale index, a lost selection or a
harvest against the wrong DOM only shows up in a sequence.

`tests/integration/website/blocks/journey.mjs` drives one, in Chrome:

open the editor → add a heading and type in it → add a quote and type in it →
add numbers → add a table and type into a cell → **set a video URL from the
sidebar** (it has no representation on the page at all) → add a gallery and
**upload a real PNG through the real file input** → move a block → add a
paragraph and delete it → save.

Then the part that matters: it fetches the page again **with no session** and
asserts the visitor receives the heading, the quote, the typed table cell, the
video as a `youtube-nocookie` embed, and the uploaded picture — and does *not*
receive the deleted paragraph.

Sixteen assertions on one continuous session, plus a hard zero on console and
page errors throughout.

### Two things the test caught about itself

* It inherited the fixture blocks the earlier sections left behind, which point
  at `/site/media/1` — a page that does not exist. Fourteen image 404s were
  being scored as editor errors. The journey now starts from an empty page.
* An assertion counted the string `w-gal-i` and got 6 for 2 pictures: the
  stylesheet mentions the class four times. It counts `<figure class="w-gal-i"`
  now. The same trap as the earlier `liveMarkup()` fix — a class name in a
  page's CSS is not an element on the page.

---

## 4. Test totals

| Tier | Coverage |
|---|---|
| **Unit** — `test_video.cpp` | 47 assertions: every YouTube and Vimeo URL shape, the four host-spoofing tricks, userinfo and port spoofing, `javascript:`/`data:`/`file:`/scheme-relative, ids containing quotes, tags, traversal, and an 80-character id |
| **Unit** — `test_media.cpp` | video signatures added to the existing image catalogue |
| **Integration** — `website/blocks` | every new block rendered through the real save endpoint: happy path, hostile path, and an XSS payload in **every field of every block** |
| **Browser** — `journey.mjs` | the full editing session above |
