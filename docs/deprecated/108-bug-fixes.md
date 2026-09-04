# 108 — Bug fixes

Status: **three fixed and verified**, `./scripts/run_tests.sh` → 76 passed, 0 failed.
Two classes of test defect remain and are listed honestly in §4.

---

## 1. `.list-toolbar` had no CSS rule at all

**Symptom:** on Products → Configuration → Categories the title and the
"+ New Category" button were jammed together against the left edge.

**Cause:** `.list-view-wrap`, `.list-toolbar` and `.list-title` were written into
the template but never given a rule anywhere in `app.css`. The browser fell back
to `display: block`, so the title and button laid out as inline content.

Measured before: `display: block`, `btnLeft: 135` immediately after the title,
with ~1200px of the bar unused. After: `display: flex`, `align-items: center`,
`gap: 12px`, button at `btnLeft 839 → btnRight 974` inside `barRight 984`.

Padding matches `.list-table`'s cells (10px) so the title lines up with the
column headings beneath it. Actions use `margin-left: auto` rather than
`justify-content: space-between`, so a second button lands beside the first
instead of splitting the row.

## 2. `product.category.search_read` ignored the domain

**Symptom:** `[["name","=","PCB Assembly"]]` returned all 80 categories. So did a
domain matching nothing.

**Cause:** the handler understood only `active` and `parent_id`, and **silently
skipped every other leaf**. The clause stayed `1=1`.

A filter that quietly widens is the worst failure mode available: the caller
believes it asked a narrow question and gets an answer to a different one. It is
also how a test can pass for the wrong reason — the first UI check "found" the
new category because it found *everything*.

**Fix:** support `id`, `name`/`display_name` (`=`, `!=`, `like`, `ilike`,
`=ilike`), `parent_id` and `active`; bind every value as `$n` (S-49); and
**refuse unknown fields and operators** rather than ignoring them.

## 3. Three dead navigation links

**Symptom:** clicking a row in the Parts Catalogue, a result in Parametric
Search, or a card on the Task Board did nothing at all.

**Cause:** all three did `window.location.hash = '#action=…&id=…'`. **This
application has no hash router.** Nothing listens for `hashchange`; screens are
reached by clicking menus, which calls `MainApp.activateLeaf()`. The URL changed
and the page sat there.

It failed silently and looked plausible — the address bar even updated — which is
why it survived three separate screens.

**Fix:** a real navigation hook. `MainApp` registers `window.ErpNav` on mount and
removes it on unmount, exposing one method:

```js
await window.ErpNav.openRecord('product.product', 42);
```

which finds an action for the model (so a component does not need to know which
menu points at it), loads it, and hands the record id to `ActionView` through a
new `initialRecordId` prop. `ActionView` opens the form directly instead of
dropping the user on a list they have to search.

`pendingRecordId` is cleared by `goHome()` and by every menu click, so a stale
record can never re-open. An unknown model returns `false` and warns rather than
pretending it worked.

Verified end to end against the real shell: the hook exists, `openRecord` returns
true, the action switches to `product.product`, the shell leaves the home screen,
and the record's name is on screen. A bogus model returns `false`.

## 4. Not fixed — and why they are worth naming

These are **test** defects, not application ones. Both are measured, listed and
reproducible; neither is fixed.

### 4.1 Seven scripts are not hermetic (docs/104)

Against the working database: 75 passed. Against the clean baseline: 68 passed,
7 failed.

    verify_money_recompute    verify_money_roundtrip
    verify_new_views_smoke    verify_no_double_audit
    verify_product_variants   verify_read_group
    verify_tax_engine

They fail because a product or order they expect to find does not exist in a
clean database. A test that only passes because an unrelated script left a
product behind is not testing what it claims, and its green is a coincidence.
Each fix is small — create the fixture instead of assuming it — but there are
seven of them and each needs its own reading.

### 4.2 Eleven scripts leak rows (docs/103)

`./scripts/audit_test_leaks.sh` lists them with per-table deltas. The restore
step makes the leaks harmless between runs; it does not make the scripts
hermetic *within* a run, so a script that reads another's debris is still wrong.

### 4.3 Smaller, known, and left

- `verify_credit_note` fails standalone — it depends on fixtures `run_tests.sh`
  seeds before the suite. It passes in the suite, but that is a hidden
  dependency.
- The Help Centre still writes `&help=<slug>` into the hash for deep links. That
  is harmless — it uses `replaceState` and never expects a router — but a link
  shared with a colleague only reopens the article if they are already on the
  Help screen. It should go through `ErpNav` too.
