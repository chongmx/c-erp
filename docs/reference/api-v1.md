# REST API v1 — API keys and the issue tracker

For scripts, CI jobs and AI agents. The browser uses JSON-RPC with a session
cookie ([http-api.md](http-api.md)); this API uses **API keys** and speaks plain
JSON over REST. The first area it covers is the issue tracker.

## API keys

Create one under **Settings → Users & Access → API Keys**. A key:

- **acts as you.** Record rules, the company boundary, and comment and history
  authorship are exactly what they are for you in the browser: the routes call
  the same server methods the screens do.
- **has scopes** — the only things it may do:

  | scope | allows |
  |---|---|
  | `tickets:read` | list and search tickets; read a ticket, its comments, history and attachments; list projects, statuses, labels, users |
  | `tickets:write` | create tickets; change status, assignee, reporter, type, priority, labels, due date, estimate, parent, project; watch/unwatch |
  | `comments:write` | post comments; edit or delete **your own** |
  | `attachments:write` | upload files and screenshots to tickets; remove them |

  Any write scope includes `tickets:read`.
- **may be limited to projects.** A ticket outside them answers **404**, as
  though it did not exist, so a key cannot probe for other projects.
- **expires** after 30 / 90 / 365 days, or never, and can be **revoked** at any
  time. Revocation is immediate: every request re-reads the key.
- **is shown once.** Only its SHA-256 is stored. Lose it, revoke it, make another.
- **is not a session.** JSON-RPC (`/web/*`) ignores it, so a key can never create
  another key, and can reach nothing its scopes do not name.

Send it as a header:

```
Authorization: Bearer cerp_…
```

Keys are rate-limited to 300 requests a minute each. Every use updates the key's
last-used time, address and count, shown on the API Keys page.

In a multi-tenant installation a key belongs to the database it was created in.

## Conventions

- Bodies are JSON (`Content-Type: application/json`), except uploads (multipart).
- Tickets are addressed by **key**: `CERP-12`, any case.
- Errors: `{"error": "<code>", "message": "<what to do>"}` with the status —
  `400 invalid`, `401 unauthorized | revoked | expired`,
  `403 insufficient_scope | forbidden`, `404 not_found`, `429 rate_limited`,
  `500 internal`, `503 unavailable`.
- Unknown fields in a body are **refused**, not ignored — a typo fails loudly.
- Priorities are `low`, `normal`, `high`, `urgent`. Types are `task`, `bug`,
  `feature`, `chore`. Statuses are the project's board columns, by name
  (`GET /projects/{key}/statuses`).
- People (`assignee`, `reporter`) are a login, an email or a user id; `null`
  clears.

## Endpoints

All under `/api/v1`.

| method | path | scope | |
|---|---|---|---|
| GET | `/me` | any | who the key acts as, its scopes and projects |
| GET | `/projects` | read | `[{key, name, tickets}]` |
| POST | `/projects` | write | `{name, key?, description?}` → **201**; `key` is the ticket prefix (CERP), derived from the name when omitted. A key limited to projects cannot create one |
| GET | `/projects/{key}/statuses` | read | `[{name, closed}]`, in board order |
| GET | `/labels` | read | label names |
| GET | `/users?q=` | read | `[{id, login, name}]` — people a ticket can be assigned to |
| GET | `/tickets` | read | search — see below |
| POST | `/tickets` | write | create; answers **201** with the ticket |
| GET | `/tickets/{key}` | read | the ticket |
| PATCH | `/tickets/{key}` | write | change fields; answers with the ticket |
| GET | `/tickets/{key}/activity` | read | comments and history, oldest first |
| GET | `/tickets/{key}/comments` | read | comments only |
| POST | `/tickets/{key}/comments` | comments | `{"body": "…"}`; **201** |
| PATCH | `/comments/{id}` | comments | `{"body": "…"}` — your own only |
| DELETE | `/comments/{id}` | comments | your own only (an admin: any) |
| GET | `/tickets/{key}/attachments` | read | `[{id, name, mimetype, size, download_url, markdown}]` |
| POST | `/tickets/{key}/attachments` | attachments | multipart, field `file` (optional `name`); **201** |
| GET | `/attachments/{id}` | read | the bytes, always as a download |
| DELETE | `/attachments/{id}` | attachments | |
| PUT / DELETE | `/tickets/{key}/watch` | write | watch / stop watching |

