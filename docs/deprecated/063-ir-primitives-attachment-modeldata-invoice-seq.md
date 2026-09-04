# 063 — ir.sequence (INV numbering), ir.attachment, ir.model.data

**Date:** 2026-08-06
**Status:** ✅ Complete and verified (28 suites green; `verify_ir_primitives` = 18 checks)

---

## 1. Invoice numbering — `INV000001`

`ir.sequence` already existed (P4); the ask was a numbering *policy* change. Customer
invoices are now a single continuous series regardless of journal:

```
account.move.INV   prefix "INV"   padding 6   reset never   ->  INV000001, INV000002, …
```

Migration 1020 seeds it. Both places that number a customer invoice — the manual
`AccountModule::handleActionPost` and `RentalBilling` — draw from `account.move.INV` when the
move is `out_invoice`/`out_refund`. Vendor bills, journal entries and payments keep their
per-journal sequences.

> One series, one definition. Neither posting path creates the sequence; the definition lives
> only in the migration, so a prefix/padding change made in the UI is never stamped back by a
> re-run (`ON CONFLICT DO NOTHING`).

Gaplessness and the concurrency guarantee are unchanged — `nextByCode` still takes a row lock
inside the posting transaction, so two concurrent posts serialise and a rolled-back post
releases its number.

---

## 2. `ir.attachment` — files, stored by content hash

Metadata in `ir_attachment` (migration 1040); bytes in a **content-addressed filestore**:

```
data/filestore/<sha256[:2]>/<sha256>
```

The split (row vs blob) is the reference ERP's, and the one `payment_proof` already uses — chosen over a
DB `bytea` column so a 20 MB datasheet does not enter every row SELECT and every `pg_dump`.

### Why content-addressing is the security property, not just an optimisation

The store path is derived from the **hash of the content**, so the uploaded filename never
reaches a path. There is no traversal surface to defend — a filename of
`../../../../tmp/PWNED.pdf` is stored at its hash like anything else, and the test confirms no
file appears outside the filestore. This is stronger than sanitising the name, because there
is no name-to-path step to get wrong.

It also **deduplicates**: the same datasheet uploaded under three names occupies one blob. The
test asserts identical content shares one `store_fname`.

### Routes (both authenticated)

```
POST /web/attachment/upload    multipart: file + optional res_model/res_id/name/description
GET  /web/content/{id}         streams the file; ?download=1 forces attachment disposition
```

Guards: 25 MB size cap; extension/mime allowlist (pdf, images, csv/txt, xlsx/docx, zip — no
executables/scripts); `res_model` validated against the live model registry (an unknown model
is dropped, not stored as a dangling link); `Content-Disposition` filename charset-restricted
(S-39). Verified: upload → download is **byte-identical** (sha256 in == sha256 out), both
routes 401 without a session.

### Datasheets need no product change

A product's datasheets are `ir.attachment` rows with `res_model='product.product'`,
`res_id=<id>`. That generic linkage *is* the feature — the test attaches a datasheet to a
product and reads it back. Listing a product's datasheets is a `search_read` on
`ir.attachment` filtered by `res_model`/`res_id`.

### Blob reclamation is a sweep, not per-unlink — on purpose

Deleting an attachment row does **not** remove the blob. That is deliberate and the reference ERP-aligned:
deduplication makes per-unlink refcounting racy (two rows may share one blob; deleting one
must not pull the file from under the other). `Filestore::gc(store_fname, remainingRefs)`
exists for a periodic sweep that removes a blob only when its last reference is gone. Wiring
that to a daily `ir.cron` is a small, safe follow-up; until then a growing filestore is bounded
by dedup and by how rarely attachments are deleted.

---

## 3. `ir.model.data` — external identifiers

`ir_model_data` (migration 1030) maps a stable `module.name` xml_id to a concrete
`(model, res_id)`, `UNIQUE (module, name)`. `core/IrModelData` provides `ensure` (honouring
`noupdate` so a re-seed never overwrites a hand-edited record), `lookup`, `refId`, and
`xmlIdOf` for the reverse direction. The model is queryable through the standard API.

This is the primitive a lot of later work assumes: referencing config rows by a name that
survives id renumbering, data export with stable ids, and eventual module-uninstall
bookkeeping. Nothing depends on it yet; it is in place for what comes next.

---

## Email server — noted, not built

You flagged it as "soon". `ir.attachment` was the stated prerequisite and is now in place (mail
bodies and outbound messages will carry attachments through the same model). The mail
transport itself — `ir.mail_server`, SMTP, queueing — is a separate piece and was not part of
this change.

---

## Verification

```
verify_ir_primitives   18 checks
  1  INV sequence prefix/padding/reset; posted invoice reads INV000002; no duplicates
  2  attachment upload → metadata → byte-identical download; blob on disk, hash-addressed
  3  traversal: ../ filename writes nothing outside the filestore
  4  both routes 401 without a session
  5  dedup: identical content shares one blob
  6  ir.model.data table, UNIQUE(module,name), API-queryable

full suite            28 suites, all green — the invoice-numbering change regressed nothing
```
