/**
 * DbStudio.js — Settings ▸ Database Tools (docs/093).
 *
 * A database browser for the company's own database, in three tabs:
 *
 *   Browse  — every table, its rows, its columns, its keys and its indexes;
 *             filter and sort the data; follow a foreign key by clicking it.
 *   SQL     — a read-only console.
 *   Schema  — the shape of the database: size and row counts, storage by
 *             module, and a map of how the tables reference each other.
 *
 * Everything here is READ-ONLY, and not because this file is careful: the
 * server runs every one of these calls inside a PostgreSQL READ ONLY
 * transaction that is never committed. There is no write path to reach.
 *
 * Two OWL details this file obeys throughout:
 *   - templates cannot see JS globals, so every bit of formatting (Math,
 *     Number, JSON) lives in a component method;
 *   - module colour is assigned from a FIXED alphabetical order of module
 *     names, never from position in a sorted-by-size list — so `account`
 *     is the same blue in the sidebar, the bars and the schema map, and
 *     filtering the list never repaints anything.
 */

// The eight module hues, mirroring --m0..--m7 in dbstudio.css. A ninth module
// gets the neutral rather than a ninth hue that would fail the contrast and
// colour-vision checks the other eight passed.
const DBS_HUES = ['#6ea8fe', '#f0a868', '#4ec9b0', '#e94560',
                  '#b48ead', '#7fd3f7', '#e8d44d', '#d98880'];
const DBS_OTHER = '#7a8ba3';

/** Stable module → colour. Alphabetical, so it never depends on what is shown. */
function dbsColorFor(module, allModules) {
    const i = allModules.indexOf(module);
    return i >= 0 && i < DBS_HUES.length ? DBS_HUES[i] : DBS_OTHER;
}

// ============================================================
// DbSchemaMap — the foreign-key map, drawn as SVG
// ============================================================
/**
 * Two views, because one drawing cannot serve both questions.
 *
 *   "modules" answers *how is this database organised* — 100+ tables laid out
 *   at once is a hairball nobody reads, so tables are rolled up into their
 *   module and the arcs carry the FK counts between modules.
 *
 *   "focus" answers *what touches this table* — one table in the middle, the
 *   tables it points at on the right, the tables that point at it on the left.
 *   Direction is therefore in the layout, not only in the arrowheads.
 */
class DbSchemaMap extends owl.Component {
    static template = owl.xml`
        <div class="dbs-mapwrap">
            <t t-if="!layout.nodes.length">
                <div class="dbs-map-empty">Nothing to draw here.</div>
            </t>
            <svg t-else="" class="dbs-map" t-att-viewBox="layout.viewBox"
                 preserveAspectRatio="xMidYMid meet" role="img"
                 t-att-aria-label="layout.aria">
                <defs>
                    <marker id="dbs-arrow" viewBox="0 0 10 10" refX="9" refY="5"
                            markerWidth="6" markerHeight="6" orient="auto-start-reverse">
                        <path d="M 0 0 L 10 5 L 0 10 z" fill="#3a4f7a"/>
                    </marker>
                </defs>

                <g>
                    <t t-foreach="layout.edges" t-as="e" t-key="e.key">
                        <path class="dbs-edge" t-att-d="e.d" t-att-stroke-width="e.width"
                              marker-end="url(#dbs-arrow)"/>
                        <text t-if="e.label" class="dbs-edge-label"
                              t-att-x="e.lx" t-att-y="e.ly" text-anchor="middle"
                              t-esc="e.label"/>
                    </t>
                </g>

                <g>
                    <t t-foreach="layout.nodes" t-as="n" t-key="n.id">
                        <g class="dbs-node" t-on-click="() => this.pick(n.id)">
                            <title t-esc="n.title"/>
                            <circle t-if="n.circle" class="dbs-node-shape"
                                    t-att-cx="n.cx" t-att-cy="n.cy" t-att-r="n.r"
                                    t-att-fill="n.fill" t-att-stroke="n.stroke" stroke-width="1.5"/>
                            <rect t-else="" class="dbs-node-shape" t-att-x="n.x" t-att-y="n.y"
                                  t-att-width="n.w" t-att-height="n.h" rx="6"
                                  t-att-fill="n.fill" t-att-stroke="n.stroke"
                                  t-att-stroke-width="n.center ? 2 : 1"/>
                            <text t-if="n.transform" class="dbs-node-label"
                                  t-att-transform="n.transform" text-anchor="end" t-esc="n.label"/>
                            <t t-else="">
                                <text class="dbs-node-label" t-att-x="n.cx" t-att-y="n.ty"
                                      text-anchor="middle" t-esc="n.label"/>
                                <text class="dbs-node-sub" t-att-x="n.cx" t-att-y="n.sy"
                                      text-anchor="middle" t-esc="n.sub"/>
                            </t>
                        </g>
                    </t>
                </g>
            </svg>
        </div>
    `;

    static props = ['graph', 'mode', 'focus', 'modules', 'onPick'];

    pick(id) { if (this.props.onPick) this.props.onPick(id); }

    /**
     * The template reads layout.nodes, layout.edges and layout.viewBox
     * separately, so an un-cached getter would lay the whole graph out several
     * times per render. Cache on the inputs that actually change it.
     */
    get layout() {
        const key = this.props.mode + '|' + (this.props.focus || '') + '|' +
                    ((this.props.graph && this.props.graph.edges.length) || 0);
        if (this._key !== key) {
            this._key = key;
            this._layout = this.props.mode === 'focus' ? this.focusLayout() : this.moduleLayout();
        }
        return this._layout;
    }

    // ---- shared helpers ----
    clip(s, n) { return s.length > n ? s.slice(0, n - 1) + '…' : s; }
    colour(mod) { return dbsColorFor(mod, this.props.modules || []); }
    /** A translucent version of a module hue, for node fills on the dark ground. */
    wash(hex) { return hex + '2e'; }

    moduleFor(id) {
        const n = (this.props.graph.nodes || []).find(x => x.id === id);
        return n ? n.module : '';
    }

