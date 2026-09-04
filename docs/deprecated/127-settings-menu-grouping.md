# 127 — Grouping the Settings menu

---

## 1. What it was

Thirteen entries, flat on one bar:

```
Users · Companies · ERP Settings · Technical ▾ · Portal Users ·
Companies & Access · Database & Backups · AI Agent · Database Tools ·
Website Pages · Website Menu · Website Forms · Form Submissions
```

More than anyone scans, in no particular order — `Database & Backups` and
`AI Agent` both sat at sequence 45, so their relative position was whatever the
id tiebreak produced. `Technical` was the only dropdown.

## 2. What it is

Five entries: one direct link and four headings.

| | |
|---|---|
| **ERP Settings** | direct — the one screen people actually come for |
| **Users & Access** ▾ | Users · Groups · Portal Users · Companies · Companies & Access |
| **Website** ▾ | Website Pages · Website Menu · Website Forms · Form Submissions |
| **Database** ▾ | Database & Backups · Database Tools · Demo Data |
| **Technical** ▾ | Document Templates · AI Agent |

`Groups` moved out of Technical, where it was buried — it is an access concept,
not a technical one. `Demo Data` moved out of Technical into Database, next to
backups and tools, because it is about what is *in* the database.

## 3. Why the headings live in `IrModule`

`ir_ui_menu.parent_id` carries a foreign key, so a child cannot be seeded
before its parent exists. Menus are seeded by seven different modules, each in
its own `ensureSchema_()`, and the order they run in is not something a menu
definition should have to know.

`IrModule` creates the `ir_ui_menu` table *and* seeds the Settings root, so it
is the only place guaranteed to run first. The three new headings (413 Users &
Access, 414 Website, 415 Database) are seeded there, with no `action_id` — a
heading opens nothing, it only holds children.

The website menus resolve their parent **by name** rather than by id:

```sql
SELECT COALESCE(
  (SELECT g.id FROM ir_ui_menu g JOIN ir_ui_menu s ON s.id = g.parent_id
    WHERE g.name = 'Website' AND s.name = 'Settings' AND s.parent_id IS NULL),
  (SELECT id FROM ir_ui_menu WHERE name = 'Settings' AND parent_id IS NULL))
```

The `COALESCE` fallback matters: a database seeded before the headings existed
still gets its website menus attached to Settings rather than losing them.

## 4. Why it had to be done in the source

Every one of these seeds is `ON CONFLICT (id) DO UPDATE SET parent_id = …`, so
the tree is rewritten from source on **every startup**. Re-parenting rows in the
database directly would have looked right until the next restart and then
silently reverted.

Files touched: `IrModule.cpp` (headings, Users, Companies, AI Agent),
`ReportModule.cpp` (Groups, ERP Settings, Companies & Access, Database &
Backups, Database Tools, Technical's sequence), `PortalModule.cpp` (Portal
Users), `RentalModule.cpp` (Demo Data), `WebsiteModule.cpp` and
`WebsiteForm.cpp` (the four website entries).

## 5. Two tests caught it

Both were asserting the old shape, and both were right to fail:

* `integration/core/db-tools` — `parent_id FROM ir_ui_menu WHERE id=74` = `30`
* `integration/rental/rental-demo` — `name='Demo Data' AND parent_id=101`

Rather than swapping one hardcoded id for another, both now assert the **path
by name** — that the entry sits under `Settings → Database`. That keeps them
testing what they were written to test, "reachable from Settings in the right
place", and stops them pinning an id that grouping may move again.
