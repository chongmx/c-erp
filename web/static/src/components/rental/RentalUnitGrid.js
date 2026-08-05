/**
 * RentalUnitGrid.js — the visual facility map (docs/046 §4).
 *
 * The one screen a storage business lives in: every locker and room laid
 * out by zone, coloured by state.
 *
 * Loaded before app.js, so owl globals are NOT destructured yet — this
 * uses the fully qualified `owl.Component` / `owl.xml` form, matching the
 * other files in components/.
 *
 * Rules this view follows, from docs/046 §9:
 *   - state is colour PLUS a glyph PLUS a text label, never colour alone
 *   - per-mark hover tooltips (a grid cell is a heatmap-shaped mark)
 *   - hit target is the whole cell, larger than the visible mark
 *   - filters in one row above the grid
 *   - a table view is always available — it is the accessible
 *     alternative and the relief for the light-mode contrast WARN
 */

const RENTAL_UNIT_STATES = [
    // Fixed slot order. Never cycled, never reordered by count.
    { key: 'occupied',    label: 'Occupied',    glyph: '■', cls: 'is-occupied'    },
    { key: 'available',   label: 'Available',   glyph: '□', cls: 'is-available'   },
    { key: 'reserved',    label: 'Reserved',    glyph: '▤', cls: 'is-reserved'    },
    { key: 'maintenance', label: 'Maintenance', glyph: '⚠', cls: 'is-maintenance' },
    { key: 'retired',     label: 'Retired',     glyph: '✖', cls: 'is-retired'     },
];

