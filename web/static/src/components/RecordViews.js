/**
 * RecordViews.js — grouped list, kanban, pivot, graph and calendar (docs/095).
 *
 * Until now every action in this app was `list` or `list,form`, and anything
 * that wanted a board, a cross-tab or a chart had to be a bespoke screen with
 * its own SQL. These five are generic: they read `fields_get` to discover what
 * a model can be grouped and measured by, then drive `read_group` — so they
 * work on any model without a line of per-model code.
 *
 * Shared vocabulary, matching the server:
 *   groupable = selection | many2one | boolean | date | datetime
 *   measure   = integer | float | monetary
 * A date group key carries a granularity the way Odoo spells it, "date:month".
 */

// Reused from the Database Tools palette: eight hues already checked for
// contrast and for separation under the three common colour-vision deficiencies,
// so charts here inherit that rather than inventing a fresh set.
const RV_HUES = ['#6ea8fe', '#f0a868', '#4ec9b0', '#e94560',
                 '#b48ead', '#7fd3f7', '#e8d44d', '#d98880'];
const RV_OTHER = '#7a8ba3';

const RV_GROUPABLE = ['selection', 'many2one', 'boolean', 'date', 'datetime'];
const RV_MEASURE   = ['integer', 'float', 'monetary'];

/** Shared behaviour: load field metadata, then load groups. */
class RvBase extends owl.Component {
    static props = ['action', 'onOpenForm?'];

    get model() { return this.props.action.res_model; }

    async loadFields() {
        const f = await RpcService.call(this.model, 'fields_get', [[]], {});
        const groupable = [], measures = [];
        for (const [name, d] of Object.entries(f || {})) {
            if (name === 'id') continue;
            const label = (d && d.string) || name;
            if (RV_GROUPABLE.includes(d && d.type)) groupable.push({ name, label, type: d.type });
            if (RV_MEASURE.includes(d && d.type))   measures.push({ name, label, type: d.type });
        }
        groupable.sort((a, b) => a.label.localeCompare(b.label));
        measures.sort((a, b) => a.label.localeCompare(b.label));
        return { fields: f || {}, groupable, measures };
    }

    /** A date field needs its granularity appended before the server sees it. */
    groupSpec(name, interval) {
        const d = this.state.fields[name];
        const isDate = d && (d.type === 'date' || d.type === 'datetime');
        return isDate ? `${name}:${interval || 'month'}` : name;
    }

    async readGroup(groupby, measures, domain) {
        return RpcService.call(this.model, 'read_group',
                               [domain || [], measures || [], groupby], {});
    }

    /** Groups come back keyed by the spec that produced them, not the bare field. */
    keyOf(group, spec) {
        return group[spec] !== undefined ? group[spec] : group[spec.split(':')[0]];
    }

    /** A group key rendered for a human: many2one arrives as [id, label]. */
    labelOf(value) {
        if (value === false || value === null || value === undefined) return 'None';
        if (Array.isArray(value)) return value[1] || ('#' + value[0]);
        if (value === true) return 'Yes';
        return String(value);
    }

    fmtNum(v) {
        const n = Number(v) || 0;
        return n.toLocaleString(undefined, { maximumFractionDigits: 2 });
    }
    hue(i) { return i < RV_HUES.length ? RV_HUES[i] : RV_OTHER; }
}