    // ---- module roll-up ----
    moduleLayout() {
        const g = this.props.graph || { nodes: [], edges: [] };
        const mods = new Map();
        for (const n of g.nodes) {
            if (!mods.has(n.module)) mods.set(n.module, { id: n.module, tables: 0, rows: 0, self: 0 });
            const m = mods.get(n.module);
            m.tables += 1;
            m.rows += n.rows || 0;
        }
        const pairs = new Map();
        for (const e of g.edges) {
            const a = this.moduleFor(e.source), b = this.moduleFor(e.target);
            if (!a || !b) continue;
            if (a === b) { if (mods.has(a)) mods.get(a).self += 1; continue; }
            const k = a + '>' + b;
            pairs.set(k, { a, b, n: ((pairs.get(k) || {}).n || 0) + 1 });
        }

        // Biggest modules to the left: that is where most arcs start, so the
        // busy end of the diagram is the end the eye reaches first.
        const list = [...mods.values()].sort((x, y) => y.tables - x.tables || x.id.localeCompare(y.id));
        if (!list.length) return { nodes: [], edges: [], viewBox: '0 0 10 10', aria: '' };

        const count = list.length;
        const pad = 66, gap = 64;
        const W = Math.max(980, pad * 2 + Math.max(1, count - 1) * gap);
        const axisY = 330, H = 470, maxRy = 266;
        const step = count > 1 ? (W - pad * 2) / (count - 1) : 0;
        const pos = new Map();
        const nodes = list.map((m, i) => {
            const cx = count > 1 ? pad + step * i : W / 2;
            const r = 6 + Math.sqrt(m.tables) * 3.4;
            pos.set(m.id, cx);
            const hue = this.colour(m.id);
            return {
                id: m.id, circle: true, cx, cy: axisY, r,
                fill: this.wash(hue), stroke: hue, center: false,
                // Rotated, so twenty labels sit side by side without colliding.
                transform: `translate(${cx - 4} ${axisY + r + 12}) rotate(-42)`,
                label: this.clip(m.id, 14) + (m.self ? ' ↻' + m.self : ''),
                sub: '',
                title: `${m.id} - ${m.tables} ${m.tables === 1 ? 'table' : 'tables'}, `
                       + `${this.fmt(m.rows)} rows`
                       + (m.self ? `, ${m.self} foreign keys inside the module` : ''),
            };
        });

        const maxN = Math.max(...[...pairs.values()].map(p => p.n), 1);
        const edges = [...pairs.entries()].map(([k, p]) => {
            const fks = p.n;
            const x1 = pos.get(p.a), x2 = pos.get(p.b);
            if (x1 === undefined || x2 === undefined) return null;
            const dx = Math.abs(x2 - x1);
            const ry = Math.min(dx / 2, maxRy);
            // Every arc bows UPWARD, into the empty half of the box, leaving the
            // area under the axis free for the rotated labels. Y grows downward
            // in SVG, so sweeping from the left point to the right one clockwise
            // (flag 1) is what rises; a right-to-left arc needs the flag flipped
            // to rise the same way. Direction is carried by the arrowhead, not
            // by which side of the axis the arc sits on.
            return {
                key: k,
                d: `M ${x1} ${axisY} A ${dx / 2} ${ry} 0 0 ${x2 > x1 ? 1 : 0} ${x2} ${axisY}`,
                width: 1 + (fks / maxN) * 4.5,
                label: fks >= 5 ? String(fks) : '',
                lx: (x1 + x2) / 2, ly: axisY - ry - 4,
            };
        }).filter(Boolean);

        return {
            nodes, edges, viewBox: `0 0 ${W} ${H}`,
            aria: `Foreign-key map across ${list.length} modules`,
        };
    }

    // ---- one table and its neighbours ----
    focusLayout() {
        const g = this.props.graph || { nodes: [], edges: [] };
        const id = this.props.focus;
        if (!id) return { nodes: [], edges: [], viewBox: '0 0 10 10', aria: '' };

        const outs = [...new Set(g.edges.filter(e => e.source === id).map(e => e.target))].filter(t => t !== id);
        const ins  = [...new Set(g.edges.filter(e => e.target === id).map(e => e.source))].filter(s => s !== id);
        const CAP = 12;
        const outsShown = outs.slice(0, CAP), insShown = ins.slice(0, CAP);

        const W = 940, H = Math.max(420, 110 + Math.max(outsShown.length, insShown.length) * 44);
        const cx = W / 2, cy = H / 2, w = 178, h = 42;

        const colFor = (t) => this.colour(this.moduleFor(t));
        const nodes = [];
        const centreHue = colFor(id);
        nodes.push({
            id, x: cx - w / 2, y: cy - h / 2, w, h, cx, ty: cy - 3, sy: cy + 12,
            fill: this.wash(centreHue), stroke: centreHue, center: true,
            label: this.clip(id, 24), sub: 'this table',
            title: id,
        });

        const place = (list, side) => {
            const n = list.length;
            const x = side === 'right' ? W - w - 30 : 30;
            const span = n > 1 ? Math.min(H - 90, n * 44) : 0;
            const top = cy - span / 2;
            return list.map((t, i) => {
                const y = (n === 1 ? cy : top + (span / (n - 1)) * i) - h / 2;
                const hue = colFor(t);
                const cols = g.edges
                    .filter(e => side === 'right' ? (e.source === id && e.target === t)
                                                  : (e.source === t && e.target === id))
                    .map(e => e.source_column);
                return {
                    node: {
                        id: t, x, y, w, h, cx: x + w / 2, ty: y + 18, sy: y + 32,
                        fill: this.wash(hue), stroke: hue, center: false,
                        label: this.clip(t, 24),
                        sub: this.clip(cols.join(', '), 26),
                        title: side === 'right'
                            ? id + ' → ' + t + ' via ' + cols.join(', ')
                            : t + ' → ' + id + ' via ' + cols.join(', '),
                    },
                    edge: side === 'right'
                        ? { key: 'o' + t, d: `M ${cx + w / 2} ${cy} C ${cx + w / 2 + 70} ${cy}, ${x - 70} ${y + h / 2}, ${x} ${y + h / 2}`, width: 1.5, label: '', lx: 0, ly: 0 }
                        : { key: 'i' + t, d: `M ${x + w} ${y + h / 2} C ${x + w + 70} ${y + h / 2}, ${cx - w / 2 - 70} ${cy}, ${cx - w / 2} ${cy}`, width: 1.5, label: '', lx: 0, ly: 0 },
                };
            });
        };

        const right = place(outsShown, 'right');
        const left  = place(insShown, 'left');
        for (const r of [...right, ...left]) nodes.push(r.node);
        const edges = [...right, ...left].map(r => r.edge);

        // Column headings, drawn as nodes with no box so the sides read as sides.
        const heading = (text, x, count, total) => ({
            id: '__h' + x, x, y: 8, w: w, h: 0, cx: x + w / 2, ty: 22, sy: 36,
            fill: 'none', stroke: 'none', center: false,
            label: text, sub: count < total ? `${count} of ${total} shown` : (total === 0 ? 'none' : ''),
            title: text,
        });
        nodes.push(heading('references →', W - w - 30, outsShown.length, outs.length));
        nodes.push(heading('← referenced by', 30, insShown.length, ins.length));

        return {
            nodes, edges, viewBox: `0 0 ${W} ${H}`,
            aria: `${id} references ${outs.length} tables and is referenced by ${ins.length}`,
        };
    }

