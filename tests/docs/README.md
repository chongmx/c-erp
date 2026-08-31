# tests/docs/

Things about testing this system that are **not derivable from the code** and
cost real time to rediscover. Environment quirks, hard-won recipes, and the
traps that produce a confidently wrong answer.

Anything explaining how the suite is *structured* belongs in `tests/README.md`
or `docs/109`. This folder is for the knowledge that would otherwise live only
in someone's head.

| Document | Read it before |
|---|---|
| [tooling.md](tooling.md) | **start here.** Every test tool, its flags, and the helper functions — with the traps each one hides |
| [test-plan.md](test-plan.md) | adding coverage — the measured surface, what is covered, and what to write next (§4b is the proposal awaiting review) |
| [menu-coverage.md](menu-coverage.md) | auditing coverage — every menu option nested as it appears, with its model and tests. **Generated:** `python3 scripts/gen_menu_doc.py` |
| [browser-render-checks.md](browser-render-checks.md) | claiming any screen "works", or trying to drive the UI from a browser |

## The rule these documents share

Most entries here exist because a check **passed while the thing it checked was
broken**. That is the failure mode worth writing down:

- an assertion that cannot fail on missing data (`t_ne "0" ""` is true for an
  empty string, so three checks passed against a query that had errored);
- a restore that reported success while silently not restoring a table;
- a sequence audit that matched no rows and reported "nothing is wrong";
- a green API suite in front of a screen that renders nothing at all.

When you find another, add it here.
