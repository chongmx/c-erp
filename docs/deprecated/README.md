# Deprecated — the historical record

**Nothing in this folder describes the current system.** These are the working
notes of how it got built: session logs, audits, plans that were executed, plans
that were abandoned, and bug reports whose bugs are long fixed.

They are kept for provenance — *why* is a decision made this way, what a
measurement actually showed, what was deliberately not done — and for nothing
else.

## Do not

- Do not treat a statement here as true of the code today. Much of it was
  already stale when it was written down; more of it has gone stale since.
- Do not pick a menu, action or group id from `026-ir-id-registry-and-checklist.md`.
  Run `bash tests/integration/core/menu-ids/test.sh`, which reads the source and
  prints the next free id.
- Do not follow a plan document. Every one of them either shipped, in a form
  the plan does not describe, or was dropped.
- Do not cite one of these to justify a change. Cite the code.

## Do

- Read one when you want the *reasoning* behind something surprising in the
  code — a measurement, a rejected alternative, a bug that shaped a design.
- Follow a `docs/NNN` reference from a source comment. Many `core/` and
  `modules/` files cite these by number; that is what the numbers are still for.

## What is current instead

Everything in the parent folder. Start at [../README.md](../README.md).

Some specific replacements:

| If you came here for | Read instead |
|---|---|
| the schema (`database-schema.md` was 37 of 130 tables) | [../reference/database-schema.md](../reference/database-schema.md) |
| the id registry (`026`) | [../reference/id-registry.md](../reference/id-registry.md) + the menu-ids test |
| the roadmap (`plan.md`, `044`, `040`) | nothing — those roadmaps are done or abandoned |
| the test architecture (`109`) | [`tests/README.md`](../../tests/README.md) and [`tests/docs/`](../../tests/docs/) |
| the baseline workflow (`104`) | [../operations/database.md](../operations/database.md) |
| the multi-company plan (`072`, `094`) | [../architecture/multi-company.md](../architecture/multi-company.md) |
| a security audit (`062`, `068`, `071`, `038`) | [../security/README.md](../security/README.md) and `tests/security/` |
| the frontend security plan (`security-assessment-plan.md`) | [../security/README.md](../security/README.md) |
| what a module does (`001`–`011`, `029`–`030`, `115`–`128`) | [../architecture/modules.md](../architecture/modules.md) |

## Contents

**137 historical files** (plus this README), in three shapes:

- **`000`–`130`** — the numbered log, one document per change, oldest first.
  Each records what was wrong, what was done, what it cost, and what was
  deliberately not done. A later document does not supersede an earlier one
  unless it says so.
- **`executed-plan-1.md`, `-2.md`, `-3.md`** — the phase plans that were
  carried out, covering roughly phases 1–17g plus the early security hardening.
- **`plan.md`, `account-module-plan.md`, `security-assessment-plan.md`** —
  forward-looking documents, all superseded.

Ordering note: `000`–`130` is chronological, so the most recent material is at
the *end*, and the early files (`000`–`030`) describe a system with about a
third of today's tables and none of the multi-company, rental, website or parts
work.

## Adding to this folder

Don't, as a rule. If a change is worth writing down, write it into the page it
affects in the parent folder — that is what keeps the documentation describing
the system rather than describing its history.
