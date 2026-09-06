/**
 * M2OSelect.js — the one many-to-one picker, used everywhere.
 *
 * WHY THIS EXISTS. Every m2o combobox in the ERP was a `<select>` filled once
 * on form open by
 *
 *     search_read([[]], fields:['id','name'], limit: 200)     // no order
 *
 * which has three separate defects, all of which a user meets as "my record is
 * not in the list":
 *
 *   1. TRUNCATION. No ORDER BY means the server's default `id ASC`, so the
 *      dropdown holds the OLDEST 200 rows. Add the 201st company and it is
 *      simply absent. Measured: with 284 partners, a freshly created company
 *      does not appear at all.
 *
 *   2. STALENESS. The fetch happens once. Create a company in another screen,
 *      come back, and the dropdown still shows what it loaded at open.
 *
 *   3. SILENT VALUE LOSS — the dangerous one. A `<select>` whose current value
 *      is not among its options falls back to the first option ("—", id 0).
 *      Open a record whose partner is outside the window, press Save, and the
 *      link is quietly cleared. No error; the field just empties.
 *
 * This component fixes all three by never holding the whole table:
 *   - the CURRENT value is always resolved by id and always displayable, so it
 *     can never be lost by not being in a page of results;
 *   - typing searches the SERVER (`name ilike`), so a new record is found the
 *     moment it exists;
 *   - "Browse all…" opens a paged dialog with the total count, so a thousand
 *     companies are as usable as five.
 *
 * USAGE
 *   <M2OSelect model="'res.partner'"
 *              value="state.record.partner_id"
 *              label="'Customer'"
 *              domain="[['is_company','=',true]]"
 *              onSelect.bind="onPartnerPicked"/>
 *
 *   onSelect receives (id, displayName). id is 0 when cleared.
 */

/**
 * Models that carry a STORED `display_name` column.
 *
 * res.partner's is "Carol, Big Carrots" — the person and the company they work
 * for — because "Carol" alone does not identify anyone in a customer list, and
 * picking the wrong Carol files a contract against the wrong company.
 *
 * This is a list rather than "just always ask for display_name" because the two
 * uses of the name are not equally forgiving. Reading it is harmless — the
 * server drops unregistered columns from a SELECT — but FILTERING on it is not:
 * a domain naming a column the model has not registered is rejected outright
 * (S-49), so a blanket `display_name ilike` would break the picker on every
 * other model in the app.
 *
 * Add a model here when it gains a stored display_name, not before.
 */
const M2O_DISPLAY_NAME_MODELS = ['res.partner'];

const M2O_PAGE     = 20;   // rows in the inline dropdown
const M2O_MODAL    = 50;   // rows per page in the browse dialog
const M2O_DEBOUNCE = 150;  // ms before a keystroke becomes a query

