# 110 — AI agent integration (Claude)

Status: **built through §6 step 5.** Settings → AI Agent, multi-provider key
storage, outbound HTTPS, the Part Lookup bridge, **web search**, **multiple
candidates**, **an editable review queue** and the **help assistant** are all
working. Verified live against xAI. §8 records what the first live call taught
us; §9 covers web search and §10 the help assistant.

**Unverified:** Anthropic's `web_search_20250305` server tool is configured but
has never returned a successful call — that key has no credit. The xAI path is
fully exercised.

---

## 1. The decision

**The API key lives in the database, and only there.**

Chosen deliberately: the database is then the complete migration unit. Move the
dump, and the new machine has the configuration, the credentials and the data —
nothing to remember, nothing to set up out of band, nothing that works on the
old box and silently doesn't on the new one.

The trade this accepts: **a database backup now carries a live credential.**
Anyone who can restore a dump can use the key. That is the price of seamless
migration and it was chosen with the alternative (environment variable) on the
table.

Two things follow, and they are not optional if the choice is to hold.

### 1a. The template dumps must be scrubbed

`backups/odoo/default-clean.dump` and `default-demo.dump` are **starting
points meant to be shared** — a clean ERP and a demo catalogue. They must never
carry a real key.

So the dump-building scripts blank `ir_ai_settings.api_key` before capture, and
verify it is blank afterwards. A working backup keeps the key (that is the
point); a template dump does not.

### 1b. A backup is now a secret

The Database & Backup screen should say so where the backups are listed:
*"Backups contain the AI API key. Treat them as credentials."* One sentence, at
the point where somebody is about to download or share one.

## 2. What "exclude the master admin" means

**Decided: the shared templates only. Working backups keep every password.**

| Dump | Passwords | Restoring it means |
|---|---|---|
| Your backups (`log/pretest.dump`, anything from the Backup screen) | **kept, untouched** | a rollback. Everyone logs in exactly as before. Nobody resets anything. |
| `default-clean.dump` / `default-demo.dump` | admin hash blanked | a **fresh install** from a template, where setting a password is expected anyway |

A rollback is a recovery, and demanding a password reset at the moment somebody
is recovering from a problem is help nobody asked for. It is also pointless:
the person restoring the dump already has the dump.

The templates are different in kind. They exist to be handed to somebody else,
so they must not ship a working administrator login — and they contain exactly
one user, so "blank the admin" costs nothing there.

The `res_users` row still travels in both cases; the ERP needs its admin to
exist with its id, groups and companies intact. Only the template's hash is
cleared.

> **What a working backup therefore contains:** password *hashes* (not
> plaintext — `AuthService` stores them hashed) and, once §1 is built, a
> **plaintext API key**. The key is the sensitive item in a dump, not the
> passwords. That is the trade §1 accepted, and it is why §1b puts the warning
> on the screen where backups are downloaded.

## 2b. When something needs it in the ENVIRONMENT

The database stays canonical. If a component expects `ANTHROPIC_API_KEY` in the
environment, the server **derives** it at boot rather than asking the operator
to maintain a second copy — which is the whole point of the DB-only decision:
restore the dump on a new machine and it configures its own environment.

Three cases, and they are not equally safe.

### It works: this process and its children

At boot and on save, the server calls `setenv("ANTHROPIC_API_KEY", …)` from the
stored value. Anything running **in-process** sees it immediately, and so does
any child process the server spawns afterwards.

### It cannot work: the operating system

`setenv` changes **this process only**. It cannot reach back into the parent
shell, a systemd unit, or a container definition — nothing outside the process
can be "installed" into from inside it. Any design that assumes otherwise is
assuming something the OS does not allow.

That is where the copy-paste fallback belongs (§2c).

### The part worth being careful about

A secret in the process environment is **more exposed, not less**:

- it is readable from `/proc/<pid>/environ` by the same user and by root,
- it appears in `ps e` output and in crash dumps,
- and it is **inherited by every child process** — which here means `pg_dump`,
  `pg_restore`, `wkhtmltopdf`, and the vendored Python label helpers, none of
  which have any business holding an API key.

So: **do not export it globally.** Set it in the environment of the specific
child that needs it, at the point of spawning, and leave the server's own
environment clean. If something in-process needs it, hand it the value directly
rather than through `getenv`.

