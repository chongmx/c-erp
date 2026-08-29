/**
 * CategoryTree.js — Product → Configuration → Categories.
 *
 * Categories are a HIERARCHY, and the old screen showed them as a flat list
 * with a "Parent" column. That is readable only if you already know the shape
 * you are looking at: to answer "what is under Passives" you had to scan every
 * row and reconstruct the tree in your head.
 *
 * So the screen is the shape of the data — a tree on the left, the detail of
 * whatever you clicked on the right.
 *
 * Three decisions worth stating:
 *
 *   * THE WHOLE TREE ARRIVES IN ONE CALL. Lazy-loading each level would be a
 *     request per chevron and, worse, would count products at a different
 *     moment for each level, so a parent could disagree with the sum of its
 *     children on screen.
 *
 *   * TWO COUNTS PER NODE. `direct` is what is filed exactly here; `total` is
 *     everything underneath. A category reading "0" while its children hold
 *     hundreds is what makes people stop trusting the numbers, so both are
 *     shown and the rolled-up one is muted.
 *
 *   * THE SPLIT IS DRAGGABLE AND REMEMBERED. Deep trees need a wide sidebar,
 *     shallow ones do not, and re-dragging it on every visit is the kind of
 *     small friction nobody reports but everybody feels.
 */
class CategoryTree extends owl.Component {
    static template = owl.xml`
        <div class="ct-shell">

            <!-- ── left: the tree ─────────────────────────────────── -->
            <div class="ct-side" t-att-style="'width:' + state.sideWidth + 'px'">
                <div class="ct-side-head">
                    <input class="ct-search" type="search" placeholder="Filter categories…"
                           t-att-value="state.filter" t-on-input="onFilter"/>
                    <button class="ct-icon" t-on-click="expandAll" title="Expand all">⊞</button>
                    <button class="ct-icon" t-on-click="collapseAll" title="Collapse all">⊟</button>
                </div>

                <div class="ct-side-tools">
                    <label class="ct-chk">
                        <input type="checkbox" t-att-checked="state.showArchived" t-on-change="toggleArchived"/>
                        <span>Archived</span>
                    </label>
                    <span class="ct-count" t-esc="visibleCount + ' shown'"/>
                </div>

                <div class="ct-tree">
                    <t t-if="state.loading">
                        <div class="ct-empty">Loading…</div>
                    </t>
                    <t t-elif="!visibleRows.length">
                        <div class="ct-empty">No categories match.</div>
                    </t>
                    <t t-else="">
                        <!-- Flattened to a list of rows carrying their depth,
                             rather than nested elements. A recursive template
                             would build a DOM as deep as the tree; this builds
                             one level and expresses depth as padding. -->
                        <div t-foreach="visibleRows" t-as="n" t-key="n.id"
                             class="ct-row"
                             t-att-class="(state.selected === n.id ? 'sel ' : '') + (n.active ? '' : 'arch')"
                             t-att-style="'padding-left:' + (6 + n.depth * 14) + 'px'"
                             t-on-click="() => this.select(n.id)">
                            <span class="ct-twist" t-if="n.hasKids"
                                  t-on-click.stop="() => this.toggle(n.id)"
                                  t-esc="state.open[n.id] ? '▾' : '▸'"/>
                            <span class="ct-twist blank" t-else=""/>
                            <span class="ct-name" t-esc="n.name"/>
                            <span class="ct-n direct" t-if="n.direct_count" t-esc="n.direct_count"/>
                            <span class="ct-n roll" t-if="n.total_count > n.direct_count" t-esc="n.total_count"/>
                        </div>
                    </t>
                </div>

                <div class="ct-side-foot">
                    <button class="btn" t-on-click="() => this.startCreate(0)">New root category</button>
                </div>
            </div>

            <!-- ── the drag handle ────────────────────────────────── -->
            <div class="ct-grip" t-on-mousedown="startResize" title="Drag to resize"></div>

            <!-- ── right: the detail ──────────────────────────────── -->
            <div class="ct-main">
                <t t-if="state.creating">
                    <div class="ct-panel">
                        <h2 class="ct-h2" t-esc="state.createParent ? 'New sub-category' : 'New category'"/>
                        <p class="ct-sub" t-if="state.createParent">
                            under <b t-esc="nameOf(state.createParent)"/>
                        </p>
                        <div class="ct-form">
                            <input class="ct-input" placeholder="Category name"
                                   t-att-value="state.createName" t-on-input="ev => state.createName = ev.target.value"
                                   t-on-keydown="onCreateKey"/>
                            <button class="btn primary" t-on-click="doCreate">Create</button>
                            <button class="btn" t-on-click="() => state.creating = false">Cancel</button>
                        </div>
                        <div class="ct-err" t-if="state.error" t-esc="state.error"/>
                    </div>
                </t>

                <t t-elif="!state.detail">
                    <div class="ct-blank">
                        <div class="ct-blank-i">🗂</div>
                        <p>Select a category to see what is in it.</p>
                    </div>
                </t>

                <t t-else="">
                    <div class="ct-panel">
                        <!-- where you are -->
                        <div class="ct-crumbs">
                            <t t-foreach="state.detail.path" t-as="p" t-key="p.id">
                                <span class="ct-crumb" t-on-click="() => this.select(p.id)" t-esc="p.name"/>
                                <span class="ct-sep" t-if="!p_last">/</span>
                            </t>
                        </div>

                        <div class="ct-title-row">
                            <t t-if="state.renaming">
                                <input class="ct-input" t-att-value="state.renameName"
                                       t-on-input="ev => state.renameName = ev.target.value"
                                       t-on-keydown="onRenameKey"/>
                                <button class="btn primary" t-on-click="doRename">Save</button>
                                <button class="btn" t-on-click="() => state.renaming = false">Cancel</button>
                            </t>
                            <t t-else="">
                                <h2 class="ct-h2" t-esc="state.detail.name"/>
                                <span class="ct-badge archived" t-if="!state.detail.active">Archived</span>
                                <span class="ct-spacer"/>
                                <button class="btn" t-on-click="startRename">Rename</button>
                                <button class="btn" t-on-click="() => this.startCreate(state.detail.id)">Add sub-category</button>
                                <button class="btn danger" t-on-click="doDelete">Delete</button>
                            </t>
                        </div>
                        <div class="ct-err" t-if="state.error" t-esc="state.error"/>

                        <!-- the numbers -->
                        <div class="ct-stats">
                            <div class="ct-stat">
                                <div class="ct-stat-n" t-esc="state.detail.counts.direct"/>
                                <div class="ct-stat-l">products here</div>
                            </div>
                            <div class="ct-stat">
                                <div class="ct-stat-n" t-esc="state.detail.counts.total"/>
                                <div class="ct-stat-l">including sub-categories</div>
                            </div>
                            <div class="ct-stat">
                                <div class="ct-stat-n" t-esc="state.detail.counts.children"/>
                                <div class="ct-stat-l">direct sub-categories</div>
                            </div>
                            <div class="ct-stat">
                                <div class="ct-stat-n" t-esc="state.detail.counts.descendants"/>
                                <div class="ct-stat-l">all descendants</div>
                            </div>
                        </div>

                        <!-- sub-categories -->
                        <t t-if="state.detail.children_list.length">
                            <h3 class="ct-h3">Sub-categories</h3>
                            <div class="ct-chips">
                                <t t-foreach="state.detail.children_list" t-as="c" t-key="c.id">
                                    <span class="ct-chip" t-on-click="() => this.select(c.id)">
                                        <span t-esc="c.name"/>
                                        <b t-esc="c.direct_count"/>
                                    </span>
                                </t>
                            </div>
                        </t>

                        <!-- what is filed here -->
                        <h3 class="ct-h3">
                            Products
                            <span class="ct-note" t-if="state.detail.counts.direct > state.detail.products.length">
                                showing first <t t-esc="state.detail.products.length"/> of
                                <t t-esc="state.detail.counts.direct"/>
                            </span>
                        </h3>
                        <t t-if="!state.detail.products.length">
                            <p class="ct-none">Nothing is filed directly in this category.</p>
                        </t>
                        <t t-else="">
                            <div class="ct-table-wrap">
                                <table class="ct-table">
                                    <thead>
                                        <tr><th>Reference</th><th>Name</th><th class="num">Sales price</th></tr>
                                    </thead>
                                    <tbody>
                                        <tr t-foreach="state.detail.products" t-as="p" t-key="p.id"
                                            t-att-class="p.active ? '' : 'muted'">
                                            <td class="mono" t-esc="p.code || '—'"/>
                                            <td t-esc="p.name"/>
                                            <td class="num" t-esc="money(p.list_price)"/>
                                        </tr>
                                    </tbody>
                                </table>
                            </div>
                        </t>

                        <!-- accounting properties -->
                        <h3 class="ct-h3">Inventory accounting</h3>
                        <div class="ct-props">
                            <div class="ct-prop">
                                <span class="ct-prop-l">Stock valuation</span>
                                <span class="ct-prop-v" t-esc="accName('valuation')"/>
                            </div>
                            <div class="ct-prop">
                                <span class="ct-prop-l">Stock input</span>
                                <span class="ct-prop-v" t-esc="accName('input')"/>
                            </div>
                            <div class="ct-prop">
                                <span class="ct-prop-l">Stock output</span>
                                <span class="ct-prop-v" t-esc="accName('output')"/>
                            </div>
                        </div>

                        <div class="ct-meta">
                            <span>id <b t-esc="state.detail.id"/></span>
                            <span t-if="state.detail.parent_name">parent <b t-esc="state.detail.parent_name"/></span>
                            <span t-if="state.detail.create_date">created <b t-esc="shortDate(state.detail.create_date)"/></span>
                            <span t-if="state.detail.write_date">updated <b t-esc="shortDate(state.detail.write_date)"/></span>
                        </div>
                    </div>
                </t>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            loading: true,
            nodes: [],
            open: {},
            selected: 0,
            detail: null,
            filter: '',
            showArchived: false,
            sideWidth: this.savedWidth(),
            creating: false,
            createParent: 0,
            createName: '',
            renaming: false,
            renameName: '',
            error: '',
        });
        owl.onWillStart(() => this.load());
        // Resizing listens on the document, not the grip: the pointer routinely
        // leaves a 6px handle mid-drag, and a mousemove bound to the grip stops
        // firing the moment it does.
        this._onMove = (ev) => this.onResize(ev);
        this._onUp   = () => this.endResize();
    }

    // ---------- data ----------
    async load(keepSelection) {
        this.state.loading = true;
        try {
            const r = await RpcService.call('product.category', 'tree',
                [{}], { include_archived: this.state.showArchived });
            this.state.nodes = (r && r.nodes) || [];
            if (!Object.keys(this.state.open).length) {
                // First load: open the roots so the screen is not a wall of
                // collapsed rows, but leave the depths below closed.
                for (const n of this.state.nodes)
                    if (!n.parent_id) this.state.open[n.id] = true;
            }
            if (keepSelection && this.state.selected) await this.loadDetail(this.state.selected);
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.loading = false;
    }

    async loadDetail(id) {
        try {
            this.state.detail = await RpcService.call('product.category', 'detail', [{ id }], {});
            this.state.error = '';
        } catch (e) {
            this.state.detail = null;
            this.state.error = String((e && e.message) || e);
        }
    }

    async select(id) {
        this.state.selected = id;
        this.state.creating = false;
        this.state.renaming = false;
        await this.loadDetail(id);
    }

    // ---------- the tree ----------
    //
    // Built fresh from the flat node list on every render pass. The list is
    // ~100 rows; memoising it would be more code than it saves, and a stale
    // tree after a create is a worse bug than a cheap rebuild.
    get roots() {
        const byId = {};
        const term = (this.state.filter || '').trim().toLowerCase();
        for (const n of this.state.nodes)
            byId[n.id] = Object.assign({}, n, { kids: [], depth: 0 });

        const roots = [];
        for (const id in byId) {
            const n = byId[id];
            const p = n.parent_id && byId[n.parent_id];
            if (p) p.kids.push(n); else roots.push(n);
        }
        const depth = (n, d) => { n.depth = d; n.kids.forEach(k => depth(k, d + 1)); };
        roots.forEach(r => depth(r, 0));

        if (!term) return roots;

        // Filtering keeps a node if it matches OR any descendant does —
        // otherwise typing a leaf's name hides the branch that leads to it and
        // the result looks empty. Matching branches are force-opened.
        const keep = (n) => {
            const hit = n.name.toLowerCase().includes(term);
            n.kids = n.kids.filter(keep);
            if (hit || n.kids.length) { this.state.open[n.id] = true; return true; }
            return false;
        };
        return roots.filter(keep);
    }

    // The tree flattened to the rows that are actually on screen: a node is
    // included only if every ancestor above it is open. This is what the
    // template iterates, so "what is rendered" and "what is expanded" cannot
    // drift apart.
    get visibleRows() {
        const out = [];
        const walk = (list) => {
            for (const n of list) {
                out.push({
                    id: n.id, name: n.name, depth: n.depth, active: n.active,
                    direct_count: n.direct_count, total_count: n.total_count,
                    hasKids: n.kids.length > 0,
                });
                if (this.state.open[n.id]) walk(n.kids);
            }
        };
        walk(this.roots);
        return out;
    }

    get visibleCount() { return this.visibleRows.length; }

    toggle(id)     { this.state.open[id] = !this.state.open[id]; }
    expandAll()    { for (const n of this.state.nodes) this.state.open[n.id] = true; }
    collapseAll()  { this.state.open = {}; }
    onFilter(ev)   { this.state.filter = ev.target.value; }
    async toggleArchived(ev) {
        this.state.showArchived = ev.target.checked;
        await this.load(true);
    }

    nameOf(id) {
        const n = this.state.nodes.find(x => x.id === id);
        return n ? n.name : '';
    }

    // ---------- writes ----------
    startCreate(parentId) {
        this.state.creating = true;
        this.state.createParent = parentId || 0;
        this.state.createName = '';
        this.state.error = '';
    }
    onCreateKey(ev) { if (ev.key === 'Enter') this.doCreate(); if (ev.key === 'Escape') this.state.creating = false; }

    async doCreate() {
        const name = (this.state.createName || '').trim();
        if (!name) { this.state.error = 'A category needs a name.'; return; }
        try {
            const vals = { name };
            if (this.state.createParent) vals.parent_id = this.state.createParent;
            const id = await RpcService.call('product.category', 'create', [vals], {});
            this.state.creating = false;
            if (this.state.createParent) this.state.open[this.state.createParent] = true;
            await this.load();
            if (typeof id === 'number') await this.select(id);
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
    }

    startRename() {
        this.state.renaming = true;
        this.state.renameName = this.state.detail ? this.state.detail.name : '';
        this.state.error = '';
    }
    onRenameKey(ev) { if (ev.key === 'Enter') this.doRename(); if (ev.key === 'Escape') this.state.renaming = false; }

    async doRename() {
        const name = (this.state.renameName || '').trim();
        if (!name) { this.state.error = 'A category needs a name.'; return; }
        try {
            await RpcService.call('product.category', 'write',
                [[this.state.detail.id], { name }], {});
            this.state.renaming = false;
            await this.load();
            await this.loadDetail(this.state.detail.id);
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
    }

    async doDelete() {
        const d = this.state.detail;
        if (!d) return;
        // Say what will actually happen, with the numbers. "Are you sure?" is
        // not a question anyone can answer.
        const warn = d.counts.children
            ? `"${d.name}" has ${d.counts.children} sub-categor${d.counts.children === 1 ? 'y' : 'ies'} and ${d.counts.total} product(s) beneath it.\n\nDelete it?`
            : `Delete "${d.name}"? ${d.counts.direct} product(s) are filed in it.`;
        if (!window.confirm(warn)) return;
        try {
            await RpcService.call('product.category', 'unlink', [[d.id]], {});
            this.state.detail = null;
            this.state.selected = 0;
            await this.load();
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
    }

    // ---------- the splitter ----------
    savedWidth() {
        const v = parseInt(window.localStorage.getItem('ct.sideWidth') || '', 10);
        return (v >= 180 && v <= 720) ? v : 300;
    }
    startResize(ev) {
        ev.preventDefault();
        this._x0 = ev.clientX;
        this._w0 = this.state.sideWidth;
        document.addEventListener('mousemove', this._onMove);
        document.addEventListener('mouseup', this._onUp);
        document.body.classList.add('ct-resizing');
    }
    onResize(ev) {
        const w = Math.min(720, Math.max(180, this._w0 + (ev.clientX - this._x0)));
        this.state.sideWidth = w;
    }
    endResize() {
        document.removeEventListener('mousemove', this._onMove);
        document.removeEventListener('mouseup', this._onUp);
        document.body.classList.remove('ct-resizing');
        window.localStorage.setItem('ct.sideWidth', String(this.state.sideWidth));
    }

    // ---------- formatting ----------
    accName(k) {
        const a = this.state.detail && this.state.detail.accounts && this.state.detail.accounts[k];
        return a && a.name ? a.name : 'Not set';
    }
    money(v) {
        const n = Number(v || 0);
        return n.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
    }
    shortDate(s) { return String(s || '').slice(0, 10); }
}