    fmt(n) { return (n || 0).toLocaleString(); }
}

// ============================================================
// DbStudio — the screen
// ============================================================
class DbStudio extends owl.Component {
    static components = { DbSchemaMap };

    static template = owl.xml`
    <div class="db-studio">
        <div class="dbs-head">
            <h2 class="dbs-title">Database Tools</h2>
            <div class="dbs-meta" t-if="state.overview">
                <span><b t-esc="state.overview.database"/></span>
                <span>PostgreSQL <b t-esc="state.overview.version"/></span>
                <span><b t-esc="state.overview.size_human"/></span>
                <span><b t-esc="fmt(state.overview.table_count)"/> tables</span>
            </div>
            <span class="dbs-ro" title="Every query runs in a PostgreSQL READ ONLY transaction that is never committed.">Read-only</span>
        </div>

        <div class="dbs-tabs">
            <button t-attf-class="dbs-tab{{ state.tab === 'browse' ? ' active' : '' }}"
                    t-on-click="() => this.setTab('browse')">Browse</button>
            <button t-attf-class="dbs-tab{{ state.tab === 'sql' ? ' active' : '' }}"
                    t-on-click="() => this.setTab('sql')">SQL</button>
            <button t-attf-class="dbs-tab{{ state.tab === 'schema' ? ' active' : '' }}"
                    t-on-click="() => this.setTab('schema')">Schema</button>
        </div>

        <t t-if="state.error"><div class="dbs-err" t-esc="state.error"/></t>

        <!-- ============ BROWSE ============ -->
        <div class="dbs-body" t-if="state.tab === 'browse'">
            <div class="dbs-side">
                <div class="dbs-search">
                    <input type="search" placeholder="Find a table…" t-model="state.search"
                           aria-label="Find a table"/>
                </div>
                <div class="dbs-tree">
                    <t t-if="state.loading"><div class="dbs-loading">Loading…</div></t>
                    <t t-foreach="groups" t-as="g" t-key="g.module">
                        <button class="dbs-group" t-on-click="() => this.toggleGroup(g.module)">
                            <span class="dbs-dot" t-attf-style="background:{{ g.color }}"/>
                            <span t-esc="g.module"/>
                            <span class="dbs-gcount" t-esc="g.tables.length"/>
                        </button>
                        <t t-if="isOpen(g.module)">
                            <t t-foreach="g.tables" t-as="t" t-key="t.name">
                                <button t-attf-class="dbs-item{{ state.sel === t.name ? ' active' : '' }}{{ t.kind === 'view' ? ' is-view' : '' }}"
                                        t-on-click="() => this.selectTable(t.name)">
                                    <span class="dbs-item-name" t-esc="t.name"/>
                                    <span class="dbs-item-rows" t-esc="fmt(t.rows)"/>
                                </button>
                            </t>
                        </t>
                    </t>
                    <t t-if="!state.loading and !groups.length">
                        <div class="dbs-empty">No table matches “<t t-esc="state.search"/>”.</div>
                    </t>
                </div>
            </div>

            <div class="dbs-main">
                <t t-if="!state.sel">
                    <div class="dbs-empty">Pick a table on the left to see its rows and its shape.</div>
                </t>
                <t t-else="">
                    <div class="dbs-thead">
                        <h3 class="dbs-tname" t-esc="state.sel"/>
                        <span class="dbs-chip" t-if="state.detail" t-esc="state.detail.kind"/>
                        <div class="dbs-tmeta" t-if="state.detail">
                            <span><t t-esc="fmt(state.detail.est_rows)"/> rows</span>
                            <span t-esc="state.detail.size_human"/>
                            <span><t t-esc="state.detail.columns.length"/> columns</span>
                        </div>
                        <span class="dbs-spacer"/>
                        <button class="dbs-btn ghost" t-on-click="queryThisTable">Open in SQL</button>
                        <button class="dbs-btn ghost" t-on-click="mapThisTable">Show on map</button>
                    </div>

                    <div class="dbs-subtabs">
                        <button t-attf-class="dbs-subtab{{ state.subtab === 'data' ? ' active' : '' }}"
                                t-on-click="() => this.setSubtab('data')">Data</button>
                        <button t-attf-class="dbs-subtab{{ state.subtab === 'columns' ? ' active' : '' }}"
                                t-on-click="() => this.setSubtab('columns')">Columns</button>
                        <button t-attf-class="dbs-subtab{{ state.subtab === 'indexes' ? ' active' : '' }}"
                                t-on-click="() => this.setSubtab('indexes')">Keys &amp; indexes</button>
                        <button t-attf-class="dbs-subtab{{ state.subtab === 'relations' ? ' active' : '' }}"
                                t-on-click="() => this.setSubtab('relations')">Relations</button>
                    </div>

                    <!-- Data -->
                    <t t-if="state.subtab === 'data'">
                        <div class="dbs-filter">
                            <select t-model="state.filter.col" aria-label="Filter column">
                                <option value="">(no filter)</option>
                                <t t-foreach="filterableColumns" t-as="c" t-key="c.name">
                                    <option t-att-value="c.name" t-esc="c.name"/>
                                </t>
                            </select>
                            <select t-model="state.filter.op" aria-label="Filter operator">
                                <option value="contains">contains</option>
                                <option value="startswith">starts with</option>
                                <option value="eq">=</option>
                                <option value="ne">≠</option>
                                <option value="gt">&gt;</option>
                                <option value="gte">≥</option>
                                <option value="lt">&lt;</option>
                                <option value="lte">≤</option>
                                <option value="empty">is null</option>
                                <option value="notempty">is not null</option>
                            </select>
                            <input t-if="needsValue" t-model="state.filter.value"
                                   placeholder="value" aria-label="Filter value"
                                   t-on-keydown="onFilterKey"/>
                            <button class="dbs-btn primary" t-on-click="applyFilter">Apply</button>
                            <button class="dbs-btn ghost" t-on-click="clearFilter" t-if="state.filter.col">Clear</button>
                            <span class="dbs-spacer"/>
                        </div>

                        <t t-if="state.rowsError"><div class="dbs-err" t-esc="state.rowsError"/></t>

                        <div class="dbs-gridwrap">
                            <t t-if="state.rowsBusy"><div class="dbs-loading">Loading rows…</div></t>
                            <table class="dbs-grid" t-elif="state.rows">
                                <thead>
                                    <tr>
                                        <t t-foreach="state.rows.columns" t-as="c" t-key="c.name">
                                            <th t-attf-class="{{ state.order === c.name ? 'sorted' : '' }}"
                                                t-att-title="'Sort by ' + c.name"
                                                t-on-click="() => this.sortBy(c.name)">
                                                <t t-esc="c.name"/><span t-if="isPk(c.name)" class="dbs-badge pk">pk</span><t t-esc="sortMark(c.name)"/>
                                                <span class="dbs-th-type" t-esc="typeOf(c.name)"/>
                                            </th>
                                        </t>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr t-foreach="state.rows.rows" t-as="r" t-key="r_index">
                                        <t t-foreach="r" t-as="v" t-key="v_index">
                                            <td t-if="v === null" class="null">NULL</td>
                                            <td t-elif="isMasked(v_index)" class="masked" t-esc="v"/>
                                            <td t-elif="fkOf(v_index)" t-att-title="v">
                                                <span class="dbs-fklink"
                                                      t-on-click="() => this.followFk(v_index, v)"
                                                      t-esc="v"/>
                                            </td>
                                            <td t-else="" t-attf-class="{{ isNum(v_index) ? 'num' : '' }}" t-att-title="v" t-esc="clip(v)"/>
                                        </t>
                                    </tr>
                                    <tr t-if="!state.rows.rows.length">
                                        <td t-att-colspan="state.rows.columns.length" class="null">No rows match.</td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>

                        <div class="dbs-foot" t-if="state.rows">
                            <span t-esc="rangeLabel"/>
                            <span t-esc="state.rows.elapsed_ms + ' ms'"/>
                            <span class="dbs-sqlpeek" t-att-title="state.rows.sql" t-esc="state.rows.sql"/>
                            <span class="dbs-spacer"/>
                            <!-- t-att-value + t-on-change rather than t-model: t-model on a
                                 select already binds "change", and a second handler for the
                                 same event on the same node collides. -->
                            <select t-att-value="state.pageSize" t-on-change="changePageSize" aria-label="Rows per page">
                                <option value="25">25</option>
                                <option value="50">50</option>
                                <option value="100">100</option>
                                <option value="200">200</option>
                            </select>
                            <button class="dbs-btn ghost" t-on-click="prevPage" t-att-disabled="state.page === 0">‹ Prev</button>
                            <button class="dbs-btn ghost" t-on-click="nextPage" t-att-disabled="!hasNext">Next ›</button>
                        </div>
                    </t>

                    <!-- Columns -->
                    <t t-if="state.subtab === 'columns'">
                        <div class="dbs-pane">
                            <div class="dbs-split">
                                <div>
                                    <table class="dbs-list" t-if="state.detail">
                                        <thead><tr><th>Column</th><th>Type</th><th>Nullable</th><th>Default</th></tr></thead>
                                        <tbody>
                                            <tr t-foreach="state.detail.columns" t-as="c" t-key="c.name"
                                                t-attf-class="clickable{{ state.profileCol === c.name ? ' selected' : '' }}"
                                                t-on-click="() => this.loadProfile(c.name)">
                                                <td>
                                                    <span class="dbs-mono" t-esc="c.name"/>
                                                    <span t-if="c.pk" class="dbs-badge pk">pk</span>
                                                    <span t-if="c.fk" class="dbs-badge fk">fk</span>
                                                    <span t-if="c.secret" class="dbs-badge secret">masked</span>
                                                </td>
                                                <td class="dbs-mono dbs-type" t-esc="c.type"/>
                                                <td t-esc="c.notnull ? 'not null' : 'null ok'"/>
                                                <td class="dbs-mono" t-att-title="c.default" t-esc="clipDefault(c.default)"/>
                                            </tr>
                                        </tbody>
                                    </table>
                                </div>

                                <div class="dbs-card">
                                    <h4>Column profile</h4>
                                    <t t-if="!state.profileCol">
                                        <p class="dbs-hint">Pick a column to see how its values are distributed.</p>
                                    </t>
                                    <t t-elif="state.profileBusy"><p class="dbs-hint">Profiling…</p></t>
                                    <t t-elif="state.profileError"><p class="dbs-hint" t-esc="state.profileError"/></t>
                                    <t t-elif="state.profile">
                                        <p class="dbs-mono" style="margin-top:0" t-esc="state.profile.column"/>
                                        <div class="dbs-bars" style="margin-bottom:12px">
                                            <div class="dbs-bar-row">
                                                <span class="dbs-bar-label">rows</span>
                                                <span/><span class="dbs-bar-val" t-esc="fmt(state.profile.total)"/>
                                            </div>
                                            <div class="dbs-bar-row">
                                                <span class="dbs-bar-label">distinct</span>
                                                <span/><span class="dbs-bar-val" t-esc="fmt(state.profile.distinct)"/>
                                            </div>
                                            <div class="dbs-bar-row">
                                                <span class="dbs-bar-label">null</span>
                                                <span/><span class="dbs-bar-val" t-esc="nullLabel"/>
                                            </div>
                                            <t t-if="state.profile.min !== undefined">
                                                <div class="dbs-bar-row">
                                                    <span class="dbs-bar-label">min / max</span>
                                                    <span/><span class="dbs-bar-val" t-esc="minMaxLabel"/>
                                                </div>
                                            </t>
                                        </div>
                                        <h4 t-if="state.profile.top_values.length">Most common</h4>
                                        <div class="dbs-bars">
                                            <t t-foreach="profileBars" t-as="b" t-key="b_index">
                                                <div class="dbs-bar-row" style="grid-template-columns:110px minmax(0,1fr) 58px">
                                                    <span class="dbs-bar-label" t-att-title="b.value" t-esc="b.value"/>
                                                    <span class="dbs-bar-track">
                                                        <span class="dbs-bar-fill"
                                                              t-attf-style="width:{{ b.pct }}%;background:{{ tableColor }}"/>
                                                    </span>
                                                    <span class="dbs-bar-val" t-esc="fmt(b.count)"/>
                                                </div>
                                            </t>
                                        </div>
                                    </t>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- Keys & indexes -->
                    <t t-if="state.subtab === 'indexes'">
                        <div class="dbs-pane" t-if="state.detail">
                            <div class="dbs-card" style="margin-bottom:18px">
                                <h4>Constraints</h4>
                                <table class="dbs-list">
                                    <thead><tr><th>Name</th><th>Kind</th><th>Definition</th></tr></thead>
                                    <tbody>
                                        <tr t-foreach="state.detail.constraints" t-as="c" t-key="c.name">
                                            <td class="dbs-mono" t-esc="c.name"/>
                                            <td t-esc="c.kind"/>
                                            <td class="dbs-mono" t-esc="c.definition"/>
                                        </tr>
                                        <tr t-if="!state.detail.constraints.length"><td colspan="3" class="dbs-hint">None.</td></tr>
                                    </tbody>
                                </table>
                            </div>
                            <div class="dbs-card">
                                <h4>Indexes</h4>
                                <table class="dbs-list">
                                    <thead><tr><th>Name</th><th>Size</th><th>Definition</th></tr></thead>
                                    <tbody>
                                        <tr t-foreach="state.detail.indexes" t-as="i" t-key="i.name">
                                            <td class="dbs-mono" t-esc="i.name"/>
                                            <td class="dbs-mono" t-esc="bytes(i.bytes)"/>
                                            <td class="dbs-mono" t-esc="i.definition"/>
                                        </tr>
                                        <tr t-if="!state.detail.indexes.length"><td colspan="3" class="dbs-hint">None.</td></tr>
                                    </tbody>
                                </table>
                            </div>
                        </div>
                    </t>

                    <!-- Relations -->
                    <t t-if="state.subtab === 'relations'">
                        <div class="dbs-pane" t-if="state.detail">
                            <div class="dbs-rel-grid">
                                <div class="dbs-card">
                                    <h4>This table references</h4>
                                    <t t-foreach="state.detail.references" t-as="r" t-key="r.name">
                                        <div class="dbs-rel-item">
                                            <span class="dbs-fklink dbs-mono" t-on-click="() => this.selectTable(r.table)" t-esc="r.table"/>
                                            <span class="dbs-rel-def" t-esc="r.definition"/>
                                        </div>
                                    </t>
                                    <p class="dbs-hint" t-if="!state.detail.references.length">Nothing — this table stands alone.</p>
                                </div>
                                <div class="dbs-card">
                                    <h4>Referenced by</h4>
                                    <t t-foreach="state.detail.referenced_by" t-as="r" t-key="r.name">
                                        <div class="dbs-rel-item">
                                            <span class="dbs-fklink dbs-mono" t-on-click="() => this.selectTable(r.table)" t-esc="r.table"/>
                                            <span class="dbs-rel-def" t-esc="r.definition"/>
                                        </div>
                                    </t>
                                    <p class="dbs-hint" t-if="!state.detail.referenced_by.length">Nothing points here.</p>
                                </div>
                            </div>
                        </div>
                    </t>
                </t>
            </div>
        </div>

        <!-- ============ SQL ============ -->
        <div class="dbs-sql" t-if="state.tab === 'sql'">
            <div class="dbs-sql-editor">
                <textarea t-ref="sqlbox" t-model="state.sql" spellcheck="false"
                          aria-label="SQL query"
                          placeholder="SELECT * FROM res_partner ORDER BY id DESC"
                          t-on-keydown="onSqlKey"/>
            </div>
            <div class="dbs-sql-bar">
                <button class="dbs-btn primary" t-on-click="runSql" t-att-disabled="state.sqlBusy">
                    <t t-esc="state.sqlBusy ? 'Running…' : 'Run'"/>
                </button>
                <span class="dbs-hint"><span class="dbs-kbd">Ctrl</span> + <span class="dbs-kbd">Enter</span></span>
                <select t-model="state.sqlLimit" aria-label="Row limit">
                    <option value="50">50 rows</option>
                    <option value="200">200 rows</option>
                    <option value="1000">1000 rows</option>
                </select>
                <button class="dbs-btn ghost" t-on-click="clearSql">Clear</button>
                <span class="dbs-spacer"/>
                <span class="dbs-hint" t-if="state.sqlResult">
                    <t t-esc="fmt(state.sqlResult.row_count)"/> rows · <t t-esc="state.sqlResult.elapsed_ms"/> ms
                </span>
                <button class="dbs-btn ghost" t-if="state.sqlResult and state.sqlResult.rows.length"
                        t-on-click="copyCsv">Copy as CSV</button>
            </div>

            <div class="dbs-snips">
                <t t-foreach="snippets" t-as="s" t-key="s.label">
                    <button class="dbs-snip" t-att-title="s.sql" t-on-click="() => this.useSnippet(s.sql)" t-esc="s.label"/>
                </t>
            </div>

            <t t-if="state.sqlError"><div class="dbs-err" t-esc="state.sqlError"/></t>
            <t t-if="state.sqlNotice"><div class="dbs-notice" t-esc="state.sqlNotice"/></t>
            <t t-if="state.sqlResult and state.sqlResult.truncated">
                <div class="dbs-notice">Showing the first <t t-esc="state.sqlResult.limit"/> rows. Raise the limit or add your own LIMIT to see more.</div>
            </t>

            <div class="dbs-gridwrap" t-if="state.sqlResult">
                <table class="dbs-grid">
                    <thead>
                        <tr><t t-foreach="state.sqlResult.columns" t-as="c" t-key="c_index"><th t-esc="c.name"/></t></tr>
                    </thead>
                    <tbody>
                        <tr t-foreach="state.sqlResult.rows" t-as="r" t-key="r_index">
                            <t t-foreach="r" t-as="v" t-key="v_index">
                                <td t-if="v === null" class="null">NULL</td>
                                <td t-else="" t-att-title="v" t-esc="clip(v)"/>
                            </t>
                        </tr>
                        <tr t-if="!state.sqlResult.rows.length">
                            <td t-att-colspan="state.sqlResult.columns.length" class="null">No rows.</td>
                        </tr>
                    </tbody>
                </table>
            </div>

            <div class="dbs-pane" t-if="state.history.length and !state.sqlResult">
                <div class="dbs-card">
                    <h4>Recent</h4>
                    <t t-foreach="state.history" t-as="h" t-key="h_index">
                        <div class="dbs-rel-item">
                            <span class="dbs-fklink dbs-mono" t-on-click="() => this.useSnippet(h)" t-esc="clipDefault(h)"/>
                        </div>
                    </t>
                </div>
            </div>
        </div>

        <!-- ============ SCHEMA ============ -->
        <div class="dbs-pane" t-if="state.tab === 'schema'">
            <t t-if="state.overview">
                <div class="dbs-tiles">
                    <div class="dbs-tile">
                        <div class="dbs-tile-n" t-esc="fmt(state.overview.table_count)"/>
                        <div class="dbs-tile-l">Tables</div>
                    </div>
                    <div class="dbs-tile">
                        <div class="dbs-tile-n" t-esc="fmt(state.overview.row_estimate)"/>
                        <div class="dbs-tile-l">Rows (estimated)</div>
                    </div>
                    <div class="dbs-tile">
                        <div class="dbs-tile-n" t-esc="fmt(state.overview.fk_count)"/>
                        <div class="dbs-tile-l">Foreign keys</div>
                    </div>
                    <div class="dbs-tile">
                        <div class="dbs-tile-n" t-esc="fmt(state.overview.index_count)"/>
                        <div class="dbs-tile-l">Indexes</div>
                    </div>
                    <div class="dbs-tile">
                        <div class="dbs-tile-n" t-esc="state.overview.size_human"/>
                        <div class="dbs-tile-l">On disk</div>
                    </div>
                </div>

                <div class="dbs-split" style="grid-template-columns:minmax(0,1fr) minmax(0,1fr)">
                    <div class="dbs-card">
                        <h4>Storage by module</h4>
                        <div class="dbs-bars">
                            <t t-foreach="moduleBars" t-as="b" t-key="b.module">
                                <div class="dbs-bar-row">
                                    <span class="dbs-bar-label" t-esc="b.module"/>
                                    <span class="dbs-bar-track">
                                        <span class="dbs-bar-fill" t-attf-style="width:{{ b.pct }}%;background:{{ b.color }}"
                                              t-attf-title="{{ b.module }}: {{ b.human }} across {{ b.tables }} tables"/>
                                    </span>
                                    <span class="dbs-bar-val" t-esc="b.human"/>
                                </div>
                            </t>
                        </div>
                    </div>

                    <div class="dbs-card">
                        <h4>Largest tables</h4>
                        <div class="dbs-bars">
                            <t t-foreach="sizeBars" t-as="b" t-key="b.table">
                                <div class="dbs-bar-row">
                                    <span class="dbs-bar-label dbs-fklink" t-on-click="() => this.jumpTo(b.table)" t-esc="b.table"/>
                                    <span class="dbs-bar-track">
                                        <span class="dbs-bar-fill" t-attf-style="width:{{ b.pct }}%;background:{{ b.color }}"
                                              t-attf-title="{{ b.table }}: {{ b.human }}, {{ fmt(b.rows) }} rows"/>
                                    </span>
                                    <span class="dbs-bar-val" t-esc="b.human"/>
                                </div>
                            </t>
                        </div>
                    </div>
                </div>

                <div style="margin-top:20px">
                    <div class="dbs-map-head">
                        <h4 style="margin:0;color:var(--muted);font-size:.82rem;letter-spacing:.05em;text-transform:uppercase">Foreign-key map</h4>
                        <button t-attf-class="dbs-btn{{ state.mapMode === 'modules' ? ' primary' : ' ghost' }}"
                                t-on-click="() => this.setMapMode('modules')">By module</button>
                        <button t-attf-class="dbs-btn{{ state.mapMode === 'focus' ? ' primary' : ' ghost' }}"
                                t-on-click="() => this.setMapMode('focus')">Around one table</button>
                        <select t-if="state.mapMode === 'focus'" t-model="state.mapFocus" aria-label="Table to centre on"
                                style="background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:5px;padding:6px 8px;font-family:inherit;font-size:.82rem">
                            <t t-foreach="allTableNames" t-as="n" t-key="n">
                                <option t-att-value="n" t-esc="n"/>
                            </t>
                        </select>
                        <span class="dbs-hint" t-if="state.mapMode === 'modules'">Arc thickness is the number of foreign keys. Click a module to browse it.</span>
                        <span class="dbs-hint" t-else="">Click any table to re-centre.</span>
                    </div>

                    <t t-if="state.graph">
                        <DbSchemaMap graph="state.graph" mode="state.mapMode" focus="state.mapFocus"
                                     modules="allModules" onPick.bind="onMapPick"/>
                    </t>
                    <t t-else=""><div class="dbs-loading">Loading the map…</div></t>

                    <div class="dbs-legend" t-if="state.mapMode === 'modules'">
                        <t t-foreach="legend" t-as="l" t-key="l.module">
                            <span><span class="dbs-dot" t-attf-style="background:{{ l.color }}"/><t t-esc="l.module"/></span>
                        </t>
                    </div>
                </div>
            </t>
            <t t-else=""><div class="dbs-loading">Loading…</div></t>
        </div>
    </div>
    `;

