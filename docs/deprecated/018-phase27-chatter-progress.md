# Phase 27 — Chatter / Audit Log

**Status:** ✅ Code complete — build clean — ⚠️ Not yet tested in browser
**Date:** 2026-03-21
**Build:** Clean

---

## What was implemented

### 1. Backend — modules/mail/MailHelpers.hpp (new)

Shared inline helper — include this in any ViewModel that needs to write
audit-log entries inside an open `pqxx::work`:

```cpp
odoo::modules::mail::postLog(txn, "sale.order", id, 0,
    "Sales order confirmed.", "log_note");
```

Parameters: `(txn, res_model, res_id, author_id, body, subtype)`
- `author_id = 0` → stored as NULL → displayed as "System"

---

### 2. Backend — modules/mail/MailModule.hpp (new)

**Table created at startup:**
```sql
CREATE TABLE IF NOT EXISTS mail_message (
  id        SERIAL PRIMARY KEY,
  res_model TEXT,
  res_id    INTEGER,
  author_id INTEGER REFERENCES res_users(id) ON DELETE SET NULL,
  body      TEXT NOT NULL DEFAULT '',
  subtype   TEXT NOT NULL DEFAULT 'note',
  date      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS mail_message_target_idx ON mail_message (res_model, res_id);
```

**MailMessageViewModel** — `mail.message`:
- `search_read` — custom LEFT JOIN query returns `author_name` field; filters by domain `[['res_model','=','...'],['res_id','=',N]]`
- `create` — inserts a new log entry; returns new id
- `fields_get` — metadata for frontend

---

### 3. Backend — postLog calls added to 5 ViewModels

| Module | Method | Log message |
|--------|--------|-------------|
| SaleModule.hpp | `action_confirm` | "Sales order confirmed." |
| SaleModule.hpp | `action_cancel` | "Sales order cancelled." |
| PurchaseModule.hpp | `action_confirm` | "Purchase order confirmed." |
| PurchaseModule.hpp | `action_cancel` | "Purchase order cancelled." |
| AccountModule.hpp | `action_post` | "Invoice posted." |
| AccountModule.hpp | `button_cancel` | "Invoice cancelled." |
| AccountModule.hpp | `button_draft` | "Reset to draft." |
| StockModule.hpp | `action_confirm` | "Transfer confirmed." |
| StockModule.hpp | `button_validate` | "Transfer validated." |

All postLog calls are inside the same `pqxx::work` transaction, so they roll back automatically if the main action fails.

---

### 4. Backend — main.cpp + CMakeLists.txt

- Added `#include "modules/mail/MailModule.hpp"` to main.cpp
- Registered `MailModule` before IrModule (after AuthModule) so the table exists when the other modules boot
- Added `modules/mail` to `target_include_directories` in CMakeLists.txt

---

### 5. Frontend — ChatterPanel component (app.js, before InvoiceFormView)

Shared OWL 2 component. Props:
- `model` — res_model string
- `recordId` — record ID
- `refreshKey?` — increment to force reload after a workflow action

Features:
- Fetches `mail.message` with domain filter on mount
- Reloads when `recordId` or `refreshKey` changes (`onWillUpdateProps`)
- Shows message feed: avatar (first letter of author name), author name, date, body
- "Log a note" textarea + Post button
- Post sends `uid` from `RpcService.getSession().uid` as `author_id`

---

### 6. Frontend — all 5 form views updated

| Form view | Change |
|-----------|--------|
| InvoiceFormView | Added `static components = { ..., ChatterPanel }`, chatter at template end, `chatRefreshKey` in state, incremented after action_post / button_cancel / button_draft |
| SaleOrderFormView | Same — incremented after action_confirm / action_cancel |
| PurchaseOrderFormView | Same — incremented after action_confirm / action_cancel |
| TransferFormView | Replaced static stub — `ChatterPanel` replaces `<div class="trn-chatter">` static entry; incremented after action_confirm / button_validate / action_cancel |
| ProductFormView | Replaced static stub — `ChatterPanel` replaces static "Product record loaded" stub; `chatRefreshKey` in state |

ChatterPanel is only rendered for existing records (`!isNew` guard).

---

### 7. Frontend — CSS (app.css)

Added shared `chatter-*` CSS classes:
- `.chatter-panel` — outer container (same style as old `.trn-chatter`)
- `.chatter-head` — "LOG" uppercase label
- `.chatter-feed` — flex column message list
- `.chatter-entry` — single message row
- `.chatter-avatar` — circular initial badge
- `.chatter-body` — author + date + message text
- `.chatter-compose` — textarea + Post button area
- `.chatter-input` — styled textarea

Old `.trn-chatter-*` classes are kept in CSS for backward compat (no code uses them anymore).

---

## Architecture notes

- `postLog()` writes with `author_id = 0` → NULL → "System" in UI. A real per-user author requires session propagation from Drogon request context into the ViewModel call chain — deferred.
- ChatterPanel does NOT paginate (limit 50). Sufficient for v1.
- Product form has no workflow actions that write log entries; only user-posted notes appear in the product chatter.

---

## What's still deferred

- Per-user author attribution (propagating session uid into ViewModels) — Phase A-auth
- Chatter pagination / infinite scroll — Phase A-log
- Chatter subtype icons (internal note vs comment) — Phase A-log
- Real-time chatter refresh via WebSocket — future

---

## Next: Phase 17e — PDF Reports

Generate PDF reports via wkhtmltopdf:
- `/report/sale_order/<id>` — Sales Order PDF
- `/report/invoice/<id>` — Invoice PDF
- `/report/purchase_order/<id>` — Purchase Order PDF
- `/report/delivery/<id>` — Delivery / Transfer PDF
