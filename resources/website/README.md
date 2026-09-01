# resources/website/

The public marketing site — **content, not code**. Every page you see at
`https://www.easylockerspace.com` is built from the files in this folder.

```bash
./scripts/seed.sh website              # push all of it to the database
./scripts/seed.sh website --dry-run    # print the SQL, change nothing
./scripts/seed.sh website --page faq   # just one page
./scripts/seed.sh website --no-publish # load it, but leave it hidden
./scripts/seed.sh website --status     # what is in the database right now
```

## Layout

```
resources/website/
  menu.tsv                     the nav bar:  sequence <TAB> slug <TAB> label
  pages/<slug>/
    meta                       title, homepage, published, indexed, sequence, description
    blocks.json                the page body
  forms/<slug>/
    meta                       title, description, submit label, success message
    fields.tsv                 name, label, type, required, sequence, options
```

A folder with a `meta` file next to its content is the same shape a test uses in
`tests/` — the convention is deliberate.

## `blocks.json` is not HTML, and that is the point

The CMS stores a page as an ordered list of **typed blocks**, and
`WebsiteRender.cpp` turns them into HTML on the way out. The available types are
`hero`, `heading`, `text`, `columns`, `pricing`, `faq`, `steps`, `stats`,
`table`, `gallery`, `image`, `video`, `map`, `quote`, `button`, `cta`,
`divider`, `spacer`, `references`, `form` and `html`.

This file is copied into `website_page.blocks_json` **verbatim** — nothing here
is a template and the deploy script never rewrites it. That is what keeps three
things true at once:

- the in-browser editor (Settings → Website → Website Pages) edits the same data
  this file holds, so neither one silently wins;
- `pricing`, `faq`, `form` and `map` keep their structure instead of collapsing
  into opaque markup;
- content still passes through the renderer's `sanitize()`.

Writing these pages as `.html` would cost all three. If you need raw markup for
one section, use a `{"type": "html", "html": "..."}` block rather than
converting the whole page.

## `meta`

`key<TAB>value`, one per line:

| key | meaning |
|---|---|
| `title` | page title, also the `<title>` tag |
| `homepage` | `yes` on exactly one page — `/` serves it |
| `published` | `no` hides it from the public site |
| `indexed` | `no` keeps it out of the sitemap |
| `sequence` | ordering |
| `description` | `<meta name="description">` |

**`homepage` and `published` together decide what the bare domain shows.**
`WebsiteModule.cpp` asks for a page that is both, and redirects to `/login` when
there is none — so an ERP-only install never answers its own domain with a 404.
A page marked `homepage` but not `published` leaves the front door on the login
screen, which is exactly how this site sat until 2026-09-01.

`website_page_homepage_uniq` is a partial unique index, so the database permits
only one homepage. The deploy script clears the flag from other pages first;
without that, the insert is rejected outright.

## Escaping in the `.tsv` and `meta` files

One record is always one line. A newline inside a value is written `\n`, a tab
`\t`, a backslash `\\`, and the deploy script turns them back. The `size` field
in `forms/storage-enquiry/fields.tsv` is the case that forces this — its options
are a newline-separated list.

## Editing

Either way works, and they meet in the same place:

- **In the browser** — Settings → Website → Website Pages, with paired
  Preview/Source views. Fastest for copy changes; the database is authoritative
  until you export.
- **In these files** — then `./scripts/seed.sh website`. This is the version-
  controlled path, and the one that survives a database reset.

They can drift. After editing in the browser, re-export before committing, or
the next `seed.sh website` will overwrite the browser edits with these files.

## History

Until 2026-09-01 this content lived inside a 16 KB Python script as embedded
JSON literals, which made a copy edit a code change and a diff unreadable. The
script is kept at `scripts/deprecated/website_seed.py` for reference only —
`scripts/seed/website.sh` replaces it and reads from here.