// ============================================================
// GroupedListView — a list broken into collapsible groups
// ============================================================
class GroupedListView extends RvBase {
    static template = owl.xml`
        <div class="rv-wrap">
            <div class="rv-bar">
                <label class="rv-lbl">Group by</label>
                <select t-on-change="onGroup">
                    <option value="">(none)</option>
                    <t t-foreach="state.groupable" t-as="g" t-key="g.name">
                        <option t-att-value="g.name" t-att-selected="g.name === state.groupby" t-esc="g.label"/>
                    </t>
                </select>
                <t t-if="state.isDate">
                    <select t-on-change="onInterval">
                        <t t-foreach="['day','week','month','quarter','year']" t-as="iv" t-key="iv">
                            <option t-att-value="iv" t-att-selected="iv === state.interval" t-esc="iv"/>
                        </t>
                    </select>
                </t>
                <label class="rv-lbl" t-if="state.measures.length">Measure</label>
                <select t-if="state.measures.length" t-on-change="onMeasure">
                    <option value="">count only</option>
                    <t t-foreach="state.measures" t-as="m" t-key="m.name">
                        <option t-att-value="m.name" t-att-selected="m.name === state.measure" t-esc="m.label"/>
                    </t>
                </select>
                <span class="rv-spacer"/>
                <span class="rv-total" t-if="state.groups.length">
                    <t t-esc="state.groups.length"/> groups · <t t-esc="fmtNum(totalCount)"/> records
                    <t t-if="state.measure"> · <t t-esc="fmtNum(totalMeasure)"/></t>
                </span>
            </div>

            <t t-if="state.error"><div class="rv-error" t-esc="state.error"/></t>
            <t t-if="state.loading"><div class="rv-loading">Loading…</div></t>

            <t t-elif="!state.groupby">
                <div class="rv-hint">Pick a field to group by.</div>
            </t>
            <t t-else="">
                <div class="rv-groups">
                    <t t-foreach="state.groups" t-as="g" t-key="g_index">
                        <div class="rv-group">
                            <button class="rv-group-head" t-on-click="() => this.toggle(g_index)">
                                <span class="rv-caret" t-esc="state.open[g_index] ? '▾' : '▸'"/>
                                <span class="rv-group-name" t-esc="labelOf(keyOf(g, state.spec))"/>
                                <span class="rv-group-count" t-esc="fmtNum(g.__count)"/>
                                <span class="rv-group-sum" t-if="state.measure" t-esc="fmtNum(g[state.measure])"/>
                            </button>
                            <t t-if="state.open[g_index]">
                                <div class="rv-rows">
                                    <t t-if="!state.rows[g_index]"><div class="rv-loading">Loading rows…</div></t>
                                    <table t-else="" class="rv-table">
                                        <tbody>
                                            <tr t-foreach="state.rows[g_index]" t-as="r" t-key="r.id"
                                                t-on-click="() => this.open(r.id)">
                                                <t t-foreach="rowCols" t-as="c" t-key="c">
                                                    <td t-esc="cell(r, c)"/>
                                                </t>
                                            </tr>
                                        </tbody>
                                    </table>
                                </div>
                            </t>
                        </div>
                    </t>
                    <div t-if="!state.groups.length" class="rv-hint">No records.</div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            fields: {}, groupable: [], measures: [],
            groupby: '', interval: 'month', measure: '', spec: '',
            groups: [], open: {}, rows: {}, loading: true, error: '',
        });
        this.init();
    }

    async init() {
        try {
            const { fields, groupable, measures } = await this.loadFields();
            Object.assign(this.state, { fields, groupable, measures });
            // Open on something useful rather than an empty screen: prefer a
            // state-like field, else the first groupable one.
            const pref = groupable.find(g => g.name === 'state') || groupable[0];
            if (pref) { this.state.groupby = pref.name; await this.reload(); }
        } catch (e) { this.state.error = (e && e.message) || 'Could not load fields.'; }
        this.state.loading = false;
    }

    get isDateField() {
        const d = this.state.fields[this.state.groupby];
        return !!d && (d.type === 'date' || d.type === 'datetime');
    }
    get totalCount()   { return this.state.groups.reduce((a, g) => a + (g.__count || 0), 0); }
    get totalMeasure() {
        const m = this.state.measure;
        return m ? this.state.groups.reduce((a, g) => a + (Number(g[m]) || 0), 0) : 0;
    }
    get rowCols() {
        // A handful of readable columns; the form is one click away for the rest.
        const pick = ['name', 'display_name', 'partner_id', 'date', 'state', 'amount_total'];
        return pick.filter(c => this.state.fields[c]);
    }

    cell(row, col) {
        const v = row[col];
        if (v === false || v === null || v === undefined) return '';
        if (Array.isArray(v)) return v[1] || ('#' + v[0]);
        return String(v);
    }

    async reload() {
        this.state.loading = true;
        this.state.error = '';
        this.state.open = {}; this.state.rows = {};
        this.state.isDate = this.isDateField;
        const spec = this.groupSpec(this.state.groupby, this.state.interval);
        this.state.spec = spec;
        try {
            this.state.groups = await this.readGroup([spec],
                                                     this.state.measure ? [this.state.measure] : [], []);
        } catch (e) {
            this.state.groups = [];
            this.state.error = (e && e.message) || 'Grouping failed.';
        }
        this.state.loading = false;
    }

    async onGroup(ev)    { this.state.groupby = ev.target.value; if (this.state.groupby) await this.reload(); else this.state.groups = []; }
    async onInterval(ev) { this.state.interval = ev.target.value; await this.reload(); }
    async onMeasure(ev)  { this.state.measure = ev.target.value; await this.reload(); }

    async toggle(i) {
        this.state.open[i] = !this.state.open[i];
        if (!this.state.open[i] || this.state.rows[i]) return;
        // Rows are fetched with the group's own __domain, so the drill-down can
        // never disagree with the count above it.
        try {
            this.state.rows[i] = await RpcService.call(
                this.model, 'search_read', [this.state.groups[i].__domain || [], this.rowCols], { limit: 200 });
        } catch (e) { this.state.rows[i] = []; this.state.error = e.message; }
    }

    open(id) { if (this.props.onOpenForm) this.props.onOpenForm(id); }
}

// ============================================================
// KanbanView — one column per group, one card per record
// ============================================================
class KanbanView extends RvBase {
    static template = owl.xml`
        <div class="rv-wrap">
            <div class="rv-bar">
                <label class="rv-lbl">Columns</label>
                <select t-on-change="onGroup">
                    <t t-foreach="state.groupable" t-as="g" t-key="g.name">
                        <option t-att-value="g.name" t-att-selected="g.name === state.groupby" t-esc="g.label"/>
                    </t>
                </select>
                <span class="rv-spacer"/>
                <span class="rv-total"><t t-esc="fmtNum(totalCount)"/> records</span>
            </div>
            <t t-if="state.error"><div class="rv-error" t-esc="state.error"/></t>
            <t t-if="state.loading"><div class="rv-loading">Loading…</div></t>
            <div t-else="" class="rv-kanban">
                <div class="rv-col" t-foreach="state.groups" t-as="g" t-key="g_index">
                    <div class="rv-col-head">
                        <span class="rv-dot" t-attf-style="background:{{ hue(g_index) }}"/>
                        <span t-esc="labelOf(keyOf(g, state.spec))"/>
                        <span class="rv-col-count" t-esc="fmtNum(g.__count)"/>
                    </div>
                    <div class="rv-cards">
                        <t t-if="!state.rows[g_index]"><div class="rv-loading">…</div></t>
                        <t t-else="">
                            <div class="rv-card" t-foreach="state.rows[g_index]" t-as="r" t-key="r.id"
                                 t-on-click="() => this.open(r.id)">
                                <div class="rv-card-title" t-esc="cardTitle(r)"/>
                                <div class="rv-card-sub" t-esc="cardSub(r)"/>
                            </div>
                            <div class="rv-more" t-if="g.__count > state.rows[g_index].length">
                                +<t t-esc="g.__count - state.rows[g_index].length"/> more
                            </div>
                        </t>
                    </div>
                </div>
                <div t-if="!state.groups.length" class="rv-hint">No records.</div>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            fields: {}, groupable: [], measures: [],
            groupby: '', spec: '', groups: [], rows: {}, loading: true, error: '',
        });
        this.init();
    }

    async init() {
        try {
            const { fields, groupable, measures } = await this.loadFields();
            Object.assign(this.state, { fields, groupable, measures });
            // A board wants few, stable columns — a selection field beats a
            // many2one with hundreds of values.
            const pref = groupable.find(g => g.name === 'state')
                      || groupable.find(g => g.type === 'selection')
                      || groupable[0];
            if (pref) { this.state.groupby = pref.name; await this.reload(); }
            else this.state.error = 'This model has no field to group columns by.';
        } catch (e) { this.state.error = (e && e.message) || 'Could not load fields.'; }
        this.state.loading = false;
    }

    get totalCount() { return this.state.groups.reduce((a, g) => a + (g.__count || 0), 0); }
    get cardCols() {
        return ['name', 'display_name', 'partner_id', 'date', 'amount_total']
            .filter(c => this.state.fields[c]);
    }
    cardTitle(r) {
        const v = r.name || r.display_name;
        return Array.isArray(v) ? v[1] : (v || ('#' + r.id));
    }
    cardSub(r) {
        const bits = [];
        if (r.partner_id)   bits.push(Array.isArray(r.partner_id) ? r.partner_id[1] : r.partner_id);
        if (r.date)         bits.push(r.date);
        if (r.amount_total !== undefined && r.amount_total !== false)
            bits.push(this.fmtNum(r.amount_total));
        return bits.join(' · ');
    }

    async reload() {
        this.state.loading = true; this.state.error = ''; this.state.rows = {};
        const spec = this.groupSpec(this.state.groupby);
        this.state.spec = spec;
        try {
            this.state.groups = await this.readGroup([spec], [], []);
            // Cards are capped per column: a board is for scanning, and a
            // thousand-card column helps nobody.
            await Promise.all(this.state.groups.map(async (g, i) => {
                try {
                    this.state.rows[i] = await RpcService.call(
                        this.model, 'search_read', [g.__domain || [], this.cardCols], { limit: 20 });
                } catch (_) { this.state.rows[i] = []; }
            }));
        } catch (e) {
            this.state.groups = [];
            this.state.error = (e && e.message) || 'Grouping failed.';
        }
        this.state.loading = false;
    }

    async onGroup(ev) { this.state.groupby = ev.target.value; await this.reload(); }
    open(id) { if (this.props.onOpenForm) this.props.onOpenForm(id); }
}

