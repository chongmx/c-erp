/**
 * PartCatalog.js — Products → Parts Catalogue (docs/098).
 *
 * A distributor-style faceted browser: the attribute strip on top, the result
 * table below. The two halves are driven by one filter object and two calls to
 * part.catalog (`facets` and `search`) that share a filter parser on the server,
 * so the count in the header is always the count of the rows underneath it.
 *
 * Scrolling is the layout's whole problem. There are more attributes than fit
 * across a screen and more values than fit down one, so the strip scrolls
 * horizontally while each facet's value list scrolls vertically inside its own
 * box, and the table scrolls in both directions independently of either. Nothing
 * is allowed to scroll the page itself — that is what makes a catalogue with 40
 * attributes and 100k rows still feel like one screen.
 *
 * Enum values apply on click; numeric ranges apply on Enter or via Apply,
 * because a half-typed "1" in a min box should not narrow the world to nothing
 * before you have typed the "k".
 */
class PartCatalog extends owl.Component {
    static template = owl.xml`
        <div class="pc-shell">

            <div class="pc-head">
                <div class="pc-title-row">
                    <h2 class="pc-title" t-esc="currentCategoryName"/>
                    <span class="pc-count">
                        Results: <b t-esc="fmtInt(state.total)"/>
                    </span>
                    <div class="pc-spacer"/>
                    <span class="pc-layout-lbl">Filter layout:</span>
                    <button class="pc-toggle" t-att-class="{active: state.layout === 'stacked'}"
                            t-on-click="() => this.setLayout('stacked')">Stacked</button>
                    <button class="pc-toggle" t-att-class="{active: state.layout === 'scrolling'}"
                            t-on-click="() => this.setLayout('scrolling')">Scrolling</button>
                </div>

                <div class="pc-controls">
                    <input class="pc-search" t-model="state.qInput" placeholder="Search within results…"
                           t-on-keydown="onSearchKey"/>
                    <button class="pc-btn" t-on-click="applySearch">Search</button>
                    <select class="pc-categ" t-on-change="onCategory">
                        <option value="0">All categories</option>
                        <t t-foreach="state.categories" t-as="c" t-key="c.id">
                            <option t-att-value="c.id" t-att-selected="c.id === state.categId"
                                    t-esc="indent(c) + ' (' + c.n + ')'"/>
                        </t>
                    </select>
                    <label class="pc-check">
                        <input type="checkbox" t-att-checked="state.inStock" t-on-change="toggleStock"/>
                        In stock only
                    </label>
                </div>

                <div class="pc-chips" t-if="chips.length">
                    <t t-foreach="chips" t-as="ch" t-key="ch.id">
                        <button class="pc-chip" t-on-click="() => this.removeChip(ch)">
                            <span class="pc-chip-k" t-esc="ch.label"/>
                            <span t-esc="ch.text"/>
                            <span class="pc-chip-x">×</span>
                        </button>
                    </t>
                    <button class="pc-btn ghost" t-on-click="resetAll">Reset all</button>
                </div>
            </div>

            <t t-if="state.error"><div class="pc-error" t-esc="state.error"/></t>

            <div class="pc-facets" t-att-class="'lay-' + state.layout">
                <t t-foreach="state.facets" t-as="f" t-key="f.key">
                    <div class="pc-facet">
                        <div class="pc-facet-h">
                            <span t-esc="f.label"/>
                            <span class="pc-facet-n" t-if="f.kind === 'enum'" t-esc="f.values.length"/>
                        </div>

                        <t t-if="f.kind === 'enum'">
                            <input class="pc-facet-q" placeholder="Filter…"
                                   t-att-value="state.facetQ[f.key] || ''"
                                   t-on-input="(ev) => this.onFacetQ(f.key, ev)"/>
                            <div class="pc-facet-list">
                                <t t-foreach="visibleValues(f)" t-as="v" t-key="v.v">
                                    <label class="pc-val" t-att-class="{on: isPicked(f.key, v.v)}">
                                        <input type="checkbox" t-att-checked="isPicked(f.key, v.v)"
                                               t-on-change="() => this.toggleEnum(f.key, v.v)"/>
                                        <span class="pc-val-t" t-esc="v.v"/>
                                        <span class="pc-val-n" t-esc="fmtInt(v.n)"/>
                                    </label>
                                </t>
                                <div class="pc-facet-empty" t-if="!visibleValues(f).length">No match</div>
                            </div>
                        </t>

                        <t t-else="">
                            <div class="pc-range">
                                <input class="pc-range-in" placeholder="Min"
                                       t-att-value="rangeVal(f.key, 'min')"
                                       t-on-input="(ev) => this.onRange(f.key, 'min', ev)"
                                       t-on-keydown="onRangeKey"/>
                                <input class="pc-range-in" placeholder="Max"
                                       t-att-value="rangeVal(f.key, 'max')"
                                       t-on-input="(ev) => this.onRange(f.key, 'max', ev)"
                                       t-on-keydown="onRangeKey"/>
                                <select class="pc-range-u" t-on-change="(ev) => this.onUnit(f.key, ev)">
                                    <t t-foreach="f.units" t-as="u" t-key="u.symbol">
                                        <option t-att-value="u.symbol"
                                                t-att-selected="u.symbol === rangeUnit(f)"
                                                t-esc="u.symbol"/>
                                    </t>
                                </select>
                            </div>
                            <div class="pc-range-span">
                                <t t-esc="spanText(f)"/>
                            </div>
                            <button class="pc-btn tiny" t-on-click="reload">Apply</button>
                        </t>
                    </div>
                </t>
                <div class="pc-facet-none" t-if="!state.facets.length and !state.loading">
                    No attributes to filter on. Seed a catalogue with
                    <code>./scripts/seed_demo_parts.sh</code>.
                </div>
            </div>

            <div class="pc-toolbar">
                <span class="pc-showing">
                    Showing <b t-esc="showingFrom"/>–<b t-esc="showingTo"/> of <b t-esc="fmtInt(state.total)"/>
                </span>
                <label class="pc-sortlbl">Per page
                    <select t-on-change="onLimit">
                        <t t-foreach="[25, 50, 100]" t-as="n" t-key="n">
                            <option t-att-value="n" t-att-selected="n === state.limit" t-esc="n"/>
                        </t>
                    </select>
                </label>
                <div class="pc-spacer"/>
                <button class="pc-btn" t-att-disabled="state.offset &lt;= 0" t-on-click="() => this.page(-1)">‹ Prev</button>
                <button class="pc-btn" t-att-disabled="state.offset + state.limit >= state.total"
                        t-on-click="() => this.page(1)">Next ›</button>
            </div>

            <div class="pc-table-wrap">
                <table class="pc-table">
                    <thead>
                        <tr>
                            <t t-foreach="columns" t-as="col" t-key="col.key">
                                <th t-att-class="col.cls" t-att-style="col.w ? ('min-width:' + col.w) : ''">
                                    <t t-if="col.sort">
                                        <button class="pc-sort" t-on-click="() => this.setSort(col.sort)">
                                            <span t-esc="col.label"/>
                                            <span class="pc-caret" t-if="state.sort === col.sort"
                                                  t-esc="state.dir === 'asc' ? '▲' : '▼'"/>
                                        </button>
                                    </t>
                                    <t t-else=""><span t-esc="col.label"/></t>
                                </th>
                            </t>
                        </tr>
                    </thead>
                    <tbody>
                        <t t-foreach="state.rows" t-as="r" t-key="r.id">
                            <tr class="pc-row" t-on-click="() => this.openProduct(r.id)">
                                <td class="pc-mpn">
                                    <div class="pc-mpn-t" t-esc="r.mpn || r.code || '—'"/>
                                    <div class="pc-code" t-esc="r.code"/>
                                </td>
                                <td t-esc="r.manufacturer || '—'"/>
                                <td class="pc-desc" t-esc="r.name"/>
                                <td class="pc-pkg" t-esc="r.package || '—'"/>
                                <td class="pc-num">
                                    <span t-att-class="r.qty_available > 0 ? 'pc-stock ok' : 'pc-stock out'"
                                          t-esc="r.qty_available > 0 ? fmtInt(r.qty_available) + ' in stock' : 'No stock'"/>
                                </td>
                                <td class="pc-num" t-esc="fmtPrice(r.list_price)"/>
                                <t t-foreach="paramCols" t-as="pcol" t-key="pcol">
                                    <td class="pc-param" t-esc="paramOf(r, pcol)"/>
                                </t>
                                <td class="pc-desc pc-dim" t-esc="r.categ"/>
                            </tr>
                        </t>
                        <tr t-if="!state.rows.length and !state.loading">
                            <td t-att-colspan="columns.length" class="pc-empty">
                                No parts match these filters.
                            </td>
                        </tr>
                    </tbody>
                </table>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({
            categId: 0, categories: [],
            q: '', qInput: '',
            inStock: false,
            enums: {}, ranges: {}, facetQ: {},
            facets: [], total: 0,
            rows: [], limit: 25, offset: 0,
            sort: 'name', dir: 'asc',
            layout: 'scrolling',
            loading: false, error: '',
        });
        owl.onWillStart(async () => {
            try {
                this.state.categories = await RpcService.call('part.catalog', 'categories', [{}], {}) || [];
            } catch (e) { /* the picker is a convenience; the browser works without it */ }
            await this.reload();
        });
    }

    // ---- filter payload ----------------------------------------------------
    // One object, sent to both endpoints, so the strip and the table can never
    // describe different result sets.
    get filter() {
        const f = {};
        if (this.state.categId) f.categ_id = this.state.categId;
        if (this.state.q)       f.q = this.state.q;
        if (this.state.inStock) f.in_stock = true;

        const en = {};
        for (const [k, vs] of Object.entries(this.state.enums))
            if (vs && vs.length) en[k] = vs.slice();
        if (Object.keys(en).length) f.enum = en;

        const rg = {};
        for (const [k, r] of Object.entries(this.state.ranges)) {
            const has = (r.min !== '' && r.min != null) || (r.max !== '' && r.max != null);
            if (!has) continue;
            const one = {};
            if (r.min !== '' && r.min != null) one.min = r.min;
            if (r.max !== '' && r.max != null) one.max = r.max;
            if (r.unit) one.unit = r.unit;
            rg[k] = one;
        }
        if (Object.keys(rg).length) f.range = rg;
        return f;
    }

    async reload(keepOffset) {
        if (!keepOffset) this.state.offset = 0;
        this.state.loading = true;
        this.state.error = '';
        const f = this.filter;
        try {
            // Both halves of the screen come from the same filter, in parallel.
            const [facets, page] = await Promise.all([
                RpcService.call('part.catalog', 'facets', [f], {}),
                RpcService.call('part.catalog', 'search', [Object.assign({}, f, {
                    limit: this.state.limit, offset: this.state.offset,
                    sort: this.state.sort, dir: this.state.dir,
                })], {}),
            ]);
            this.state.facets = (facets && facets.facets) || [];
            this.state.total  = (page && page.total) || 0;
            this.state.rows   = (page && page.rows) || [];
            // Seed each range facet's unit from the server's own list so the
            // select always shows something valid.
            for (const fa of this.state.facets) {
                if (fa.kind !== 'range') continue;
                if (!this.state.ranges[fa.key])
                    this.state.ranges[fa.key] = { min: '', max: '', unit: this.defaultUnit(fa) };
                else if (!this.state.ranges[fa.key].unit)
                    this.state.ranges[fa.key].unit = this.defaultUnit(fa);
            }
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load the catalogue.';
            this.state.facets = []; this.state.rows = []; this.state.total = 0;
        } finally {
            this.state.loading = false;
        }
    }

    // Pick the unit whose scale suits the data, not always the SI base: a
    // catalogue of 4.7 kΩ parts should not open showing "4700".
    defaultUnit(f) {
        const units = f.units || [];
        if (!units.length) return '';
        const hi = f.max || 0;
        let best = units[0];
        for (const u of units) {
            if (!u.factor) continue;
            const scaled = hi / u.factor;
            if (scaled >= 1 && scaled < 1000) best = u;
        }
        return best.symbol;
    }

    // ---- facet interaction -------------------------------------------------
    isPicked(key, v) { return (this.state.enums[key] || []).includes(v); }

    async toggleEnum(key, v) {
        const cur = this.state.enums[key] || [];
        this.state.enums[key] = cur.includes(v) ? cur.filter(x => x !== v) : cur.concat([v]);
        if (!this.state.enums[key].length) delete this.state.enums[key];
        await this.reload();
    }

    rangeVal(key, which) { const r = this.state.ranges[key]; return (r && r[which]) || ''; }
    rangeUnit(f) { const r = this.state.ranges[f.key]; return (r && r.unit) || this.defaultUnit(f); }

    onRange(key, which, ev) {
        if (!this.state.ranges[key]) this.state.ranges[key] = { min: '', max: '', unit: '' };
        this.state.ranges[key][which] = ev.target.value;
    }
    onRangeKey(ev) { if (ev.key === 'Enter') this.reload(); }
    async onUnit(key, ev) {
        if (!this.state.ranges[key]) this.state.ranges[key] = { min: '', max: '', unit: '' };
        this.state.ranges[key].unit = ev.target.value;
        await this.reload();
    }

    onFacetQ(key, ev) { this.state.facetQ[key] = ev.target.value; }

    visibleValues(f) {
        const q = (this.state.facetQ[f.key] || '').trim().toLowerCase();
        const vals = f.values || [];
        return q ? vals.filter(v => String(v.v).toLowerCase().includes(q)) : vals;
    }

    spanText(f) {
        const u = (f.units || []).find(x => x.symbol === this.rangeUnit(f));
        const div = (u && u.factor) || 1;
        const lo = this.fmtNum((f.min || 0) / div), hi = this.fmtNum((f.max || 0) / div);
        return lo === hi ? (lo + ' ' + (u ? u.symbol : '')) : (lo + ' – ' + hi + ' ' + (u ? u.symbol : ''));
    }

    // ---- top controls ------------------------------------------------------
    onSearchKey(ev) { if (ev.key === 'Enter') this.applySearch(); }
    async applySearch() { this.state.q = (this.state.qInput || '').trim(); await this.reload(); }
    async onCategory(ev)  { this.state.categId = parseInt(ev.target.value, 10) || 0; await this.reload(); }
    async toggleStock(ev) { this.state.inStock = !!ev.target.checked; await this.reload(); }
    setLayout(l) { this.state.layout = l; }

    async resetAll() {
        this.state.enums = {}; this.state.facetQ = {};
        for (const k of Object.keys(this.state.ranges)) {
            this.state.ranges[k].min = '';
            this.state.ranges[k].max = '';
        }
        this.state.q = ''; this.state.qInput = '';
        this.state.inStock = false;
        this.state.categId = 0;
        await this.reload();
    }

    // Applied filters as removable chips — the only way to see everything that
    // is currently narrowing the list without scrolling the whole strip.
    get chips() {
        const out = [];
        if (this.state.q) out.push({ id: 'q', label: 'Search', text: this.state.q, kind: 'q' });
        if (this.state.inStock) out.push({ id: 'stock', label: '', text: 'In stock only', kind: 'stock' });
        for (const [k, vs] of Object.entries(this.state.enums))
            for (const v of vs)
                out.push({ id: k + '=' + v, label: this.labelOf(k), text: v, kind: 'enum', key: k, v });
        for (const [k, r] of Object.entries(this.state.ranges)) {
            const lo = r.min, hi = r.max;
            if ((lo === '' || lo == null) && (hi === '' || hi == null)) continue;
            const txt = (lo !== '' && lo != null ? lo : '…') + ' – ' +
                        (hi !== '' && hi != null ? hi : '…') + ' ' + (r.unit || '');
            out.push({ id: k + '~', label: this.labelOf(k), text: txt.trim(), kind: 'range', key: k });
        }
        return out;
    }

    labelOf(key) {
        const f = this.state.facets.find(x => x.key === key);
        return f ? f.label : key.replace(/^param:/, '');
    }

    async removeChip(ch) {
        if (ch.kind === 'q')     { this.state.q = ''; this.state.qInput = ''; }
        if (ch.kind === 'stock') { this.state.inStock = false; }
        if (ch.kind === 'enum')  { await this.toggleEnum(ch.key, ch.v); return; }
        if (ch.kind === 'range') {
            this.state.ranges[ch.key].min = '';
            this.state.ranges[ch.key].max = '';
        }
        await this.reload();
    }

    // ---- table -------------------------------------------------------------
    // Parameters shared by most of the page become their own columns; the rest
    // stay in the row's name. Which ones those are depends on the result set,
    // so the header is derived per page rather than fixed.
    get paramCols() {
        const seen = new Map();
        for (const r of this.state.rows)
            for (const p of (r.params || []))
                seen.set(p.name, (seen.get(p.name) || 0) + 1);
        const need = Math.max(1, this.state.rows.length * 0.5);
        return [...seen.entries()].filter(([, n]) => n >= need).map(([k]) => k).slice(0, 8);
    }

    paramOf(row, name) {
        const p = (row.params || []).find(x => x.name === name);
        return p ? p.value : '';
    }

    get columns() {
        const base = [
            { key: 'mpn',   label: 'MPN',          sort: 'code',  w: '170px' },
            { key: 'mfr',   label: 'Manufacturer', sort: null,    w: '140px' },
            { key: 'name',  label: 'Description',  sort: 'name',  w: '260px' },
            { key: 'pkg',   label: 'Package',      sort: null,    w: '90px'  },
            { key: 'stock', label: 'Availability', sort: 'stock', w: '120px', cls: 'pc-num' },
            { key: 'price', label: 'Price',        sort: 'price', w: '90px',  cls: 'pc-num' },
        ];
        for (const p of this.paramCols) base.push({ key: 'p:' + p, label: p, sort: null, w: '110px' });
        base.push({ key: 'categ', label: 'Category', sort: null, w: '150px' });
        return base;
    }

    async setSort(key) {
        if (!key) return;
        if (this.state.sort === key) this.state.dir = this.state.dir === 'asc' ? 'desc' : 'asc';
        else { this.state.sort = key; this.state.dir = 'asc'; }
        await this.reload();
    }

    async onLimit(ev) { this.state.limit = parseInt(ev.target.value, 10) || 25; await this.reload(); }

    async page(d) {
        const next = this.state.offset + d * this.state.limit;
        if (next < 0 || next >= this.state.total) return;
        this.state.offset = next;
        await this.reload(true);
    }

    get showingFrom() { return this.state.total ? this.state.offset + 1 : 0; }
    get showingTo()   { return Math.min(this.state.offset + this.state.limit, this.state.total); }

    get currentCategoryName() {
        if (!this.state.categId) return 'Parts Catalogue';
        const c = this.state.categories.find(x => x.id === this.state.categId);
        return c ? c.path.split(' / ').pop() : 'Parts Catalogue';
    }

    indent(c) { return '  '.repeat(Math.max(0, (c.depth || 1) - 1)) + c.path.split(' / ').pop(); }

    // ---- formatting --------------------------------------------------------
    fmtInt(n) { return (Math.round(n || 0)).toLocaleString('en-US'); }
    fmtPrice(n) {
        const v = Number(n || 0);
        // Component prices run to fractions of a cent, so 2dp would render a
        // whole catalogue as "$0.00" — but padding every price to 5dp is just
        // as unreadable. Show only the digits that carry information.
        if (!v) return '$0.00';
        if (v >= 0.01) return '$' + v.toFixed(2);
        return '$' + v.toFixed(6).replace(/0+$/, '').replace(/\.$/, '.0');
    }
    fmtNum(v) {
        const n = Number(v || 0);
        if (!n) return '0';
        if (Math.abs(n) >= 1000 || Math.abs(n) < 0.001) return n.toPrecision(3).replace(/\.?0+e/, 'e');
        return String(parseFloat(n.toPrecision(4)));
    }

    /**
     * Open a record in the shell. Falls back to a warning rather than pretending
     * it worked — the previous `location.hash` version failed silently because
     * this app has no hash router at all.
     */
    openRecord(model, id) {
        if (window.ErpNav && window.ErpNav.openRecord) return window.ErpNav.openRecord(model, id);
        console.warn('Cannot navigate: the shell is not mounted.');
        return false;
    }

    openProduct(id) { return this.openRecord('product.product', id); }
}