This is a real widening of exposure and it buys nothing unless a component
genuinely cannot be handed the value another way. Prefer passing it.

## 2c. The copy-paste fallback

For the cases the process cannot reach — a systemd unit, a Docker run line, a
shell profile — the settings screen offers the exact line for the target,
generated from the stored value:

```
systemd    Environment="ANTHROPIC_API_KEY=sk-ant-…"      (drop-in under /etc/systemd/system/c-erp.service.d/)
docker     -e ANTHROPIC_API_KEY=sk-ant-…
shell      export ANTHROPIC_API_KEY=sk-ant-…
```

**This is the one place the key legitimately reaches the browser, and it
contradicts rule §4.1 on purpose.** So it is not the normal read path:

- a separate admin-only action (`reveal_for_setup`), never part of
  `search_read` or `read`;
- masked until an explicit click, so it is not sitting on screen behind
  somebody's shoulder;
- **audited** — every reveal writes an audit row with who and when, because a
  credential that can be revealed without a trace is one nobody can reason
  about after an incident;
- and the screen says plainly that the value is about to be shown.

## 3. Where the settings live

**Settings → AI Agent**, backed by a new model `ir.ai.settings` — a single row.

Not `ir_config_parameter`: that table is a plain key/value store with no access
control, so any authenticated user could `search_read` the key straight out of
it.

| Field | Purpose |
|---|---|
| `enabled` | master off switch |
| `provider` | `anthropic` · `mock` (tests) |
| `api_key` | **write-only** — see §4 |
| `model` | `claude-sonnet-5` default · `claude-opus-5` for hard parts · `claude-haiku-4-5-20251001` for bulk |
| `max_output_tokens` | cost ceiling per call |
| `daily_call_cap` | cost ceiling per day |
| `capabilities` | starts as *Part Lookup only* |
| `last_ok_at`, `last_error` | what "Test connection" learned |

## 4. Rules the implementation must keep

1. **The key never reaches the browser** through a normal read. `search_read`
   and `read` return `configured: true/false` and the last four characters —
   never the value. Every path that returns it is a path that can leak it.
   The single exception is the audited `reveal_for_setup` action in §2c, which
   exists because the operator sometimes genuinely needs to paste it elsewhere.
2. **The key never reaches an error message.** SEC-28 already gates `ex.what()`
   behind devMode; the AI service must also scrub the key from anything it logs,
   because an HTTP client will happily print the Authorization header.
3. **Admin-only.** Reading or writing `ir.ai.settings` requires admin, like the
   database tools.
4. **The agent gets no write access.** It may call `describe` and `submit`
   only. Proposals stage; a person applies them.

## 5. Why the agent stays behind the staging gate

The agent reads datasheets and vendor pages — **untrusted text that can carry
instructions aimed at the model.** Because `submit` only stages a proposal and
a human clicks Apply, a hijacked or confidently-wrong response cannot silently
write a wrong resistance into a part somebody solders.

That containment is the existing Part Lookup contract (docs/097), and this
integration is deliberately built to fit inside it rather than beside it.

## 6. Build order

1. **`ir_ai_settings` table + model + admin guard**, with the write-only key.
   Nothing calls out yet.
2. **Settings → AI Agent screen**, with "Test connection" — one minimal call
   that reports ok/fail and never echoes the key. First outbound HTTP in this
   codebase: async, timeout, retry, and off the request thread.
3. **Scrub step in the dump builders** (§1a) plus the warning on the backup
   screen (§1b). Do this in the same pass as 2, not later — the first key
   entered is the one that ends up in a template dump.
4. **"Ask the agent" on Part Lookup** — MPN in, staged proposal out.
5. Broaden only after watching it be wrong a few times.

## 6b. Outbound HTTPS — where it sits

**Check first whether it is needed.** Drogon is already linked and ships
`drogon::HttpClient`. If this build has TLS support compiled in, an HTTPS call
costs no new dependency and runs on the existing event loop. Verify before
adding libcurl:

```cpp
auto c = drogon::HttpClient::newHttpClient("https://api.anthropic.com");
```

If that cannot do TLS here, libcurl is the better second choice — not because
drogon's client is bad, but because for a **secret-bearing call to one
endpoint** the things that matter are explicit timeouts, explicit redirect
policy and explicit certificate verification, and curl exposes all three
plainly.

