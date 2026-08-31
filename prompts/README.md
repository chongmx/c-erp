# prompts/

The instructions c-erp sends to an AI provider, as editable text rather than
C++ string literals.

They are here so that a deployment team can change how the agent behaves —
tone, strictness, vocabulary, house conventions — **without rebuilding the
server, and with the change visible in git** like any other change.

## The files

| File | Task | Used by |
|---|---|---|
| `part_lookup.md` | Identify a component and return candidates | Products → Part Lookup |
| `help_assistant.md` | Answer a question from the manual | Help → Help Centre, assistant rail |
| `bom_headers.md` | Map the columns of an unrecognised BOM export | Manufacturing → BOM Editor |
| `bom_clean.md` | Normalise imported BOM rows to house conventions | Manufacturing → BOM Editor |

`bom_clean.md` is the one most worth editing for your own shop: it encodes how
*you* write a value, a package and a description. The shipped version prefers
`4k7` over `4.7K` and `0603` over `C_0603_1608Metric` — change it if your house
style differs.

## Placeholders

`{{name}}` is replaced at call time. **The code decides which placeholders
exist for each task** — it is what supplies the values — so the list is fixed
per task and shown on screen in **Settings → AI Agent → Prompts**.

A placeholder the task does not define is left in the text exactly as written,
which is usually a typo you want to see rather than a silent blank.

Removing a **required** placeholder is refused when saving: a part-lookup
prompt with no `{{query}}` in it asks the model about nothing at all.

## Where the text actually comes from

Three sources, in order:

1. **A database override**, if somebody edited the prompt in Settings → AI
   Agent. It wins, and the screen says so.
2. **The file here.** This is the normal case.
3. **A copy compiled into the binary**, used only if the file is missing, so a
   bad deployment degrades to working-but-stale rather than broken. The screen
   warns when this happens.

Edits made in the UI are stored in the database, *not* written back here — a
process that rewrites its own git-tracked source files is a process that fights
whoever deployed it. Use the UI to try something; move it into the file to keep
it. **Reset to file** in the UI drops the override.

## A word of warning

These are instructions to a model that can search the web and whose answers
land in your catalogue. Loosening a rule here loosens it for real data.

The code's own safeguards do *not* live in these files and cannot be edited
away — the double-multiplier guard, the unit allowlist, the staging queue, and
the rule that the agent never picks a part all sit in C++ and still apply to
whatever a rewritten prompt produces. Treat that as a floor, not a licence.
