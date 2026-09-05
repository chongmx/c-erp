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
    // OWL resolves a sub-component at FIRST RENDER, so a class that renders
    // <M2OSelect/> without naming it here throws only when a user opens this
    // exact screen — never at load, and never in an API test.
    static components = { M2OSelect };

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
                <button t-on-click="openNewUnit">+ New unit</button>
                <button t-on-click="toggleView">
                    <t t-esc="state.view === 'grid' ? 'Table view' : 'Grid view'"/>
                </button>
                <button t-on-click="load">Refresh</button>
            </div>

            <!-- ONE dialog for both adding and editing.
                 Adding had no UI at all — this screen replaces the list view
                 for rental.unit, so there was no New button anywhere and a
                 lettings business could not add the thing it lets. Editing had
                 the mirror problem: clicking a unit called openUnit(), which
                 did nothing, so a typo in a code or a unit moved to another
                 zone could not be corrected from the only screen that shows
                 units. Two dialogs would be two sets of fields to keep in
                 step, so this is one, and its id decides which it is.

                 NB: no backticks anywhere in this template. The whole thing is
                 a JS template LITERAL, so a stray backtick — even inside an XML
                 comment — closes the string early, and the file dies with
                 "Unexpected identifier". The component then never defines, the
                 app never boots, and the browser reports it three layers away
                 as a screen that will not open. -->
            <div t-if="state.unitDlg" class="m2o-modal-back" t-on-click="closeUnitDlg">
                <div class="m2o-modal" t-on-click.stop="() => {}">
                    <div class="m2o-modal-head">
                        <span t-esc="state.unitDlg.id ? 'Edit ' + state.unitDlg.origCode : 'New unit'"/>
                        <button class="m2o-x" t-on-click="closeUnitDlg">×</button>
                    </div>
                    <div class="ru-dlg">
                        <div class="ru-dlg-two">
                            <label>Code
                                <input class="form-input" data-nu="code"
                                       t-att-value="state.unitDlg.code"
                                       t-on-input="ev => { this.state.unitDlg.code = ev.target.value; }"
                                       placeholder="e.g. A-101"/></label>
                            <label>Name
                                <input class="form-input" data-nu="name"
                                       t-att-value="state.unitDlg.name"
                                       t-on-input="ev => { this.state.unitDlg.name = ev.target.value; }"/></label>
                        </div>
                        <!-- Type is a SEARCH box, not a <select> of a prefetch.
                             The prefetch was loaded once when the screen opened,
                             capped at 200 and unordered, so a unit type created
                             afterwards — on the Unit Types screen, in another
                             tab, by anyone — simply was not in this list, and
                             there was no way to search for it. M2OSelect asks
                             the server on every focus, so a type created a
                             moment ago is found by typing part of its name. -->
                        <label>Type
                            <M2OSelect model="'rental.unit.type'" label="'Unit Type'"
                                       placeholder="'Search a unit type…'"
                                       value="state.unitDlg.type_id"
                                       fields="['code']"
                                       searchFields="['code']"
                                       format="(r) => (r.code ? r.code + ' — ' : '') + (r.name || '')"
                                       onSelect="(id) => { this.state.unitDlg.type_id = id; }"/></label>
                        <div class="ru-dlg-three">
                            <label>Site
                                <input class="form-input" data-nu="site"
                                       t-att-value="state.unitDlg.site"
                                       t-on-input="ev => { this.state.unitDlg.site = ev.target.value; }"/></label>
                            <label>Zone
                                <input class="form-input" data-nu="zone"
                                       t-att-value="state.unitDlg.zone"
                                       t-on-input="ev => { this.state.unitDlg.zone = ev.target.value; }"
                                       placeholder="e.g. Ground floor"/></label>
                            <label>Floor
                                <input class="form-input" data-nu="floor"
                                       t-att-value="state.unitDlg.floor"
                                       t-on-input="ev => { this.state.unitDlg.floor = ev.target.value; }"/></label>
                        </div>
                        <label>Service status
                            <select class="form-input" data-nu="state"
                                    t-on-change="ev => { this.state.unitDlg.state = ev.target.value; }">
                                <t t-foreach="allStates" t-as="s" t-key="s.key">
                                    <option t-att-value="s.key"
                                            t-att-selected="state.unitDlg.state === s.key ? true : undefined"
                                            t-esc="s.label"/>
                                </t>
                            </select></label>
                        <p class="ru-dlg-note">
                            Available, reserved and occupied are <strong>derived</strong> from
                            this unit's contracts and are recomputed automatically. Only
                            <strong>maintenance</strong> and <strong>retired</strong> stick —
                            they are operator facts, and they take a unit out of the lettable
                            stock until you put it back.
                        </p>
                        <label>Notes
                            <textarea class="form-input" data-nu="notes" rows="2"
                                      t-on-input="ev => { this.state.unitDlg.notes = ev.target.value; }"><t t-esc="state.unitDlg.notes"/></textarea></label>
                        <div t-if="state.unitDlg.error" class="error" t-esc="state.unitDlg.error"/>
                    </div>
                    <div class="m2o-modal-foot">
                        <button t-on-click="closeUnitDlg">Cancel</button>
                        <span/>
                        <button class="btn btn-primary" t-on-click="saveUnit"
                                t-att-disabled="state.unitDlg.saving ? true : undefined"
                                t-esc="state.unitDlg.id ? 'Save' : 'Create'"/>
                    </div>
                </div>
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
            unitDlg: null,   // the add/edit dialog, or null when closed
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
     * A many2one arrives as a BARE ID from this backend, not as the reference ERP's
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

    /** A blank dialog. */
    openNewUnit() {
        this.state.unitDlg = {
            id: 0, origCode: '', code: '', name: '', type_id: 0,
            site: '', zone: '', floor: '', state: 'available', notes: '',
            error: '', saving: false,
        };
        // The Type picker searches the server for itself, but the grid's type
        // FILTER and its type labels still come from the list fetched when this
        // screen opened. Refresh it here so a type added since then is not
        // missing from both — and so a unit created with that type is labelled
        // the moment the grid reloads, rather than showing a blank Type cell.
        this.reloadTypes();
    }

    /** The same dialog, filled from a unit. */
    editUnit(u) {
        this.state.unitDlg = {
            id:       u.id,
            origCode: u.code,
            code:     u.code || '',
            name:     u.name || '',
            type_id:  this.typeId(u) || 0,
            site:     u.site  || '',
            zone:     u.zone  || '',
            floor:    u.floor || '',
            state:    u.state || 'available',
            notes:    u.notes || '',
            error:    '', saving: false,
        };
        this.reloadTypes();
    }

    async reloadTypes() {
        try {
            const types = await RpcService.call('rental.unit.type', 'search_read', [[]],
                { fields: ['id', 'name', 'default_rate'], limit: 200, order: 'name ASC' });
            this.state.types = types || [];
        } catch (_) { /* the picker still works; only the filter goes stale */ }
    }
    closeUnitDlg() { this.state.unitDlg = null; }

    /** Create the unit, then reload so it appears in the grid immediately. */
    /**
     * Create or update, depending on whether the dialog carries an id.
     *
     * One path, so the two can never drift into accepting different fields —
     * which is how "I can set a zone when I create a unit but not afterwards"
     * happens.
     */
    async saveUnit() {
        const nu = this.state.unitDlg;
        if (!nu || nu.saving) return;
        const code = (nu.code || '').trim();
        if (!code) { nu.error = 'A code is required.'; return; }
        nu.saving = true; nu.error = '';
        try {
            const vals = {
                code,
                name:  (nu.name  || '').trim() || code,
                site:  (nu.site  || '').trim(),
                zone:  (nu.zone  || '').trim(),
                floor: (nu.floor || '').trim(),
                notes: nu.notes || '',
                state: nu.state || 'available',
            };
            // M2OSelect reports a NUMBER, and 0 means cleared. Sending
            // type_id: 0 would be a foreign key to nothing; `false` is how this
            // ORM spells "no relation" (normalizeForDb_).
            vals.type_id = nu.type_id ? (parseInt(nu.type_id, 10) || false) : false;

            if (nu.id) await RpcService.call('rental.unit', 'write', [[nu.id], vals], {});
            else       await RpcService.call('rental.unit', 'create', [vals], {});

            this.state.unitDlg = null;
            await this.load();
        } catch (e) {
            nu.error  = (e && e.message) || String(e);
            nu.saving = false;
        }
    }

    /**
     * Clicking a unit opens it for editing.
     *
     * A parent may still claim the click — that hook was here for the phase
     * that opens the unit's contract — but with nothing supplying it the click
     * used to do nothing at all, on the only screen that lists units. Editing
     * is the useful default until something better claims it.
     */
    openUnit(u) {
        if (this.props && typeof this.props.onOpenUnit === 'function') {
            this.props.onOpenUnit(u);
            return;
        }
        this.editUnit(u);
    }
}