### Placement

```
core/infrastructure/HttpClient.{hpp,cpp}   the ONLY file that includes <curl/curl.h>
core/AiService.{hpp,cpp}                   provider logic: builds the Anthropic request,
                                           parses the response into a LookupResult
modules/ir/IrModule.cpp                    calls AiService. Never curl, never a URL.
```

Why there, in this codebase's terms:

- `core/infrastructure/` already holds what talks to the outside world —
  `DbConnection`, `SessionManager`, `JsonRpcDispatcher`. An outbound client is
  the same category of thing.
- `core/` holds services with domain knowledge — `Money`, `TaxEngine`,
  `LabelRenderer`, `DbBackup`. `AiService` belongs beside them: it knows what a
  part lookup is, not what a socket is.
- **PERF-E holds**: `curl.h` appears in exactly one `.cpp`. Swapping curl for
  drogon's client later touches one file and recompiles one translation unit.

### CMake

```bash
sudo apt install libcurl4-openssl-dev libssl-dev   # headers are not installed yet
```

```cmake
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(c-erp PRIVATE
    drogon
    pqxx
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
)
```

`curl_global_init(CURL_GLOBAL_DEFAULT)` runs once at startup, before any
worker thread exists — it is not thread-safe and calling it lazily from two
threads is a real crash, not a theoretical one.

### The threading rule

**A curl call must never run on drogon's event loop.** It is synchronous; a
10-second API call on the loop stalls every other request on that thread, and
the pool-exhaustion 503 you already return would start firing for reasons
nobody could explain.

Run it on a worker thread. For `test_connection` a short synchronous wait off
the loop is acceptable (bounded, one call, an operator is watching). For part
lookups, queue the work and let the screen poll — the proposal already stages
before anyone sees it, so nothing needs the answer synchronously.

### Non-negotiable curl options

| Option | Value | Why |
|---|---|---|
| `CURLOPT_SSL_VERIFYPEER` | `1` | never disable; without it the TLS is decorative |
| `CURLOPT_SSL_VERIFYHOST` | `2` | verifies the certificate matches the host |
| `CURLOPT_PROTOCOLS_STR` | `"https"` | a redirect must not downgrade to http |
| `CURLOPT_FOLLOWLOCATION` | `0` | never follow a redirect while holding an `Authorization` header |
| `CURLOPT_CONNECTTIMEOUT` / `CURLOPT_TIMEOUT` | 5s / 30s | a hung call must not hold a worker forever |
| `CURLOPT_VERBOSE` | `0` in production | verbose prints headers, and the key is a header |

And the rule that outranks them: **the Authorization header is never logged.**
A debug flag that prints the request is a debug flag that prints the key.

## 7. Testing

- **`provider=mock` returns a canned `LookupResult`**, so the suite never
  touches the network and CI needs no secret.
- One opt-in test for the real call, marked `needs=network` and skipped by
  default — the same pattern the render checks use when Chrome is missing.
- A test that asserts **the key is never returned** by `search_read`, `read`,
  or any error response — and that `reveal_for_setup` is the only path that
  returns it, requires admin, and **writes an audit row**. That is the
  assertion that stops a future refactor quietly exposing it.
- A test that asserts the server's **own** environment does not hold the key
  (§2b), so nobody "simplifies" the per-child handling into a global export and
  hands it to `pg_dump` by accident.
- A test that asserts **`default-clean.dump` and `default-demo.dump` contain no
  key**, so §1a cannot rot.

## 8. What the first live call taught us — the double multiplier

The very first real lookup (`RC0805FR-074K7L`, via Grok) came back correct in
every field but one:

```
resistance  value "4k7"   unit "kΩ"     <- the multiplier written TWICE
power       value "125m"  unit "W"      <- correct
tolerance   value "1"     unit "%"      <- correct
```

`apply()` stores `parseSiValue(value) × unit.factor`. `4k7` parses to 4700 and
`kΩ` has factor 1000, so that part would have been filed as **4.7 MΩ** — a
thousandfold error, in a plausible-looking field, on a part somebody solders.

This is worth dwelling on because of *how* it failed. Not a crash, not a
refusal, not a low confidence score — the model reported **0.9** and every other
field was right. It is precisely the confidently-wrong output §5 exists for, and
it arrived on call number one.