    setup() {
        this.state = owl.useState({
            tab: 'browse',
            loading: true, error: '',
            overview: null, tables: [], graph: null,
            search: '', openGroups: {}, sel: null, detail: null, subtab: 'data',
            rows: null, rowsBusy: false, rowsError: '',
            page: 0, pageSize: '50', order: '', dir: 'asc',
            filter: { col: '', op: 'contains', value: '' },
            profile: null, profileCol: '', profileBusy: false, profileError: '',
            sql: '', sqlResult: null, sqlError: '', sqlNotice: '', sqlBusy: false,
            sqlLimit: '200', history: [],
            mapMode: 'modules', mapFocus: '',
        });
        this.sqlbox = owl.useRef('sqlbox');
        try {
            const h = window.localStorage.getItem('dbstudio.history');
            if (h) this.state.history = JSON.parse(h).slice(0, 10);
        } catch (_) { /* history is a convenience, never a hard dependency */ }
        this.load();
    }

    // ---- loading ----
    async load() {
        try {
            const [ov, tb] = await Promise.all([
                RpcService.dbTool('overview'),
                RpcService.dbTool('tables'),
            ]);
            this.state.overview = ov;
            this.state.tables = tb.tables || [];
            // Open the biggest module so the sidebar is not a wall of closed rows.
            const first = this.groups[0];
            if (first) this.state.openGroups[first.module] = true;
        } catch (e) {
            this.state.error = e.message || 'Could not read the database.';
        }
        this.state.loading = false;
    }

