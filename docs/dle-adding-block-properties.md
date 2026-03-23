# How to Add Properties to DLE Blocks

`DocumentLayoutEditor` (DLE) lives in `web/static/src/app.js`.

---

## 1. Add the column to `ir_report_template` (backend)

In `modules/report/ReportModule.hpp`, inside `ensureSchema_()`:

```cpp
txn.exec("ALTER TABLE ir_report_template ADD COLUMN IF NOT EXISTS my_prop INTEGER NOT NULL DEFAULT 0");
```

---

## 2. Expose the column in `handleSearchRead`, `handleRead`, `handleWrite`

**`handleSearchRead`** — add to the SELECT and to the result serialisation loop:
```cpp
// SELECT
"my_prop, "

// result loop
obj["my_prop"] = row["my_prop"].as<int>(0);
```

**`handleRead`** — same pattern.

**`handleWrite`** — read from `vals`, add to both UPDATE branches (with-template and without):
```cpp
int myProp = vals.value("my_prop", json(0)).get<int>();
// ...
txn.exec_params("UPDATE ir_report_template SET ..., my_prop=$N WHERE id=$M",
                ..., myProp, id);
```

---

## 3. Add to the render SELECT (and use it)

In the main render route (`/report/html/...`) and `portalRenderDoc` in `PortalModule.hpp`:

```cpp
"SELECT template_html, paper_format, orientation, "
"COALESCE(my_prop, 0) AS my_prop "
"FROM ir_report_template WHERE model=$1 AND active=true ORDER BY id LIMIT 1"
// ...
const int myProp = tplRows[0]["my_prop"].as<int>(0);
```

---

## 4. Add the property to `docSettings` state (frontend)

In the `setup()` method, inside `useState({...})`:

```js
docSettings: { ..., my_prop: 0 },
```

---

## 5. Load it in `onDocTypeChange`

```js
const tpls = await RpcService.call('ir.report.template', 'search_read',
    [[['model', '=', model]]],
    { fields: ['id', ..., 'my_prop'], limit: 1 });
if (tpls && tpls.length > 0) {
    this.state.docSettings.my_prop = tpls[0].my_prop ?? 0;
}
```

---

## 6. Save it in `onSave`

```js
my_prop: parseInt(this.state.docSettings.my_prop) || 0,
```

---

## 7. Add a handler METHOD on the class (critical — see pitfall below)

```js
setMyProp(ev) { this.state.docSettings.my_prop = parseInt(ev.target.value); }
```

> **OWL pitfall — inline assignment:** `t-on-change` handlers **must** be named component methods
> when the body is an assignment. Inline arrow functions with assignment expressions
> (e.g. `t-on-change="ev => this.state.foo = ev.target.value"`) compile to an assignment statement
> instead of a callable function, producing `TypeError: vN is not a function` at runtime.
> Simple method calls with no assignment (e.g. `t-on-change="() => this.toggleBlock(idx)"`) are fine inline.
>
> **Save pitfall — zero coalescing:** Never use `parseInt(x) || default` when `0` is a valid value.
> `parseInt(0) || 2` evaluates to `2` because `0` is falsy. Use instead:
> `Number.isInteger(x) ? x : default`

---

## 8. Make the handler call `rebuildHtml()`

If the property affects the rendered output (e.g. decimal formatting), call `this.rebuildHtml()` at
the end of the handler so the preview updates immediately:

```js
setMyProp(ev) {
    this.state.docSettings.my_prop = parseInt(ev.target.value);
    this.rebuildHtml();
}
```

`dleRenderPreview` receives `this.state.docSettings` via `rebuildHtml` and can use the values to
format dummy data (see `dleFormatPrec` for the decimal example).

---

## 10. Wire up the template

Show the property only for the relevant block type, using `togglePropSect`/`isPropSectOpen` to
make the section collapsible exactly like the built-in block property groups:

```xml
<t t-if="state.blocks[state.selectedBlock].type === 'items_table'">
    <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
         t-on-click="()=>this.togglePropSect('My Section')">
        <span>My Section</span>
        <span class="dle-acc-icon" t-esc="isPropSectOpen('My Section') ? '\u25BE' : '\u25B8'"/>
    </div>
    <t t-if="isPropSectOpen('My Section')">
    <div class="dle-prop-row">
        <label>My Property</label>
        <select class="dle-prop-input" t-on-change="setMyProp">
            <t t-foreach="[0,1,2,3]" t-as="n" t-key="n">
                <option t-att-value="n"
                        t-att-selected="state.docSettings.my_prop === n ? true : undefined"
                        t-esc="n"/>
            </t>
        </select>
    </div>
    </t>  <!-- end t-if isPropSectOpen -->
</t>  <!-- end t-if block type check -->
```

---

## Checklist

| Step | Location | Pitfall |
|------|----------|---------|
| `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` | `ReportModule.hpp` `ensureSchema_()` | — |
| SELECT + serialise in search_read / read | `ReportModule.hpp` `handleSearchRead`, `handleRead` | — |
| Parse + UPDATE in write | `ReportModule.hpp` `handleWrite` | — |
| `COALESCE(col, default)` in render SELECT | `ReportModule.hpp` render route + `PortalModule.hpp` `portalRenderDoc` | — |
| Add to `docSettings` state | `app.js` DLE `setup()` | — |
| Load in `onDocTypeChange` | `app.js` DLE | — |
| Save in `onSave` | `app.js` DLE | Use `Number.isInteger(x) ? x : default`, NOT `x \|\| default` (zero is falsy) |
| **Named method** on class (never inline assignment) | `app.js` DLE | OWL `t-on-change` with assignment body → `vN is not a function` |
| Call `this.rebuildHtml()` in handler | `app.js` DLE | Without this, preview won't update when property changes |
| Collapsible header with `togglePropSect`/`isPropSectOpen` | `app.js` DLE xml template | Don't forget the arrow icon `\u25BE`/`\u25B8` |
| `t-on-change="methodName"` in template | `app.js` DLE xml template | — |