### Three responses, in order of how much they are worth

1. **The prompt** now spends more words on units than on everything else
   combined, states the rule as "write the magnitude once", and shows the exact
   wrong form as a worked negative example. The unit vocabulary is sent grouped
   by quantity with the base marked, so the *relationship* between `Ω` and `kΩ`
   is visible rather than implied by a flat list.

2. **A server-side guard**, because a prompt is a request and not a constraint.
   When the value carries a multiplier *and* the unit does, `normaliseUnits_()`
   takes the value as authoritative — `4k7` means 4.7k on every schematic ever
   printed — and demotes the unit to its base. The correct rows are left alone:
   a guard that "fixes" good input is worse than none.

3. **It is reported, never silent.** The response carries an `adjusted` array
   and Part Lookup renders it as a warning above the proposal. A correction
   nobody can see is its own kind of wrong — the reviewer is the one who has to
   judge whether the reading we chose was the intended one, and it is also how
   we find out the prompt is still losing.

## 9. Web search — the thing that was missing

Until this was built, "ask the agent" **did not look anything up**. It sent a
prompt and got back whatever was in the model's weights, including a `source`
URL the model had never opened. Every answer looked identical to a researched
one. That is the most dangerous shape a feature can have: right often enough to
be trusted, with nothing on screen distinguishing recall from research.

### The wire is a third shape, and it is stored, not compiled

| provider | how it searches |
|---|---|
| Anthropic | `tools: [{type: "web_search_20250305", …}]` on `/v1/messages` |
| xAI | a **Responses** call on `/v1/responses` — `input` not `messages`, `output[]` back |
| mock | cannot; reports `searched: false` |

xAI's original `search_parameters` was retired mid-flight and now answers
**410 "Live search is deprecated"**. Discovering the replacement took probing
the API, which answered helpfully once asked wrongly enough:

```
422 tools[0].type: unknown variant `web_search`, expected `function` or `live_search`
410 Live search is deprecated. Please switch to the Agent Tools API
200 /v1/responses + tools:[{type:"web_search"}]      <- the working route
```

So `search_style`, `search_tool` and `search_path` live in `ir_ai_provider`.
A vendor retiring an API is then a settings change, not a rebuild — which is
the whole reason that table exists.

**A bug this uncovered:** the error extractor only handled `error` as an
*object* with `.message`. xAI returns it as a bare *string*, so the one message
that explained the failure — "Live search is deprecated, switch to…" — was
being discarded, and the screen said only "the service replied 410". An error
path that swallows the error is worse than no error path.

### What the screen shows, and why each part earns its place

- **searched the web / from memory** — the single most important fact about an
  answer, and the one nothing else reveals.
- **Searched for** — the actual queries. A bad search explains a bad result;
  without it a wrong answer is inexplicable.
- **Pages it read** — cited URLs first, then everything the search returned.
  These are two different lists and conflating them would overstate the
  evidence.
- **Notes** — the agent's own account of what it could not settle.

### Candidates, not an answer

A part number has one right answer; a description usually does not. `0805 4.7k
1%` is made by Yageo, Vishay and Walsin alike, and silently staging the first
one buries the ambiguity that was the most useful thing the query revealed. So
`ask` returns up to `max_candidates` (default 3), each with a `why`, and a
person picks. Verified live: that query returned Walsin, Yageo and Vishay
equivalents, with the notes explaining "no single 'the' part; any equivalent
works".

## 10. The help assistant

The Help Centre's right rail was a **stub** — a static "Not connected yet"
paragraph and a `disabled` textarea, written before any of this existed. It is
now wired to the same provider plumbing.

Three decisions worth recording:

1. **Not admin-only.** Configuring the agent is an administrator's job; asking
   the manual a question is everybody's. A new `status` method returns exactly
   two booleans — `ready` and `admin` — and reads no configuration at all, so
   it cannot leak one.
2. **Retrieval is term-by-term.** The first version matched the whole question
   as a single `ILIKE` substring, which finds nothing, because no article
   contains "how do I write 4k7 units" verbatim. It silently answered "not in
   the manual" for questions the manual answers well. Scoring now counts how
   many of the question's terms an article carries, weighting title and
   keywords above body.
3. **Citations are filtered against what was actually retrieved.** A model can
   cite a slug it invented, and a dead button is worse than no button.

