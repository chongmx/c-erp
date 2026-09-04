# temp — working notes with a shelf life

Documents that are **useful now but not part of the description of the system**:
a migration plan being executed, an investigation in progress, a decision
record that has not yet become behaviour.

The rest of `docs/` answers "how does this work today". Anything that answers
"what are we in the middle of" belongs here, so those two never get mixed up.

## Rules

- **One topic per file**, named for the topic: `partner-import-plan.md`, not
  `notes.md`.
- **Date it** in the first line, so its age is visible without `git log`.
- **When the work lands, fold it into the real pages and delete the file.**
  The permanent home is `architecture/` for how something is shaped,
  `reference/` for tables and endpoints, `development/` for rules a new file
  must follow, `operations/` for running it, `guides/` for a task walkthrough.
- Nothing here is authoritative. If a page here and the code disagree, the code
  is right — and the page should probably have been deleted already.

Empty is the healthy state.

## Not for

- **Finished descriptions of behaviour** — those belong in the topical pages.
- **History.** The frozen historical record is [`../deprecated/`](../deprecated/);
  it is not appended to.