    async ensureGraph() {
        if (this.state.graph) return;
        try { this.state.graph = await RpcService.dbTool('graph'); }
        catch (e) { this.state.error = e.message; }
    }

    // ---- tabs ----
    setTab(t) {
        this.state.tab = t;
        if (t === 'schema') this.ensureGraph();
        if (t === 'sql') this.focusSql();
    }
    setSubtab(t) {
        this.state.subtab = t;
        if (t === 'data' && !this.state.rows) this.loadRows();
    }
    setMapMode(m) {
        this.state.mapMode = m;
        if (m === 'focus' && !this.state.mapFocus)
            this.state.mapFocus = this.state.sel || (this.state.tables[0] || {}).name || '';
    }
    onMapPick(id) {
        // In module mode a click is "browse this module"; in focus mode it re-centres.
        if (this.state.mapMode === 'modules') {
            this.state.openGroups[id] = true;
            const t = this.state.tables.find(x => x.module === id);
            if (t) this.jumpTo(t.name);
        } else if (id && !id.startsWith('__h')) {
            this.state.mapFocus = id;
        }
    }
    jumpTo(name) { this.state.tab = 'browse'; this.selectTable(name); }
    mapThisTable() {
        this.state.mapFocus = this.state.sel;
        this.state.mapMode = 'focus';
        this.state.tab = 'schema';
        this.ensureGraph();
    }
    queryThisTable() {
        this.state.sql = 'SELECT *\nFROM ' + this.state.sel + '\nORDER BY 1 DESC';
        this.state.tab = 'sql';
        this.focusSql();
    }