// ============================================================
// PivotView — a cross-tab of two group keys and one measure
// ============================================================
class PivotView extends RvBase {
    static template = owl.xml`
        <div class="rv-wrap">
            <div class="rv-bar">
                <label class="rv-lbl">Rows</label>
                <select t-on-change="(ev) => this.onAxis('row', ev)">
                    <t t-foreach="state.groupable" t-as="g" t-key="'r'+g.name">
                        <option t-att-value="g.name" t-att-selected="g.name === state.rowBy" t-esc="g.label"/>
                    </t>
                </select>
                <label class="rv-lbl">Columns</label>
                <select t-on-change="(ev) => this.onAxis('col', ev)">
                    <option value="">(none)</option>
                    <t t-foreach="state.groupable" t-as="g" t-key="'c'+g.name">
                        <option t-att-value="g.name" t-att-selected="g.name === state.colBy" t-esc="g.label"/>
                    </t>
                </select>
                <label class="rv-lbl">Measure</label>
                <select t-on-change="onMeasure">
                    <option value="">Count</option>
                    <t t-foreach="state.measures" t-as="m" t-key="m.name">
                        <option t-att-value="m.name" t-att-selected="m.name === state.measure" t-esc="m.label"/>
                    </t>
                </select>
            </div>
            <t t-if="state.error"><div class="rv-error" t-esc="state.error"/></t>
            <t t-if="state.loading"><div class="rv-loading">Loading…</div></t>
            <div t-else="" class="rv-scroll">
                <table class="rv-pivot">
                    <thead>
                        <tr>
                            <th t-esc="rowLabel"/>
                            <th t-foreach="grid.cols" t-as="c" t-key="c" t-esc="c"/>
                            <th class="rv-tot">Total</th>
                        </tr>
                    </thead>
                    <tbody>
                        <tr t-foreach="grid.rows" t-as="r" t-key="r">
                            <th class="rv-rowhead" t-esc="r"/>
                            <td t-foreach="grid.cols" t-as="c" t-key="c"
                                t-esc="fmtCell(cellAt(grid, r, c))"/>
                            <td class="rv-tot" t-esc="fmtCell(grid.rowTot[r])"/>
                        </tr>
                        <tr class="rv-totrow">
                            <th>Total</th>
                            <td t-foreach="grid.cols" t-as="c" t-key="c" t-esc="fmtCell(grid.colTot[c])"/>
                            <td class="rv-tot" t-esc="fmtCell(grid.grand)"/>
                        </tr>
                    </tbody>
                </table>
                <div t-if="!grid.rows.length" class="rv-hint">No data.</div>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            fields: {}, groupable: [], measures: [],
            rowBy: '', colBy: '', measure: '', groups: [], loading: true, error: '',
        });
        this.init();
    }

    async init() {
        try {
            const { fields, groupable, measures } = await this.loadFields();
            Object.assign(this.state, { fields, groupable, measures });
            if (groupable.length) {
                this.state.rowBy = (groupable.find(g => g.name === 'state') || groupable[0]).name;
                const second = groupable.find(g => g.name !== this.state.rowBy);
                this.state.colBy = second ? second.name : '';
                this.state.measure = measures.length ? measures[0].name : '';
                await this.reload();
            } else this.state.error = 'This model has nothing to pivot on.';
        } catch (e) { this.state.error = (e && e.message) || 'Could not load fields.'; }
        this.state.loading = false;
    }

    get rowLabel() {
        const f = this.state.groupable.find(g => g.name === this.state.rowBy);
        return f ? f.label : '';
    }

    /**
     * Fold the flat group list into a cross-tab.
     * The cell key joins row and column with U+0001 rather than a comma —
     * a label containing a comma ("Kuala Lumpur, Malaysia") would otherwise
     * collide with a different cell.
     */
    get grid() {
        const rows = [], cols = [], cell = {}, rowTot = {}, colTot = {};
        let grand = 0;
        const m = this.state.measure;
        const rspec = this.state.rowSpec, cspec = this.state.colSpec;
        for (const g of this.state.groups) {
            const r = this.labelOf(this.keyOf(g, rspec));
            const c = cspec ? this.labelOf(this.keyOf(g, cspec)) : 'Total';
            const v = m ? (Number(g[m]) || 0) : (g.__count || 0);
            if (!rows.includes(r)) rows.push(r);
            if (!cols.includes(c)) cols.push(c);
            const k = this.cellKey(r, c);
            cell[k]   = (cell[k]   || 0) + v;
            rowTot[r] = (rowTot[r] || 0) + v;
            colTot[c] = (colTot[c] || 0) + v;
            grand += v;
        }
        rows.sort(); cols.sort();
        return { rows, cols, cell, rowTot, colTot, grand };
    }

    /**
     * Cell keys join the row and column labels. The separator must be something
     * no label can contain — a comma collides with "Kuala Lumpur, Malaysia" —
     * and it must never reach the template: OWL parses templates as XML, where a
     * control character is invalid and takes the whole component down at compile
     * time. So it is built here, in JS, and the template calls cellAt().
     */
    cellKey(r, c) { return String(r) + '\u0001' + String(c); }
    cellAt(grid, r, c) { return grid.cell[this.cellKey(r, c)]; }

    fmtCell(v) { return (v === undefined || v === null) ? '' : this.fmtNum(v); }

    async reload() {
        this.state.loading = true; this.state.error = '';
        const rspec = this.groupSpec(this.state.rowBy);
        const cspec = this.state.colBy ? this.groupSpec(this.state.colBy) : '';
        this.state.rowSpec = rspec; this.state.colSpec = cspec;
        try {
            this.state.groups = await this.readGroup(
                cspec ? [rspec, cspec] : [rspec],
                this.state.measure ? [this.state.measure] : [], []);
        } catch (e) {
            this.state.groups = [];
            this.state.error = (e && e.message) || 'Pivot failed.';
        }
        this.state.loading = false;
    }

    async onAxis(which, ev) {
        if (which === 'row') this.state.rowBy = ev.target.value;
        else                 this.state.colBy = ev.target.value;
        await this.reload();
    }
    async onMeasure(ev) { this.state.measure = ev.target.value; await this.reload(); }
}

// ============================================================
// GraphView — bar / line / pie, drawn as SVG
// ============================================================
class GraphView extends RvBase {
    static template = owl.xml`
        <div class="rv-wrap">
            <div class="rv-bar">
                <label class="rv-lbl">Group by</label>
                <select t-on-change="onGroup">
                    <t t-foreach="state.groupable" t-as="g" t-key="g.name">
                        <option t-att-value="g.name" t-att-selected="g.name === state.groupby" t-esc="g.label"/>
                    </t>
                </select>
                <t t-if="state.isDate">
                    <select t-on-change="onInterval">
                        <t t-foreach="['day','week','month','quarter','year']" t-as="iv" t-key="iv">
                            <option t-att-value="iv" t-att-selected="iv === state.interval" t-esc="iv"/>
                        </t>
                    </select>
                </t>
                <label class="rv-lbl">Measure</label>
                <select t-on-change="onMeasure">
                    <option value="">Count</option>
                    <t t-foreach="state.measures" t-as="m" t-key="m.name">
                        <option t-att-value="m.name" t-att-selected="m.name === state.measure" t-esc="m.label"/>
                    </t>
                </select>
                <span class="rv-spacer"/>
                <button t-foreach="['bar','line','pie']" t-as="k" t-key="k"
                        t-attf-class="rv-chip{{ state.kind === k ? ' active' : '' }}"
                        t-on-click="() => this.setKind(k)" t-esc="k"/>
            </div>
            <t t-if="state.error"><div class="rv-error" t-esc="state.error"/></t>
            <t t-if="state.loading"><div class="rv-loading">Loading…</div></t>
            <div t-else="" class="rv-chart">
                <t t-if="!points.length"><div class="rv-hint">No data.</div></t>
                <t t-else="">
                    <!-- Bar and line share a plot frame; pie does not. -->
                    <svg t-if="state.kind !== 'pie'" class="rv-svg" viewBox="0 0 900 380"
                         preserveAspectRatio="xMidYMid meet" role="img" t-att-aria-label="ariaLabel">
                        <g>
                            <t t-foreach="plot.ticks" t-as="tk" t-key="tk.v">
                                <line class="rv-grid" x1="70" t-att-y1="tk.y" x2="880" t-att-y2="tk.y"/>
                                <text class="rv-axis" x="62" t-att-y="tk.y + 4" text-anchor="end" t-esc="fmtNum(tk.v)"/>
                            </t>
                        </g>
                        <t t-if="state.kind === 'bar'">
                            <g>
                                <t t-foreach="plot.bars" t-as="b" t-key="b.i">
                                    <rect class="rv-bar-rect" t-att-x="b.x" t-att-y="b.y"
                                          t-att-width="b.w" t-att-height="b.h" rx="3"
                                          t-att-fill="hue(b.i)">
                                        <title t-esc="b.label + ': ' + fmtNum(b.v)"/>
                                    </rect>
                                </t>
                            </g>
                        </t>
                        <t t-else="">
                            <polyline class="rv-line" t-att-points="plot.linePoints"/>
                            <t t-foreach="plot.bars" t-as="b" t-key="b.i">
                                <circle class="rv-pt" t-att-cx="b.x + b.w / 2" t-att-cy="b.y" r="4">
                                    <title t-esc="b.label + ': ' + fmtNum(b.v)"/>
                                </circle>
                            </t>
                        </t>
                        <g>
                            <t t-foreach="plot.bars" t-as="b" t-key="'l'+b.i">
                                <text class="rv-xlabel" t-att-transform="b.labelTransform"
                                      text-anchor="end" t-esc="b.short"/>
                            </t>
                        </g>
                    </svg>