It deliberately does not search the web: the manual is the authority on this
ERP, and browsing would invite a confident answer about a different one.

## 10b. The second BOM seam — tidying rows

`clean_bom_rows` normalises the *text* of staged rows to house conventions and
hands them back through `bom.import parse` as `rows:[...]`, which was already
the documented agent path. Nothing new is trusted.

Verified live on a deliberately messy set:

```
"C1, C2, C5"  designators  "C1, C2, C5"                     → C1,C2,C5
              value        100nF                            → 100n
              footprint    Capacitor_SMD:C_0603_1608Metric  → 0603
              description  ~                                → (blank)
R1-R4         quantity     0                                → 4
              mpn          R-EU_R0603                       → (blank)
              value        4.7K ohm                         → 4k7
TP1           fitted       true                             → false
```

Three of those are judgements worth noting: it **counted** `R1-R4` to fill in a
missing quantity, recognised `R-EU_R0603` as a *library reference* rather than
an MPN, and read "DNP" out of a description to clear the fitted flag.

### The identity guard

**Same row count back, or the whole answer is refused.** A model asked to tidy
60 rows will sometimes return 58 — merging two that look alike, or dropping one
it could not parse — and a BOM quietly missing a line is far worse than an
untidy one: the board is short a part and nothing anywhere says so. There is no
safe way to reconcile that, so it is not attempted.

Fields are also merged *onto* our row rather than replacing it, so a field the
model omits keeps its original value instead of vanishing.

### Still no part selection

It rewrites text. Resolution runs again afterwards on the server, which is what
keeps an import reproducible: the same rows always resolve to the same parts,
whether or not anyone tidied them first.

## 11. Prompts as files

The prompts were C++ string literals, which made the part of this most likely
to need tuning per deployment the one part that needed a rebuild to change.
This is an open-source project; a deployment team should be able to change what
the agent is told without touching the build.

```
prompts/part_lookup.md      Products → Part Lookup
prompts/help_assistant.md   Help Centre assistant rail
prompts/bom_headers.md      BOM Editor column mapping
prompts/bom_clean.md        BOM Editor tidy-up (house conventions)
prompts/README.md           the contract, for whoever opens the folder first
```

`bom_clean.md` is the one a deployment is most likely to want to change: it is
where "how we write a value" lives. Shipping that as a file rather than a
string literal is most of the point of this section.

`{{placeholder}}` is substituted at call time. **The code owns the placeholder
list** — it is what supplies the values — so it is fixed per task, shown on
screen, and an unknown `{{typo}}` is left in the text verbatim rather than
silently blanked, because a visible typo is one somebody can find.

### Three sources, in order

| | |
|---|---|
| **override** | a row in `ir_ai_prompt`, written only by the UI |
| **file** | `prompts/*.md` — the normal case |
| **compiled** | a terse copy in the binary, used only when the file is missing |

The compiled copy exists so a bad deployment degrades to working-but-stale
rather than breaking every AI feature, and the screen says loudly when it is in
use. It is deliberately *not* the shipped text: keeping the defaults out of
both the binary-as-source-of-truth and the database is what makes **Reset to
file** mean something, rather than restoring whatever happened to be default at
install time.

### The UI does not write to the files

An override is stored in the database. A process that rewrites its own
git-tracked sources fights whoever deployed it, and turns `git status` into
noise. Try it as an override; keep it by moving it into the file.

### What a rewritten prompt still cannot do

Prompt text is *advice* to a model. Every safeguard that matters is in C++ and
applies to whatever the model returns regardless of what it was asked:

- the double-multiplier guard (§8) still demotes `4k7 kΩ`
- unknown units are still rejected at `submit`
- `invalid` proposals still cannot be applied
- the agent still cannot write to the catalogue

That separation is the reason it is safe to hand the prompts to operators at
all. If a safeguard could be prompted away, it was never a safeguard.

### The mock provider is the regression fixture

`provider=mock` deliberately returns **the exact broken shape above** and then
runs through the same `normaliseUnits_()` a live answer does. A mock that
returned clean data would exercise none of the post-processing, and since the
suite has no network, "untested offline" would mean untested.
`tests/integration/core/ai-settings` §7c asserts the corrected value reaches the
database as `4700`, not `4700000`, by actually applying the proposal.