    // ---- sidebar ----
    get allModules() {
        // Alphabetical and computed from every table, not the filtered view, so a
        // module keeps its colour no matter what the search box says.
        return [...new Set(this.state.tables.map(t => t.module))].sort();
    }
    get allTableNames() { return this.state.tables.map(t => t.name); }

    get groups() {
        const q = (this.state.search || '').trim().toLowerCase();
        const mods = this.allModules;
        const by = new Map();
        for (const t of this.state.tables) {
            if (q && !t.name.toLowerCase().includes(q)) continue;
            if (!by.has(t.module)) by.set(t.module, []);
            by.get(t.module).push(t);
        }
        return [...by.entries()]
            .map(([module, tables]) => ({
                module, tables,
                color: dbsColorFor(module, mods),
                bytes: tables.reduce((a, t) => a + (t.bytes || 0), 0),
            }))
            .sort((a, b) => b.bytes - a.bytes);
    }
    get legend() { return this.allModules.map(m => ({ module: m, color: dbsColorFor(m, this.allModules) })); }
    isOpen(m) { return !!this.state.openGroups[m] || !!(this.state.search || '').trim(); }
    toggleGroup(m) { this.state.openGroups[m] = !this.state.openGroups[m]; }

    // ---- table selection ----
    async selectTable(name) {
        this.state.sel = name;
        this.state.detail = null;
        this.state.rows = null;
        this.state.rowsError = '';
        this.state.page = 0;
        this.state.order = '';
        this.state.dir = 'asc';
        this.state.filter = { col: '', op: 'contains', value: '' };
        this.state.profile = null;
        this.state.profileCol = '';
        try {
            this.state.detail = await RpcService.dbTool('table', { table: name });
        } catch (e) { this.state.rowsError = e.message; return; }
        if (this.state.subtab === 'data') this.loadRows();
    }