                    <svg t-else="" class="rv-svg" viewBox="0 0 900 380"
                         preserveAspectRatio="xMidYMid meet" role="img" t-att-aria-label="ariaLabel">
                        <t t-foreach="pie" t-as="s" t-key="s.i">
                            <path t-att-d="s.d" t-att-fill="hue(s.i)" stroke="var(--surface)" stroke-width="2">
                                <title t-esc="s.label + ': ' + fmtNum(s.v)"/>
                            </path>
                        </t>
                        <g>
                            <t t-foreach="pie" t-as="s" t-key="'lg'+s.i">
                                <rect t-att-x="600" t-att-y="40 + s.i * 22" width="11" height="11"
                                      rx="2" t-att-fill="hue(s.i)"/>
                                <text class="rv-legend" x="618" t-att-y="50 + s.i * 22"
                                      t-esc="s.short + '  ' + fmtNum(s.v)"/>
                            </t>
                        </g>
                    </svg>
                </t>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            fields: {}, groupable: [], measures: [],
            groupby: '', interval: 'month', measure: '', kind: 'bar',
            spec: '', isDate: false, groups: [], loading: true, error: '',
        });
        this.init();
    }

    async init() {
        try {
            const { fields, groupable, measures } = await this.loadFields();
            Object.assign(this.state, { fields, groupable, measures });
            if (groupable.length) {
                this.state.groupby = (groupable.find(g => g.name === 'state') || groupable[0]).name;
                this.state.measure = measures.length ? measures[0].name : '';
                await this.reload();
            } else this.state.error = 'This model has nothing to chart.';
        } catch (e) { this.state.error = (e && e.message) || 'Could not load fields.'; }
        this.state.loading = false;
    }

    get points() {
        const m = this.state.measure;
        return this.state.groups.map((g, i) => ({
            i,
            label: this.labelOf(this.keyOf(g, this.state.spec)),
            v: m ? (Number(g[m]) || 0) : (g.__count || 0),
        }));
    }
    get ariaLabel() {
        const f = this.state.groupable.find(g => g.name === this.state.groupby);
        return `${this.state.kind} chart by ${f ? f.label : ''}`;
    }
    short(s) { return s.length > 14 ? s.slice(0, 13) + '…' : s; }

    /** Bar/line geometry plus a rounded axis scale. */
    get plot() {
        const pts = this.points;
        const max = Math.max(...pts.map(p => p.v), 1);
        // Round the top of the scale up to something a human would pick.
        const mag  = Math.pow(10, Math.floor(Math.log10(max)));
        const top  = Math.ceil(max / mag) * mag;
        const X0 = 70, X1 = 880, Y0 = 30, Y1 = 300;
        const band = (X1 - X0) / Math.max(pts.length, 1);
        const y = v => Y1 - (v / top) * (Y1 - Y0);

        const ticks = [];
        for (let i = 0; i <= 4; i++) {
            const v = (top / 4) * i;
            ticks.push({ v, y: y(v) });
        }
        const bars = pts.map(p => {
            const w = Math.min(band * 0.62, 70);
            const x = X0 + band * p.i + (band - w) / 2;
            return {
                i: p.i, label: p.label, short: this.short(p.label), v: p.v,
                x, w, y: y(p.v), h: Math.max(Y1 - y(p.v), 1),
                // Rotated so a dozen labels do not overlap into mush.
                labelTransform: `translate(${x + w / 2 + 4} ${Y1 + 14}) rotate(-40)`,
            };
        });
        const linePoints = bars.map(b => `${b.x + b.w / 2},${b.y}`).join(' ');
        return { ticks, bars, linePoints };
    }

    /** Pie slices as SVG arc paths. */
    get pie() {
        const pts = this.points;
        const total = pts.reduce((a, p) => a + p.v, 0);
        if (!total) return [];
        const cx = 320, cy = 190, r = 140;
        let a0 = -Math.PI / 2;
        return pts.map(p => {
            const frac = p.v / total;
            const a1 = a0 + frac * Math.PI * 2;
            const big = frac > 0.5 ? 1 : 0;
            const x0 = cx + r * Math.cos(a0), y0 = cy + r * Math.sin(a0);
            const x1 = cx + r * Math.cos(a1), y1 = cy + r * Math.sin(a1);
            // A single slice covering everything cannot be drawn as an arc —
            // start and end coincide and the path collapses. Draw a circle.
            const d = frac >= 0.9999
                ? `M ${cx} ${cy - r} A ${r} ${r} 0 1 1 ${cx - 0.01} ${cy - r} Z`
                : `M ${cx} ${cy} L ${x0} ${y0} A ${r} ${r} 0 ${big} 1 ${x1} ${y1} Z`;
            a0 = a1;
            return { i: p.i, label: p.label, short: this.short(p.label), v: p.v, d };
        });
    }

    get isDateField() {
        const d = this.state.fields[this.state.groupby];
        return !!d && (d.type === 'date' || d.type === 'datetime');
    }

    async reload() {
        this.state.loading = true; this.state.error = '';
        this.state.isDate = this.isDateField;
        this.state.spec = this.groupSpec(this.state.groupby, this.state.interval);
        try {
            this.state.groups = await this.readGroup([this.state.spec],
                                                     this.state.measure ? [this.state.measure] : [], []);
        } catch (e) {
            this.state.groups = [];
            this.state.error = (e && e.message) || 'Chart failed.';
        }
        this.state.loading = false;
    }

    async onGroup(ev)    { this.state.groupby = ev.target.value; await this.reload(); }
    async onInterval(ev) { this.state.interval = ev.target.value; await this.reload(); }
    async onMeasure(ev)  { this.state.measure = ev.target.value; await this.reload(); }
    setKind(k) { this.state.kind = k; }
}

