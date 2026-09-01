#!/usr/bin/env python3
"""Generate tests/docs/menu-coverage.md — every menu option, nested, with the
test that covers it. Regenerate any time; it is derived, never hand-edited.

    python3 tests/tools/gen_menu_doc.py
"""
import os, subprocess, collections, datetime

# Repo root by walking up for CMakeLists.txt — a hardcoded absolute path only
# worked on the machine it was written on.
ROOT = os.path.dirname(os.path.abspath(__file__))
while ROOT != '/' and not os.path.isfile(os.path.join(ROOT, 'CMakeLists.txt')):
    ROOT = os.path.dirname(ROOT)
os.chdir(ROOT)
env = dict(os.environ, PGPASSWORD='odoo')

def q(sql):
    out = subprocess.run(['psql', '-h', 'localhost', '-U', 'odoo', '-d', 'odoo', '-tAF', '\x1f', '-c', sql],
                         capture_output=True, text=True, env=env).stdout
    return [l.split('\x1f') for l in out.splitlines() if l.strip()]

rows = q("""
SELECT m.id, COALESCE(m.parent_id,0), m.name, COALESCE(m.sequence,999),
       COALESCE(a.res_model,''), COALESCE(a.name,'')
  FROM ir_ui_menu m
  LEFT JOIN ir_act_window a ON a.id = m.action_id
 ORDER BY COALESCE(m.sequence,999), m.id
""")

kids = collections.defaultdict(list)
for mid, parent, name, seq, model, aname in rows:
    kids[int(parent)].append({'id': int(mid), 'name': name, 'seq': int(seq),
                              'model': model, 'action': aname})

# Which test files mention each model.
tests = []
for dirpath, _dirs, files in os.walk('tests'):
    if 'test.sh' in files:
        p = os.path.join(dirpath, 'test.sh')
        tests.append((dirpath[len('tests/'):], open(p, encoding='utf-8', errors='replace').read()))

def covering(model):
    if not model:
        return []
    return sorted(name for name, body in tests if model in body)

RENDER = [name for name, body in tests if 'render.mjs' in body]

lines, stats = [], collections.Counter()
leaf_rows = []

def walk(parent, depth):
    for m in sorted(kids.get(parent, []), key=lambda x: (x['seq'], x['id'])):
        pad = '  ' * depth
        children = kids.get(m['id'], [])
        if m['model']:
            cov = covering(m['model'])
            stats['leaves'] += 1
            if cov:
                stats['covered'] += 1
                mark = '✅' if len(cov) > 1 else '🟡'
                note = f"`{m['model']}` — {mark} {', '.join('`'+c+'`' for c in cov[:3])}"
                if len(cov) > 3:
                    note += f" +{len(cov)-3}"
            else:
                stats['uncovered'] += 1
                note = f"`{m['model']}` — ❌ **no test**"
            lines.append(f"{pad}- **{m['name']}** · {note}")
            leaf_rows.append((m['name'], m['model'], cov))
        else:
            lines.append(f"{pad}- **{m['name']}**" + ('' if children else ' · _(no action)_'))
        walk(m['id'], depth + 1)

walk(0, 0)

pct = (stats['covered'] * 100) // max(stats['leaves'], 1)
uncovered = sorted({(m, n) for n, m, c in leaf_rows if not c})
thin = sorted({(m, n) for n, m, c in leaf_rows if len(c) == 1})

doc = f"""# Menu coverage — every page in the ERP

Every menu option, nested exactly as it appears in the interface, with the
model it opens and the tests that touch that model.

**Generated from the database, not written by hand.** Regenerate after adding
a menu or a test:

```bash
python3 tests/tools/gen_menu_doc.py
```

| | |
|---|---|
| Menu options that open a page | **{stats['leaves']}** |
| Covered by at least one test | **{stats['covered']}** ({pct}%) |
| No test at all | **{stats['uncovered']}** |

Legend: ✅ two or more tests · 🟡 exactly one (thin — often only a
"the form opens" smoke check) · ❌ none.

> **The {pct}% is generous and is not page coverage.** A page counts as covered
> when ANY test mentions the model behind it. Many pages share one model —
> every entry under Accounting → Journals, Customers and Vendors is
> `account.move` — so one well-tested model marks a dozen distinct pages green.
> Credit Notes and Refunds are separate screens with separate behaviour; the
> tests that make them ✅ were written for invoices.
>
> The two audit lists at the bottom are the real backlog. Treat 🟡 as
> "probably untested" until you open the named test and check.

> **Coverage here means "a test mentions this model".** It does not mean the
> page is exercised, and it never means the page was rendered. Read
> [test-plan.md](test-plan.md) §3 for what a real test of a page has to do,
> and [browser-render-checks.md](browser-render-checks.md) for why a green
> API test can sit in front of a blank screen.

---

## The menu tree

{chr(10).join(lines)}

---

## Audit list — no test at all ({len(uncovered)})

| Model | Page |
|---|---|
{chr(10).join(f'| `{m}` | {n} |' for m, n in uncovered)}

## Audit list — thin, one test only ({len(thin)})

Each of these is touched by a single test, and several only incidentally.
Check whether that test actually exercises the page or merely opens its form.

| Model | Page |
|---|---|
{chr(10).join(f'| `{m}` | {n} |' for m, n in thin)}

## Pages with a browser render check

Only these have been proven to draw: {', '.join('`'+r+'`' for r in RENDER) if RENDER else '_none_'}.

Every other page in this document is unverified visually, including every one
marked ✅.

---

_Generated {datetime.date.today().isoformat()} from `ir_ui_menu` × `ir_act_window`._
"""

open('tests/docs/menu-coverage.md', 'w', encoding='utf-8', newline='\n').write(doc)
print(f"leaves={stats['leaves']} covered={stats['covered']} uncovered={stats['uncovered']} thin={len(thin)}")
