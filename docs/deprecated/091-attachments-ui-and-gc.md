# 091 — Attachments: the UI, and the bytes nobody was reclaiming

docs/090 listed `ir.attachment` as "not built". That was wrong, and checking the
database rather than trusting the note turned up a different, more interesting
problem.

Suite: **63 passed, 0 failed**.

## What was already there

`ir.attachment` is fully implemented and has been for some time:

- `ir_attachment` (migration 1040) with the reference ERP column set — `res_model`,
  `res_id`, `res_field`, `mimetype`, `file_size`, `checksum`, `store_fname`,
  `public`, indexed on `(res_model, res_id)` and on `checksum`;
- `IrAttachmentModel` + a registered viewmodel, so it is reachable over RPC;
- `POST /web/attachment/upload` — multipart, 25 MB cap, extension/mime
  allowlist (no executables), basename-only filename handling, `res_model`
  validated against the live model registry;
- `GET /web/content/{id}` — streams the bytes, `?download=1` forces a download,
  the filename is charset-restricted before it reaches the header (S-39);
- `core::Filestore` — **content-addressed** storage at `data/filestore/<h[:2]>/<h>`,
  so the request filename never becomes a path and identical files are stored once;
- 18 checks in `verify_ir_primitives.sh` covering upload, byte-for-byte
  download, and dedup.

That is a well-built feature. The table had **zero rows**.

## What was actually missing

**1. No UI.** `grep -rn attachment web/static/src/` returned nothing. Every
piece of plumbing existed and no screen in the application could reach it, which
is why the table was empty and why the note in docs/090 looked true from the
outside.

**2. Deleting an attachment leaked its bytes.** `Filestore::gc()` was written —
correctly, including the reference-count argument that makes deduplicated
storage safe — and **nothing ever called it**. Confirmed directly rather than by
reading: upload a file, `unlink` the row over RPC, and the row is gone while the
blob is still sitting in `data/filestore`. Every deletion would have leaked, for
the life of the install.

## The fix

### `IrAttachmentViewModel`

Replaces the bare `GenericViewModel<IrAttachmentModel>`:

- **`unlink`** reads each row's `store_fname` *before* the delete (afterwards
  there is nothing to look it up from), performs the delete, then counts the
  rows still pointing at each distinct blob and calls
  `Filestore::gc(store_fname, remainingRefs)`. Because storage is
  content-addressed, two attachments genuinely share one file: `gc()` is a no-op
  while anyone still references it, which is precisely the case a naive
  "delete the row, delete the file" would corrupt.
- **`search_read`** returns what a file list actually needs — `url`
  (`/web/content/<id>`), `size_human`, the upload timestamp and the uploader —
  instead of a raw byte count and a path every caller would have to rebuild.

The bytes still never cross JSON-RPC. A base64 blob on every `search_read` would
defeat the point of storing the file out of the row.

### `AttachmentPanel`

A generic OWL component, shaped like the existing `ChatterPanel`
(`model` + `recordId`, plus optional `title` and `readonly`):

- lists the record's files with icon, name, size, date, download and remove;
- uploads by click **or** drag-and-drop, through the multipart route with
  `credentials: 'same-origin'` — not through `RpcService`;
- states the limits in the panel rather than failing after a 25 MB upload.

Adding files to another form is now one tag.

Wired into two places:

| Form | Panel | Note |
|---|---|---|
| Expense report | **Receipts** | `readonly` once the report leaves draft — what the approver saw is part of the record from then on |
| Product | **Datasheets & Documents** | the original PartKeepr motivation (docs/029) |

## Guard

`scripts/verify_attachments.sh` (22 checks). The two load-bearing ones are the
pair a naive delete gets wrong, and they are asserted against the real filesystem:

- deleting an attachment **reclaims its bytes** — the regression that was live;
- deleting one of two attachments sharing a blob **must not** pull the file out
  from under the other, and the survivor must still download.

Plus the round trip the panel depends on (upload against a record, list back
*only* that record's files, download byte-for-byte), and the guards: a `.exe` is
refused, and both routes reject an unauthenticated caller.

One test bug worth recording: `grep -o '"id":[0-9]*'` matches **zero** digits, so
the JSON-RPC envelope's own `"id":null` counted as a result and every list looked
one item longer than it was. `[0-9][0-9]*`.

## Housekeeping

One orphaned blob existed from before the fix (created while confirming the leak)
and was removed. There is deliberately **no** startup sweep for orphans: a
"delete every blob with no row" pass would race an upload that has written its
file but not yet committed its row. `gc()` on the delete path is the correct
place, and it is now wired.

## Still open

- `rental_expense.attachment_id` remains an unregistered column. It predates
  `ir.attachment` and should be dropped in favour of the polymorphic
  `(res_model, res_id)` link the panel already uses — a migration, not a field
  registration.
- No attachments on invoices, purchase orders or transfers yet. One tag each
  when wanted.