// ============================================================
// CalendarView — a month grid keyed on a date field
// ============================================================
class CalendarView extends RvBase {
    static template = owl.xml`
        <div class="rv-wrap">
            <div class="rv-bar">
                <label class="rv-lbl">Date field</label>
                <select t-on-change="onField">
                    <t t-foreach="state.dateFields" t-as="f" t-key="f.name">
                        <option t-att-value="f.name" t-att-selected="f.name === state.dateField" t-esc="f.label"/>
                    </t>
                </select>
                <button class="rv-nav" t-on-click="() => this.move(-1)">‹</button>
                <span class="rv-month" t-esc="monthLabel"/>
                <button class="rv-nav" t-on-click="() => this.move(1)">›</button>
                <button class="rv-chip" t-on-click="today">Today</button>
                <span class="rv-spacer"/>
                <span class="rv-total"><t t-esc="state.records.length"/> in view</span>
            </div>
            <t t-if="state.error"><div class="rv-error" t-esc="state.error"/></t>
            <t t-if="state.loading"><div class="rv-loading">Loading…</div></t>
            <div t-else="" class="rv-cal">
                <div class="rv-cal-head">
                    <div t-foreach="dayNames" t-as="d" t-key="d" t-esc="d"/>
                </div>
                <div class="rv-cal-grid">
                    <div t-foreach="cells" t-as="c" t-key="c.key"
                         t-attf-class="rv-cal-cell{{ c.outside ? ' outside' : '' }}{{ c.today ? ' today' : '' }}">
                        <div class="rv-cal-daynum" t-esc="c.day"/>
                        <div class="rv-cal-events">
                            <div class="rv-event" t-foreach="c.shown" t-as="r" t-key="r.id"
                                 t-on-click="() => this.open(r.id)" t-att-title="title(r)">
                                <t t-esc="title(r)"/>
                            </div>
                            <div class="rv-more" t-if="c.hidden > 0"
                                 t-att-title="c.hidden + ' more on this day'">
                                +<t t-esc="c.hidden"/> more
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    `;