    async loadRows() {
        if (!this.state.sel) return;
        this.state.rowsBusy = true;
        this.state.rowsError = '';
        const f = this.state.filter;
        const args = {
            table: this.state.sel,
            limit: this.pageSizeN,
            offset: this.state.page * this.pageSizeN,
        };
        if (this.state.order) { args.order = this.state.order; args.dir = this.state.dir; }
        if (f.col) args.filter = { col: f.col, op: f.op, value: f.value };
        try {
            this.state.rows = await RpcService.dbTool('rows', args);
        } catch (e) {
            this.state.rows = null;
            this.state.rowsError = e.message;
        }
        this.state.rowsBusy = false;
    }

    get pageSizeN() { return parseInt(this.state.pageSize, 10) || 50; }
    get hasNext() {
        const r = this.state.rows;
        return !!r && (r.offset + r.rows.length) < r.total;
    }
    get rangeLabel() {
        const r = this.state.rows;
        if (!r) return '';
        if (!r.rows.length) return '0 rows';
        const from = r.offset + 1, to = r.offset + r.rows.length;
        return `${this.fmt(from)}–${this.fmt(to)} of ${r.estimated ? '~' : ''}${this.fmt(r.total)}`;
    }
    prevPage() { if (this.state.page > 0) { this.state.page--; this.loadRows(); } }
    nextPage() { if (this.hasNext) { this.state.page++; this.loadRows(); } }
    changePageSize(ev) { this.state.pageSize = ev.target.value; this.state.page = 0; this.loadRows(); }

    sortBy(col) {
        if (this.state.order === col) this.state.dir = this.state.dir === 'asc' ? 'desc' : 'asc';
        else { this.state.order = col; this.state.dir = 'asc'; }
        this.state.page = 0;
        this.loadRows();
    }
    sortMark(col) { return this.state.order === col ? (this.state.dir === 'asc' ? ' ▲' : ' ▼') : ''; }

    get needsValue() { return this.state.filter.op !== 'empty' && this.state.filter.op !== 'notempty'; }
    get filterableColumns() { return (this.state.detail && this.state.detail.columns) || []; }
    applyFilter() { this.state.page = 0; this.loadRows(); }
    clearFilter() {
        this.state.filter = { col: '', op: 'contains', value: '' };
        this.state.page = 0;
        this.loadRows();
    }
    onFilterKey(ev) { if (ev.key === 'Enter') this.applyFilter(); }

    // ---- per-cell rendering helpers ----
    colMeta(idx) {
        const r = this.state.rows;
        if (!r || !r.columns[idx]) return null;
        const name = r.columns[idx].name;
        return (this.state.detail && this.state.detail.columns.find(c => c.name === name)) || null;
    }
    isPk(name) {
        const c = this.state.detail && this.state.detail.columns.find(x => x.name === name);
        return !!(c && c.pk);
    }
    typeOf(name) {
        const c = this.state.detail && this.state.detail.columns.find(x => x.name === name);
        return c ? c.type : '';
    }
    isMasked(idx) {
        const r = this.state.rows;
        return !!(r && r.columns[idx] && r.columns[idx].masked);
    }
    fkOf(idx) { const m = this.colMeta(idx); return m ? m.fk : null; }
    isNum(idx) {
        const m = this.colMeta(idx);
        if (!m) return false;
        return ['smallint', 'integer', 'bigint', 'numeric', 'real', 'double precision']
            .some(t => m.type.startsWith(t));
    }
    /** Follow a foreign key: open the target table filtered to that one row. */
    followFk(idx, value) {
        const fk = this.fkOf(idx);
        if (!fk) return;
        const target = fk.table, col = fk.column;
        this.selectTable(target).then(() => {
            this.state.filter = { col, op: 'eq', value: String(value) };
            this.state.subtab = 'data';
            this.state.page = 0;
            this.loadRows();
        });
    }