class RentalUnitGrid extends owl.Component {
    static template = owl.xml`
        <div class="rental-grid-wrap viz-root">

            <div class="rental-filters">
                <select t-model="state.fZone" t-on-change="applyFilters">
                    <option value="">All zones</option>
                    <t t-foreach="zones" t-as="z" t-key="z">
                        <option t-att-value="z"><t t-esc="z or '(no zone)'"/></option>
                    </t>
                </select>
                <select t-model="state.fType" t-on-change="applyFilters">
                    <option value="">All types</option>
                    <t t-foreach="state.types" t-as="ty" t-key="ty.id">
                        <option t-att-value="ty.id"><t t-esc="ty.name"/></option>
                    </t>
                </select>
                <select t-model="state.fState" t-on-change="applyFilters">
                    <option value="">All states</option>
                    <t t-foreach="allStates" t-as="s" t-key="s.key">
                        <option t-att-value="s.key"><t t-esc="s.label"/></option>
                    </t>
                </select>
                <input placeholder="code contains…" t-model="state.fCode"
                       t-on-input="applyFilters"/>
                <span class="spacer"/>
                <button t-on-click="toggleView">
                    <t t-esc="state.view === 'grid' ? 'Table view' : 'Grid view'"/>
                </button>
                <button t-on-click="load">Refresh</button>
            </div>

            <t t-if="state.loading">
                <div class="rental-empty">Loading units…</div>
            </t>
            <t t-elif="state.error">
                <div class="rental-empty"><t t-esc="state.error"/></div>
            </t>
            <t t-elif="!state.units.length">
                <div class="rental-empty">
                    No units yet. Add one under Rental → Configuration.
                </div>
            </t>
            <t t-else="">

                <div class="rental-summary">
                    <t t-foreach="allStates" t-as="s" t-key="s.key">
                        <div t-if="counts[s.key]">
                            <span class="n"><t t-esc="counts[s.key]"/></span>
                            <t t-esc="s.label"/>
                        </div>
                    </t>
                    <div>
                        <span class="n"><t t-esc="occupancyPct"/>%</span>
                        Occupancy
                    </div>
                </div>

                <div class="rental-legend">
                    <t t-foreach="allStates" t-as="s" t-key="s.key">
                        <span class="item">
                            <span class="swatch"
                                  t-attf-style="background: var(--u-{{s.key}})"/>
                            <span t-esc="s.glyph"/>
                            <span t-esc="s.label"/>
                        </span>
                    </t>
                </div>

                <t t-if="state.view === 'grid'">
                    <t t-foreach="grouped" t-as="g" t-key="g.zone">
                        <div class="rental-zone">
                            <h3><t t-esc="g.zone"/> — <t t-esc="g.units.length"/> units</h3>
                            <div class="rental-grid">
                                <t t-foreach="g.units" t-as="u" t-key="u.id">
                                    <button class="rental-cell"
                                            t-att-class="cellClass(u)"
                                            t-on-mouseenter="ev => this.showTip(ev, u)"
                                            t-on-mousemove="ev => this.moveTip(ev)"
                                            t-on-mouseleave="hideTip"
                                            t-on-focus="ev => this.showTip(ev, u)"
                                            t-on-blur="hideTip"
                                            t-on-click="() => this.openUnit(u)"
                                            t-att-aria-label="ariaFor(u)">
                                        <span class="top">
                                            <span class="code" t-esc="u.code"/>
                                            <span class="glyph" t-esc="glyphFor(u)"/>
                                        </span>
                                        <span class="label" t-esc="u.state"/>
                                    </button>
                                </t>
                            </div>
                        </div>
                    </t>
                </t>

                <t t-else="">
                    <table class="rental-table">
                        <thead>
                            <tr>
                                <th>Code</th><th>Name</th><th>Type</th>
                                <th>Zone</th><th>State</th>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="filtered" t-as="u" t-key="u.id">
                                <tr t-on-click="() => this.openUnit(u)" style="cursor:pointer">
                                    <td t-esc="u.code"/>
                                    <td t-esc="u.name"/>
                                    <td t-esc="typeName(u)"/>
                                    <td t-esc="u.zone or '—'"/>
                                    <td>
                                        <span class="dot"
                                              t-attf-style="background: var(--u-{{u.state}})"/>
                                        <t t-esc="glyphFor(u)"/>
                                        <t t-esc="' ' + u.state"/>
                                    </td>
                                </tr>
                            </t>
                        </tbody>
                    </table>
                </t>
            </t>

            <t t-if="state.tip.show">
                <div class="rental-tip"
                     t-attf-style="left: {{state.tip.x}}px; top: {{state.tip.y}}px">
                    <div class="t-code"><t t-esc="state.tip.code"/></div>
                    <t t-foreach="state.tip.rows" t-as="r" t-key="r.k">
                        <div class="t-row"><t t-esc="r.k"/>: <b t-esc="r.v"/></div>
                    </t>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            units: [], types: [], loading: true, error: '',
            view: 'grid',
            fZone: '', fType: '', fState: '', fCode: '',
            tip: { show: false, x: 0, y: 0, code: '', rows: [] },
        });
        owl.onWillStart(() => this.load());
    }

    get allStates() { return RENTAL_UNIT_STATES; }

    async load() {
        this.state.loading = true;
        this.state.error = '';
        try {
            // PERF-F: capped. A facility with more units than this needs
            // pagination, not a bigger number.
            const units = await RpcService.call('rental.unit', 'search_read', [[]], {
                fields: ['id', 'code', 'name', 'type_id', 'zone', 'floor', 'site', 'state', 'notes'],
                limit: 1000,
            });
            const types = await RpcService.call('rental.unit.type', 'search_read', [[]], {
                fields: ['id', 'name', 'default_rate'], limit: 200,
            });
            this.state.units = units || [];
            this.state.types = types || [];
        } catch (e) {
            this.state.error = (e && e.message) ? e.message : 'Could not load units.';
        } finally {
            this.state.loading = false;
        }
    }

    // --- filtering -------------------------------------------------
    applyFilters() { /* state is reactive; getters recompute */ }

    get filtered() {
        const { fZone, fType, fState, fCode } = this.state;
        const code = (fCode || '').trim().toLowerCase();
        return this.state.units.filter(u => {
            if (fZone && (u.zone || '') !== fZone) return false;
            if (fState && u.state !== fState) return false;
            if (code && !(u.code || '').toLowerCase().includes(code)) return false;
            if (fType && String(this.typeId(u)) !== String(fType)) return false;
            return true;
        });
    }

    get zones() {
        const set = new Set(this.state.units.map(u => u.zone || ''));
        return [...set].sort();
    }

    get grouped() {
        const by = new Map();
        for (const u of this.filtered) {
            const z = u.zone || 'Unzoned';
            if (!by.has(z)) by.set(z, []);
            by.get(z).push(u);
        }
        return [...by.entries()]
            .sort((a, b) => a[0].localeCompare(b[0]))
            .map(([zone, units]) => ({
                zone,
                units: units.sort((a, b) => (a.code || '').localeCompare(b.code || '')),
            }));
    }

    get counts() {
        const c = {};
        for (const s of RENTAL_UNIT_STATES) c[s.key] = 0;
        for (const u of this.filtered) if (c[u.state] !== undefined) c[u.state]++;
        return c;
    }

    get occupancyPct() {
        // Retired units are not stock, so they are excluded from the
        // denominator — counting them would understate occupancy forever.
        const c = this.counts;
        const lettable = c.occupied + c.available + c.reserved + c.maintenance;
        if (!lettable) return 0;
        return Math.round((c.occupied / lettable) * 100);
    }

    // --- presentation ----------------------------------------------
    stateDef(u) {
        return RENTAL_UNIT_STATES.find(s => s.key === u.state) || RENTAL_UNIT_STATES[4];
    }
    cellClass(u)  { return this.stateDef(u).cls; }
    glyphFor(u)   { return this.stateDef(u).glyph; }
    ariaFor(u)    { return `Unit ${u.code}, ${this.stateDef(u).label}`; }

    /**
     * A many2one arrives as a BARE ID from this backend, not as Odoo's
     * [id, label] pair — see formatCell() in app.js, which copes with
     * both. Assuming the pair here rendered a blank Type column and
     * silently broke the type filter, with no error anywhere.
     *
     * Both shapes are accepted so this keeps working if the server-side
     * convention ever changes.
     */
    typeId(u) {
        if (Array.isArray(u.type_id)) return u.type_id[0];
        return (typeof u.type_id === 'number') ? u.type_id : 0;
    }

    /** Label resolved from the types already loaded, not from the row. */
    typeName(u) {
        if (Array.isArray(u.type_id) && u.type_id[1]) return u.type_id[1];
        const t = this.state.types.find(t => t.id === this.typeId(u));
        return t ? t.name : '';
    }

    toggleView() { this.state.view = this.state.view === 'grid' ? 'table' : 'grid'; }

    // --- tooltip ---------------------------------------------------
    showTip(ev, u) {
        const rows = [
            { k: 'State', v: this.stateDef(u).label },
            { k: 'Type',  v: this.typeName(u) || '—' },
            { k: 'Zone',  v: u.zone || '—' },
        ];
        if (u.name)  rows.push({ k: 'Name',  v: u.name });
        if (u.notes) rows.push({ k: 'Notes', v: u.notes });
        this.state.tip = { show: true, x: 0, y: 0, code: u.code, rows };
        this.moveTip(ev);
    }
    moveTip(ev) {
        if (!this.state.tip.show) return;
        // Flip near the right/bottom edge so the tooltip never forces the
        // page to scroll sideways.
        const pad = 14;
        const w = 260, h = 130;
        let x = ev.clientX + pad;
        let y = ev.clientY + pad;
        if (x + w > window.innerWidth)  x = ev.clientX - w - pad;
        if (y + h > window.innerHeight) y = ev.clientY - h - pad;
        this.state.tip.x = Math.max(4, x);
        this.state.tip.y = Math.max(4, y);
    }
    hideTip() { this.state.tip.show = false; }

    openUnit(u) {
        // Phase 4 opens the contract. Until contracts have a form, the
        // click is a no-op rather than a dead link that appears to work.
        if (this.props && typeof this.props.onOpenUnit === 'function') {
            this.props.onOpenUnit(u);
        }
    }
}