    setup() {
        const now = new Date();
        this.state = owl.useState({
            fields: {}, dateFields: [], dateField: '',
            year: now.getFullYear(), month: now.getMonth(),
            records: [], loading: true, error: '',
        });
        this.init();
    }

    async init() {
        try {
            const { fields } = await this.loadFields();
            this.state.fields = fields;
            const df = [];
            for (const [name, d] of Object.entries(fields))
                if (d && (d.type === 'date' || d.type === 'datetime'))
                    df.push({ name, label: d.string || name });
            df.sort((a, b) => a.label.localeCompare(b.label));
            this.state.dateFields = df;
            if (df.length) {
                // Prefer the field a user would think of as "the" date.
                const pref = df.find(f => f.name === 'date') || df.find(f => f.name === 'date_order') || df[0];
                this.state.dateField = pref.name;
                await this.reload();
            } else this.state.error = 'This model has no date field to place on a calendar.';
        } catch (e) { this.state.error = (e && e.message) || 'Could not load fields.'; }
        this.state.loading = false;
    }

    get dayNames() { return ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']; }
    get monthLabel() {
        return new Date(this.state.year, this.state.month, 1)
            .toLocaleDateString(undefined, { month: 'long', year: 'numeric' });
    }
    pad(n) { return (n < 10 ? '0' : '') + n; }
    ymd(y, m, d) { return `${y}-${this.pad(m + 1)}-${this.pad(d)}`; }

    title(r) {
        const v = r.name || r.display_name;
        if (Array.isArray(v)) return v[1];
        return v || ('#' + r.id);
    }

    /** Six weeks starting on the Monday on or before the 1st. */
    get cells() {
        const { year, month } = this.state;
        const first = new Date(year, month, 1);
        const shift = (first.getDay() + 6) % 7;          // Monday-first
        const start = new Date(year, month, 1 - shift);
        const byDay = {};
        const fld = this.state.dateField;
        for (const r of this.state.records) {
            const raw = r[fld];
            if (!raw) continue;
            const key = String(raw).slice(0, 10);
            (byDay[key] = byDay[key] || []).push(r);
        }
        const todayKey = (() => { const n = new Date(); return this.ymd(n.getFullYear(), n.getMonth(), n.getDate()); })();
        // A busy day can hold hundreds of records — a general ledger puts most
        // of a month on a handful of dates. Rendering them all turned every cell
        // into an unreadable stack of slivers, so only a few are drawn and the
        // rest are counted.
        const PER_DAY = 4;
        const out = [];
        for (let i = 0; i < 42; i++) {
            const d = new Date(start.getFullYear(), start.getMonth(), start.getDate() + i);
            const key = this.ymd(d.getFullYear(), d.getMonth(), d.getDate());
            const recs = byDay[key] || [];
            out.push({
                key, day: d.getDate(),
                outside: d.getMonth() !== month,
                today: key === todayKey,
                records: recs,
                shown: recs.slice(0, PER_DAY),
                hidden: Math.max(recs.length - PER_DAY, 0),
            });
        }
        return out;
    }

    async reload() {
        this.state.loading = true; this.state.error = '';
        const { year, month, dateField } = this.state;
        // Fetch the whole visible grid, not just the month, so events in the
        // leading and trailing days are not mysteriously missing.
        const from = new Date(year, month, -6);
        const to   = new Date(year, month + 1, 7);
        const f = this.ymd(from.getFullYear(), from.getMonth(), from.getDate());
        const t = this.ymd(to.getFullYear(), to.getMonth(), to.getDate());
        const cols = ['name', 'display_name', dateField].filter(c => this.state.fields[c] || c === dateField);
        try {
            this.state.records = await RpcService.call(this.model, 'search_read',
                [[[dateField, '>=', f], [dateField, '<=', t]], cols], { limit: 500 });
        } catch (e) {
            this.state.records = [];
            this.state.error = (e && e.message) || 'Could not load records.';
        }
        this.state.loading = false;
    }

    async onField(ev) { this.state.dateField = ev.target.value; await this.reload(); }
    async move(delta) {
        let m = this.state.month + delta, y = this.state.year;
        if (m < 0)  { m = 11; y--; }
        if (m > 11) { m = 0;  y++; }
        this.state.month = m; this.state.year = y;
        await this.reload();
    }
    async today() {
        const n = new Date();
        this.state.year = n.getFullYear(); this.state.month = n.getMonth();
        await this.reload();
    }
    open(id) { if (this.props.onOpenForm) this.props.onOpenForm(id); }
}