class M2OSelect extends owl.Component {
    static template = owl.xml`
        <div class="m2o" t-att-data-model="props.model">
            <div class="m2o-control">
                <input t-ref="input"
                       t-attf-class="form-input m2o-input{{ state.selectedId ? ' has-value' : '' }}"
                       t-att-placeholder="props.placeholder || 'Type to search…'"
                       t-att-value="state.text"
                       t-att-disabled="props.readonly ? true : undefined"
                       t-att-data-m2o="props.model"
                       t-on-input="onType"
                       t-on-keydown="onKey"
                       t-on-focus="onFocus"
                       t-on-blur="onBlur"/>
                <button class="m2o-clear" t-if="state.selectedId and !props.readonly"
                        t-on-mousedown.prevent="clear" title="Clear">×</button>
            </div>

            <div class="m2o-pop" t-if="state.open">
                <t t-if="state.loading">
                    <div class="m2o-note">Searching…</div>
                </t>
                <t t-elif="!state.options.length">
                    <div class="m2o-note">No match for “<t t-esc="state.query"/>”</div>
                </t>
                <t t-else="">
                    <t t-foreach="state.options" t-as="o" t-key="o.id">
                        <div t-attf-class="m2o-opt{{ o.id === state.selectedId ? ' sel' : '' }}"
                             t-on-mousedown.prevent="() => this.pick(o)">
                            <t t-esc="o.display"/>
                        </div>
                    </t>
                </t>
                <div class="m2o-more" t-if="state.total > state.options.length">
                    <span><t t-esc="state.total - state.options.length"/> more…</span>
                    <button class="m2o-browse" t-on-mousedown.prevent="openBrowser">Browse all</button>
                </div>
            </div>

            <!-- Browse dialog: for when scrolling a dropdown is the wrong tool -->
            <div class="m2o-modal-back" t-if="state.browsing" t-on-click="closeBrowser">
                <div class="m2o-modal" t-on-click.stop="() => {}">
                    <div class="m2o-modal-head">
                        <span>Select <t t-esc="props.label || props.model"/></span>
                        <button class="m2o-x" t-on-click="closeBrowser">×</button>
                    </div>
                    <input class="form-input m2o-modal-search" placeholder="Type to filter…"
                           t-att-value="state.query" t-on-input="onModalType"/>
                    <div class="m2o-modal-list">
                        <t t-if="state.loading"><div class="m2o-note">Searching…</div></t>
                        <t t-elif="!state.page.length"><div class="m2o-note">Nothing matches.</div></t>
                        <t t-else="">
                            <t t-foreach="state.page" t-as="o" t-key="o.id">
                                <div t-attf-class="m2o-row{{ o.id === state.selectedId ? ' sel' : '' }}"
                                     t-on-click="() => this.pick(o, true)">
                                    <t t-esc="o.display"/>
                                </div>
                            </t>
                        </t>
                    </div>
                    <div class="m2o-modal-foot">
                        <button t-att-disabled="state.offset === 0 ? true : undefined"
                                t-on-click="prevPage">‹ Prev</button>
                        <span>
                            <t t-esc="state.total ? state.offset + 1 : 0"/>–<t
                               t-esc="Math.min(state.offset + state.page.length, state.total)"/>
                            of <t t-esc="state.total"/>
                        </span>
                        <button t-att-disabled="state.offset + state.page.length >= state.total ? true : undefined"
                                t-on-click="nextPage">Next ›</button>
                    </div>
                </div>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({
            text: '', query: '', options: [], page: [],
            selectedId: 0, total: 0, offset: 0,
            open: false, browsing: false, loading: false,
        });
        this._timer = null;
        this._seq   = 0;          // drops the reply of a superseded search
        this.inputRef = owl.useRef('input');
        owl.onWillStart(() => this.syncValue());
        // A parent that swaps the record must not leave the old label showing.
        owl.onWillUpdateProps(next => this.syncValue(next));
        // t-att-value writes the ATTRIBUTE, and a browser stops mirroring the
        // attribute into the displayed value as soon as the user has typed in
        // the box ("dirty value flag"). Without this, picking an option would
        // leave the half-typed search term on screen while the real value
        // silently differed. Push the property directly after every patch.
        owl.onMounted(() => this.syncInput());
        owl.onPatched(() => this.syncInput());
        owl.onWillUnmount(() => clearTimeout(this._timer));
    }

    syncInput() {
        const el = this.inputRef.el;
        if (el && el.value !== this.state.text) el.value = this.state.text;
    }

    // --- the current value -------------------------------------------------
    /**
     * Resolve whatever the parent gave us into an id + a human label.
     *
     * The label is fetched BY ID, never looked up in a page of search results.
     * That is the whole defence against defect 3: a value outside the current
     * window is still shown correctly and still round-trips on save.
     */
    async syncValue(props) {
        const p = props || this.props;
        const raw = p.value;
        let id = 0, display = '';
        if (Array.isArray(raw))            { id = raw[0] || 0; display = raw[1] || ''; }
        else if (typeof raw === 'number')  { id = raw; }
        else if (typeof raw === 'string')  { id = parseInt(raw, 10) || 0; }
        else if (raw && typeof raw === 'object' && raw.id) { id = raw.id; display = raw.name || ''; }

        // Same id, already labelled, and the box is not mid-edit: nothing to do.
        // Without this a parent re-render would fire a `read` per keystroke.
        if (id && id === this.state.selectedId && this.state.text && !this.state.open) return;

        this.state.selectedId = id || 0;
        if (!id) { this.state.text = ''; return; }
        // A [id, name] pair from `read` already carries the label, but only the
        // bare name — re-read when this field formats its own label, so the
        // selected row reads the same as the rows in the dropdown. Same for a
        // model with a stored display_name: a pair built elsewhere may carry
        // "Carol" while every row in the dropdown reads "Carol, Big Carrots",
        // and a field that disagrees with its own list looks like a wrong value.
        if (display && typeof p.format !== 'function' && !this.hasDisplayName) {
            this.state.text = display; return;
        }
        try {
            const recs = await RpcService.call(p.model, 'read', [[id], this.readFields()], {});
            const rec = Array.isArray(recs) ? recs[0] : null;
            this.state.text = rec ? this.label(rec) : (display || `#${id}`);
        } catch (_) {
            // Never blank the box on a failed lookup — an empty field reads as
            // "nothing selected" and invites the user to overwrite a good value.
            this.state.text = `#${id}`;
        }
    }

    // --- searching ---------------------------------------------------------
    baseDomain() {
        const d = this.props.domain;
        return Array.isArray(d) ? JSON.parse(JSON.stringify(d)) : [];
    }

    /**
     * The domain for a typed term.
     *
     * By default only `name` is searched. `searchFields` widens that, because
     * some records are known by something else entirely: a rental unit is
     * "A-101", its code, and its name is "Unit A1" — typing the code found
     * nothing at all, which made the unit picker useless for units.
     */
    searchDomain(term) {
        const dom = this.baseDomain();
        const t = (term || '').trim();
        if (!t) return dom;
        const extra = Array.isArray(this.props.searchFields) ? this.props.searchFields : [];
        // Where the label is stored, search it too: typing "Big Carrots" should
        // find the people who work there, not just the company itself.
        const cols = ['name'];
        if (this.hasDisplayName) cols.push('display_name');
        for (const c of extra) if (!cols.includes(c)) cols.push(c);
        // Prefix notation: N-1 ORs in front of N leaves.
        for (let i = 0; i < cols.length - 1; i++) dom.push('|');
        for (const c of cols) dom.push([c, 'ilike', t]);
        return dom;
    }

    /**
     * Which columns to read, and how to turn a record into one line of text.
     *
     * Several of the lists this widget replaced were not showing a bare `name`
     * — accounts read "1100 Debtors", locations show their full path. Without
     * `fields`/`format` the conversion would have silently downgraded those
     * labels, which is exactly the kind of quiet loss this whole change exists
     * to stop.
     */
    readFields() {
        const extra = Array.isArray(this.props.fields) ? this.props.fields : [];
        const base  = ['id', 'name'];
        if (this.hasDisplayName) base.push('display_name');
        return [...base, ...extra.filter(f => !base.includes(f))];
    }

    /** Does this model store the composed label? See M2O_DISPLAY_NAME_MODELS. */
    get hasDisplayName() {
        return M2O_DISPLAY_NAME_MODELS.includes(this.props.model);
    }

    /**
     * One record, one line of text.
     *
     * An explicit `format` still wins — a caller that wants "A-101 — Unit A1"
     * asked for it. Otherwise the model's own stored display_name is preferred
     * over the bare name, which is what turns every partner picker in the app
     * into "Carol, Big Carrots" without touching a single call site. There are
     * more than forty of them and several render a model chosen at runtime
     * (`model="f.relation"` on the generic form), so per-site formatting could
     * never have covered them all.
     */
    label(rec) {
        if (typeof this.props.format === 'function') {
            const s = this.props.format(rec);
            if (s) return String(s);
        }
        return rec.display_name || rec.name || `#${rec.id}`;
    }

    async fetch(term, offset, limit) {
        const domain = this.searchDomain(term);
        const [recs, total] = await Promise.all([
            RpcService.call(this.props.model, 'search_read', [domain],
                { fields: this.readFields(), limit, offset, order: 'name ASC' }),
            RpcService.call(this.props.model, 'search_count', [domain], {}),
        ]);
        return {
            rows: (Array.isArray(recs) ? recs : []).map(r => ({
                id: r.id, display: this.label(r),
            })),
            total: typeof total === 'number' ? total : 0,
        };
    }

    async runSearch() {
        const seq = ++this._seq;
        this.state.loading = true;
        try {
            const { rows, total } = await this.fetch(this.state.query, 0, M2O_PAGE);
            if (seq !== this._seq) return;      // a later keystroke already won
            this.state.options = rows;
            this.state.total   = total;
        } catch (_) {
            if (seq !== this._seq) return;
            this.state.options = []; this.state.total = 0;
        } finally {
            if (seq === this._seq) this.state.loading = false;
        }
    }

    onType(e) {
        this.state.query = e.target.value;
        this.state.text  = e.target.value;
        this.state.open  = true;
        clearTimeout(this._timer);
        this._timer = setTimeout(() => this.runSearch(), M2O_DEBOUNCE);
    }

    onKey(e) {
        if (e.key === 'Escape') { this.state.open = false; this.syncValue(); }
        else if (e.key === 'Enter') {
            // Enter on a single unambiguous match selects it, so a fast typist
            // never has to reach for the mouse.
            e.preventDefault();
            if (this.state.options.length === 1) this.pick(this.state.options[0]);
        }
    }

    onFocus() {
        this.state.open  = true;
        this.state.query = '';
        this.runSearch();          // always fresh: fixes the staleness defect
    }

    onBlur() {
        // Let a mousedown on an option win the race against blur.
        setTimeout(() => {
            if (!this.state.browsing) {
                this.state.open = false;
                this.syncValue();  // restore the label if they typed and gave up
            }
        }, 150);
    }

    // --- choosing ----------------------------------------------------------
    pick(opt, fromModal) {
        this.state.selectedId = opt.id;
        this.state.text  = opt.display;
        this.state.query = '';
        this.state.open  = false;
        if (fromModal) this.state.browsing = false;
        if (this.props.onSelect) this.props.onSelect(opt.id, opt.display);
    }

    clear() {
        this.state.selectedId = 0;
        this.state.text  = '';
        this.state.query = '';
        this.state.open  = false;
        if (this.props.onSelect) this.props.onSelect(0, '');
    }

    // --- the browse dialog -------------------------------------------------
    async openBrowser() {
        this.state.browsing = true;
        this.state.open     = false;
        this.state.offset   = 0;
        await this.loadPage();
    }

    closeBrowser() { this.state.browsing = false; this.syncValue(); }

    onModalType(e) {
        this.state.query  = e.target.value;
        this.state.offset = 0;
        clearTimeout(this._timer);
        this._timer = setTimeout(() => this.loadPage(), M2O_DEBOUNCE);
    }

    async loadPage() {
        this.state.loading = true;
        try {
            const { rows, total } = await this.fetch(this.state.query, this.state.offset, M2O_MODAL);
            this.state.page  = rows;
            this.state.total = total;
        } catch (_) {
            this.state.page = []; this.state.total = 0;
        } finally {
            this.state.loading = false;
        }
    }

    prevPage() {
        this.state.offset = Math.max(0, this.state.offset - M2O_MODAL);
        this.loadPage();
    }
    nextPage() {
        if (this.state.offset + M2O_MODAL >= this.state.total) return;
        this.state.offset += M2O_MODAL;
        this.loadPage();
    }
}

M2OSelect.props = {
    model:       { type: String },
    value:       { optional: true },
    label:       { type: String, optional: true },
    domain:      { type: Array,  optional: true },
    fields:      { type: Array,  optional: true },   // extra columns to read
    searchFields:{ type: Array,  optional: true },   // columns a typed term also matches
    format:      { type: Function, optional: true }, // record -> label
    placeholder: { type: String, optional: true },
    readonly:    { type: Boolean, optional: true },
    onSelect:    { type: Function, optional: true },
};