    // ---- column profile ----
    async loadProfile(col) {
        this.state.profileCol = col;
        this.state.profile = null;
        this.state.profileError = '';
        this.state.profileBusy = true;
        try {
            this.state.profile = await RpcService.dbTool('profile', { table: this.state.sel, column: col });
        } catch (e) { this.state.profileError = e.message; }
        this.state.profileBusy = false;
    }
    get profileBars() {
        const p = this.state.profile;
        if (!p || !p.top_values.length) return [];
        const max = Math.max(...p.top_values.map(v => v.count), 1);
        return p.top_values.map(v => ({
            value: v.value === '' ? '(empty)' : this.clipTo(v.value, 40),
            count: v.count,
            pct: Math.max(1, Math.round((v.count / max) * 100)),
        }));
    }
    get nullLabel() {
        const p = this.state.profile;
        if (!p) return '';
        if (!p.total) return '0';
        return `${this.fmt(p.nulls)} (${Math.round((p.nulls / p.total) * 100)}%)`;
    }
    get minMaxLabel() {
        const p = this.state.profile;
        if (!p) return '';
        return `${p.min === null ? '—' : p.min} / ${p.max === null ? '—' : p.max}`;
    }
    get tableColor() {
        const t = this.state.tables.find(x => x.name === this.state.sel);
        return dbsColorFor(t ? t.module : '', this.allModules);
    }

    // ---- schema charts ----
    get moduleBars() {
        const ov = this.state.overview;
        if (!ov || !ov.modules) return [];
        const max = Math.max(...ov.modules.map(m => m.bytes), 1);
        const mods = this.allModules;
        return ov.modules.slice(0, 12).map(m => ({
            module: m.module, tables: m.tables,
            human: this.bytes(m.bytes),
            pct: Math.max(1, Math.round((m.bytes / max) * 100)),
            color: dbsColorFor(m.module, mods),
        }));
    }
    get sizeBars() {
        const ov = this.state.overview;
        if (!ov || !ov.top_by_size) return [];
        const max = Math.max(...ov.top_by_size.map(t => t.bytes), 1);
        const mods = this.allModules;
        return ov.top_by_size.map(t => ({
            table: t.table, rows: t.rows,
            human: this.bytes(t.bytes),
            pct: Math.max(1, Math.round((t.bytes / max) * 100)),
            color: dbsColorFor(t.module, mods),
        }));
    }

    // ---- SQL console ----
    get snippets() {
        return [
            { label: 'Biggest tables', sql: "SELECT c.relname AS table, pg_size_pretty(pg_total_relation_size(c.oid)) AS size,\n       c.reltuples::bigint AS approx_rows\nFROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace\nWHERE n.nspname = 'public' AND c.relkind = 'r'\nORDER BY pg_total_relation_size(c.oid) DESC\nLIMIT 20" },
            { label: 'Unbalanced journal entries', sql: "SELECT m.id, m.name, SUM(l.debit) AS debit, SUM(l.credit) AS credit\nFROM account_move m\nJOIN account_move_line l ON l.move_id = m.id\nGROUP BY m.id, m.name\nHAVING SUM(l.debit) <> SUM(l.credit)\nORDER BY m.id" },
            { label: 'Invoices by state', sql: "SELECT move_type, state, count(*) AS n, SUM(amount_total)/1000000.0 AS total\nFROM account_move\nGROUP BY move_type, state\nORDER BY move_type, state" },
            { label: 'Stock on hand', sql: "SELECT p.name AS product, SUM(q.quantity)/1000000.0 AS qty\nFROM stock_quant q JOIN product_product p ON p.id = q.product_id\nGROUP BY p.name\nHAVING SUM(q.quantity) <> 0\nORDER BY qty DESC" },
            { label: 'Tables with no primary key', sql: "SELECT c.relname AS table\nFROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace\nWHERE n.nspname = 'public' AND c.relkind = 'r'\n  AND NOT EXISTS (SELECT 1 FROM pg_constraint k\n                  WHERE k.conrelid = c.oid AND k.contype = 'p')\nORDER BY 1" },
            { label: 'Unused indexes', sql: "SELECT relname AS table, indexrelname AS index, idx_scan AS scans,\n       pg_size_pretty(pg_relation_size(indexrelid)) AS size\nFROM pg_stat_user_indexes\nWHERE idx_scan = 0\nORDER BY pg_relation_size(indexrelid) DESC" },
        ];
    }
    useSnippet(sql) { this.state.sql = sql; this.state.tab = 'sql'; this.focusSql(); }
    clearSql() {
        this.state.sql = '';
        this.state.sqlResult = null;
        this.state.sqlError = '';
        this.state.sqlNotice = '';
        this.focusSql();
    }
    focusSql() {
        // The ref only exists once the SQL tab has actually rendered.
        Promise.resolve().then(() => { if (this.sqlbox.el) this.sqlbox.el.focus(); });
    }
    onSqlKey(ev) {
        if ((ev.ctrlKey || ev.metaKey) && ev.key === 'Enter') { ev.preventDefault(); this.runSql(); }
    }
    async runSql() {
        const sql = (this.state.sql || '').trim();
        if (!sql || this.state.sqlBusy) return;
        this.state.sqlBusy = true;
        this.state.sqlError = '';
        this.state.sqlNotice = '';
        try {
            this.state.sqlResult = await RpcService.dbTool('query', { sql, limit: parseInt(this.state.sqlLimit, 10) });
            this.remember(sql);
        } catch (e) {
            this.state.sqlResult = null;
            this.state.sqlError = e.message;
        }
        this.state.sqlBusy = false;
    }
    remember(sql) {
        const h = [sql, ...this.state.history.filter(x => x !== sql)].slice(0, 10);
        this.state.history = h;
        try { window.localStorage.setItem('dbstudio.history', JSON.stringify(h)); } catch (_) { /* fine */ }
    }
    async copyCsv() {
        const r = this.state.sqlResult;
        if (!r) return;
        const esc = (v) => {
            const s = v === null ? '' : String(v);
            return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
        };
        const text = [r.columns.map(c => esc(c.name)).join(','),
                      ...r.rows.map(row => row.map(esc).join(','))].join('\n');
        try {
            await window.navigator.clipboard.writeText(text);
            this.state.sqlNotice = `Copied ${r.rows.length} rows to the clipboard.`;
        } catch (_) {
            this.state.sqlNotice = 'Could not reach the clipboard — your browser blocked it.';
        }
    }

    // ---- formatting (templates cannot call JS globals) ----
    fmt(n) { return (Number(n) || 0).toLocaleString(); }
    bytes(n) {
        const v = Number(n) || 0;
        if (v >= 1073741824) return (v / 1073741824).toFixed(1) + ' GB';
        if (v >= 1048576)    return (v / 1048576).toFixed(1) + ' MB';
        if (v >= 1024)       return Math.round(v / 1024) + ' kB';
        return v + ' B';
    }
    clip(v) { return this.clipTo(v === null ? '' : String(v), 90); }
    clipTo(s, n) { return s.length > n ? s.slice(0, n - 1) + '…' : s; }
    clipDefault(v) { return v === null || v === undefined ? '' : this.clipTo(String(v), 60); }
}