### Searching — `GET /tickets`

| parameter | |
|---|---|
| `project` | a project key |
| `state` | `open` (default), `closed`, `all` |
| `status` | a status name |
| `type` | task, bug, feature, chore |
| `label` | a label name |
| `assignee` | a login, `me`, or `none` |
| `q` | words in the key or title (matched literally) |
| `limit`, `offset` | at most 200 per page (default 50) |

Answers `{total, limit, offset, tickets: [...]}`, most urgent first.

### A ticket

```json
{
  "key": "CERP-12", "id": 41, "url": "/api/v1/tickets/CERP-12",
  "title": "Save button stays on Saving", "description": "…",
  "type": "bug", "priority": "high", "priority_value": 1,
  "status": "In Progress", "closed": false, "blocked": false,
  "project": "CERP", "project_name": "c-erp",
  "assignee": {"id": 1, "name": "Administrator"}, "reporter": {"id": 1, "name": "Administrator"},
  "parent": null, "labels": ["ui"], "watchers": [{"id": 1, "name": "Administrator", "login": "admin"}],
  "subtasks": [{"key": "CERP-13", "title": "…", "status": "New", "closed": false, "type": "task"}],
  "statuses": ["New", "In Progress", "Review", "Done", "Cancelled"],
  "due": "2026-12-31", "estimate_hours": 2, "logged_hours": 0,
  "comment_count": 1, "attachment_count": 1,
  "created_at": "2026-09-19T10:00:00", "updated_at": "2026-09-19T11:30:00", "closed_on": ""
}
```

### Creating and changing — fields

`POST /tickets` needs `project` and `title`. Both it and `PATCH` accept:
`title`, `description`, `type`, `priority`, `status`, `assignee`, `reporter`,
`due` (`YYYY-MM-DD` or `null`), `estimate_hours`, `blocked` (bool), `parent`
(a key or `null`), `labels` (names — replaces the set; new names become labels),
and `project` (a key — moving a ticket gives it the new project's next key).

Every change is written to the ticket's history as its owner, in words —
`Status: In Progress → Done` — the same line the screen writes.

### Screenshots in text

Upload, then put the answer's `markdown` (`![name](/web/content/42)`) into a
description or comment; the ticket screen shows it inline.

## Examples

```sh
export CERP=https://www.example.com CERP_KEY=cerp_…
auth=(-H "Authorization: Bearer $CERP_KEY")

curl "${auth[@]}" "$CERP/api/v1/tickets?project=CERP&type=bug"

curl "${auth[@]}" -H 'Content-Type: application/json' -X POST "$CERP/api/v1/tickets" \
     -d '{"project":"CERP","title":"Save stays on Saving","type":"bug","priority":"high","labels":["ui"]}'

curl "${auth[@]}" -H 'Content-Type: application/json' -X PATCH "$CERP/api/v1/tickets/CERP-12" \
     -d '{"status":"Done"}'

curl "${auth[@]}" -H 'Content-Type: application/json' -X POST "$CERP/api/v1/tickets/CERP-12/comments" \
     -d '{"body":"Fixed in 5ff39e6."}'

curl "${auth[@]}" -F file=@screenshot.png "$CERP/api/v1/tickets/CERP-12/attachments"
```

## Adding an area

Scopes are declared in `scopeDefs()` in `modules/api/ApiModule.cpp`, grouped by
area, and the Settings page lists them from there. An area adds its scopes and
its routes; routes call the area's existing view models through `callVm`, so the
rules the screens enforce are the rules the API enforces.

Tests: `tests/integration/api/tickets-v1` (every endpoint, every refusal) and
`tests/functional/core/api-keys` (the Settings page, by clicking).
