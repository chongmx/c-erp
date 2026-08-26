/**
 * PartLookup.js — the review desk for agent-proposed parts (docs/097).
 *
 * The lookup API stages results rather than applying them, because an agent
 * that reads datasheets will sometimes be confidently wrong and a wrong
 * resistance that lands silently is a part somebody solders. This screen is the
 * "disposes" half of that: it shows what was proposed, what c-erp distrusts
 * about it, and applies it only when a person says so.
 *
 * The paste panel is not a debugging leftover — it is how you drive the whole
 * pipeline before an agent exists, and how you reproduce a bad result later.
 */
class PartLookup extends owl.Component {
    static template = owl.xml`
    <div class="pl-shell">
        <div class="pl-head">
            <h2 class="pl-title">Part Lookup</h2>
            <span class="pl-sub">Proposals from the lookup agent. Nothing reaches the catalogue until you apply it.</span>
            <span class="pl-spacer"/>
            <button class="pl-btn ghost" t-on-click="() => this.setPaste(!state.paste)">
                <t t-esc="state.paste ? 'Close paste' : 'Paste a result'"/>
            </button>
            <button class="pl-btn ghost" t-on-click="reload">Refresh</button>
        </div>

        <t t-if="state.error"><div class="pl-error" t-esc="state.error"/></t>
        <t t-if="state.notice"><div class="pl-notice" t-esc="state.notice"/></t>

        <!-- Paste panel: submit a LookupResult by hand -->
        <div t-if="state.paste" class="pl-paste">
            <div class="pl-paste-head">
                <span>Paste a <b>LookupResult</b> (see docs/LOOKUP_API.md)</span>
                <span class="pl-spacer"/>
                <button class="pl-btn ghost" t-on-click="fillSample">Insert a sample</button>
                <button class="pl-btn primary" t-on-click="submitPaste" t-att-disabled="state.busy">Submit</button>
            </div>
            <textarea t-ref="paste" t-model="state.pasteText" spellcheck="false"
                      placeholder='{"query":"4.7k 0805 resistor","mpn":"...", "parameters":[...]}'/>
        </div>

        <div class="pl-body">
            <!-- queue -->
            <div class="pl-list">
                <div class="pl-filters">
                    <button t-foreach="filters" t-as="f" t-key="f.id"
                            t-attf-class="pl-chip{{ state.filter === f.id ? ' active' : '' }}"
                            t-on-click="() => this.setFilter(f.id)">
                        <t t-esc="f.label"/>
                    </button>
                </div>
                <t t-if="state.loading"><div class="pl-hint">Loading…</div></t>
                <t t-elif="!state.rows.length"><div class="pl-hint">Nothing here.</div></t>
                <t t-else="">
                    <button t-foreach="state.rows" t-as="r" t-key="r.id"
                            t-attf-class="pl-row{{ state.sel === r.id ? ' active' : '' }}"
                            t-on-click="() => this.select(r.id)">
                        <div class="pl-row-top">
                            <span class="pl-mpn" t-esc="r.mpn || r.query"/>
                            <span t-attf-class="pl-state s-{{ r.state }}" t-esc="r.state"/>
                        </div>
                        <div class="pl-row-sub">
                            <t t-esc="r.manufacturer || '—'"/>
                            <t t-if="r.confidence"> · <t t-esc="pct(r.confidence)"/></t>
                            <t t-if="r.issues"> · <span class="pl-warn"><t t-esc="r.issues"/> issue(s)</span></t>
                        </div>
                        <div class="pl-row-date" t-esc="r.created"/>
                    </button>
                </t>
            </div>

            <!-- detail -->
            <div class="pl-detail">
                <t t-if="!state.detail"><div class="pl-hint">Pick a proposal to review it.</div></t>
                <t t-else="">
                    <div class="pl-d-head">
                        <h3 t-esc="state.detail.payload.name || state.detail.mpn || state.detail.query"/>
                        <span t-attf-class="pl-state s-{{ state.detail.state }}" t-esc="state.detail.state"/>
                    </div>

                    <div class="pl-facts">
                        <div><span>Query</span><b t-esc="state.detail.query"/></div>
                        <div><span>MPN</span><b t-esc="state.detail.mpn || '—'"/></div>
                        <div><span>Manufacturer</span><b t-esc="state.detail.manufacturer || '—'"/></div>
                        <div><span>Confidence</span><b t-esc="pct(state.detail.confidence)"/></div>
                        <div t-if="state.detail.source">
                            <span>Source</span><a t-att-href="state.detail.source" target="_blank"
                                                  rel="noopener noreferrer" t-esc="state.detail.source"/>
                        </div>
                        <div t-if="state.detail.payload.datasheet_url">
                            <span>Datasheet</span><a t-att-href="state.detail.payload.datasheet_url"
                                target="_blank" rel="noopener noreferrer">open</a>
                        </div>
                    </div>

                    <!-- what the server distrusts, shown before the data itself -->
                    <div t-if="state.detail.issues.length" class="pl-issues">
                        <h4>What to check</h4>
                        <div t-foreach="state.detail.issues" t-as="i" t-key="i_index"
                             t-attf-class="pl-issue lvl-{{ i.level }}">
                            <span class="pl-issue-field" t-esc="i.field"/>
                            <span t-esc="i.message"/>
                        </div>
                    </div>

                    <h4>Parameters</h4>
                    <div class="pl-scroll">
                        <table class="pl-table">
                            <thead><tr><th>Name</th><th>Value</th><th>Unit</th></tr></thead>
                            <tbody>
                                <tr t-foreach="params" t-as="p" t-key="p_index">
                                    <td t-esc="p.name"/>
                                    <td class="pl-mono" t-esc="p.value"/>
                                    <td class="pl-mono" t-esc="p.unit || ''"/>
                                </tr>
                                <tr t-if="!params.length"><td colspan="3" class="pl-hint">No parameters proposed.</td></tr>
                            </tbody>
                        </table>
                    </div>

                    <h4>Where it will go</h4>
                    <div class="pl-apply">
                        <label>Category</label>
                        <select t-model="state.categId">
                            <option value="0">(leave unset)</option>
                            <t t-foreach="state.categories" t-as="c" t-key="c.id">
                                <option t-att-value="c.id" t-esc="c.path"/>
                            </t>
                        </select>
                        <label>Existing product</label>
                        <input type="number" min="0" t-model="state.productId"
                               placeholder="0 = create a new one"/>
                    </div>
                    <div t-if="state.detail.payload.category_path and !state.detail.categ_id" class="pl-hint">
                        The agent suggested “<t t-esc="state.detail.payload.category_path"/>”, which did not match a
                        category — pick one above.
                    </div>

                    <div class="pl-actions">
                        <button class="pl-btn primary" t-on-click="apply"
                                t-att-disabled="state.busy or state.detail.state === 'applied'">
                            <t t-esc="state.detail.state === 'applied' ? 'Already applied' : 'Apply to catalogue'"/>
                        </button>
                        <button class="pl-btn danger" t-on-click="reject"
                                t-att-disabled="state.busy or state.detail.state === 'applied'">Reject</button>
                        <span class="pl-spacer"/>
                        <span t-if="state.detail.product_id" class="pl-hint">
                            product #<t t-esc="state.detail.product_id"/>
                        </span>
                    </div>
                </t>
            </div>
        </div>
    </div>
    `;

    setup() {
        this.state = owl.useState({
            rows: [], detail: null, sel: 0, categories: [],
            filter: 'pending', categId: '0', productId: '0',
            loading: true, busy: false, error: '', notice: '',
            paste: false, pasteText: '',
        });
        this.pasteRef = owl.useRef('paste');
        this.init();
    }

    get filters() {
        return [
            { id: 'pending',  label: 'Pending' },
            { id: 'invalid',  label: 'Needs fixing' },
            { id: 'applied',  label: 'Applied' },
            { id: 'rejected', label: 'Rejected' },
            { id: '',         label: 'All' },
        ];
    }
    get params() {
        const p = this.state.detail && this.state.detail.payload;
        return (p && Array.isArray(p.parameters)) ? p.parameters : [];
    }
    pct(v) { return v ? Math.round(Number(v) * 100) + '%' : '—'; }

    async init() {
        try {
            // The same vocabulary the agent is told to target, so the reviewer
            // picks from exactly the list the agent was choosing from.
            const d = await RpcService.call('part.lookup', 'describe', [{}], {});
            this.state.categories = (d && d.categories) || [];
        } catch (e) { this.state.error = (e && e.message) || 'Could not load the category list.'; }
        await this.reload();
    }

    async reload() {
        this.state.loading = true;
        this.state.error = '';
        try {
            const domain = this.state.filter ? [['state', '=', this.state.filter]] : [];
            this.state.rows = await RpcService.call('part.lookup', 'search_read', [domain], {});
        } catch (e) {
            this.state.rows = [];
            this.state.error = (e && e.message) || 'Could not load proposals.';
        }
        this.state.loading = false;
    }

    async setFilter(f) { this.state.filter = f; this.state.detail = null; this.state.sel = 0; await this.reload(); }

    async select(id) {
        this.state.sel = id;
        this.state.notice = '';
        try {
            const d = await RpcService.call('part.lookup', 'read', [[id]], {});
            this.state.detail = d;
            this.state.categId = String(d.categ_id || 0);
            this.state.productId = String(d.product_id || 0);
        } catch (e) { this.state.error = (e && e.message) || 'Could not open that proposal.'; }
    }

    async apply() {
        if (!this.state.detail) return;
        this.state.busy = true; this.state.error = ''; this.state.notice = '';
        try {
            const r = await RpcService.call('part.lookup', 'apply', [{
                id: this.state.detail.id,
                category_id: parseInt(this.state.categId, 10) || 0,
                product_id: parseInt(this.state.productId, 10) || 0,
            }], {});
            this.state.notice = `Applied to product #${r.product_id} — ${r.parameters} parameter(s) written.`;
            await this.reload();
            await this.select(this.state.detail.id);
        } catch (e) { this.state.error = (e && e.message) || 'Apply failed.'; }
        this.state.busy = false;
    }

    async reject() {
        if (!this.state.detail) return;
        this.state.busy = true;
        try {
            await RpcService.call('part.lookup', 'reject', [[this.state.detail.id]], {});
            this.state.notice = 'Rejected. Nothing was written.';
            await this.reload();
            await this.select(this.state.detail.id);
        } catch (e) { this.state.error = (e && e.message) || 'Reject failed.'; }
        this.state.busy = false;
    }

    setPaste(on) {
        this.state.paste = on;
        if (on) Promise.resolve().then(() => { if (this.pasteRef.el) this.pasteRef.el.focus(); });
    }

    fillSample() {
        this.state.pasteText = JSON.stringify({
            query: '4.7k 0805 1% resistor',
            mpn: 'RC0805FR-074K7L',
            manufacturer: 'Yageo',
            name: 'Yageo RC0805FR-074K7L',
            category_path: 'All / Electronics / Passives / Resistors / SMD Resistors',
            source: 'https://example.com/rc0805.pdf',
            confidence: 0.92,
            parameters: [
                { name: 'Resistance', value: '4k7',  unit: 'Ω' },
                { name: 'Tolerance',  value: '1',    unit: '%' },
                { name: 'Power',      value: '125m', unit: 'W' },
            ],
        }, null, 2);
    }

    async submitPaste() {
        this.state.busy = true; this.state.error = ''; this.state.notice = '';
        try {
            let body;
            try { body = JSON.parse(this.state.pasteText); }
            catch (_) { throw new Error('That is not valid JSON.'); }
            const r = await RpcService.call('part.lookup', 'submit', [body], {});
            // A result with errors is still stored — say so plainly rather than
            // reporting a failure, because the reviewer can fix it here.
            this.state.notice = r.ok
                ? `Staged as #${r.id}. Review it below.`
                : `Staged as #${r.id} with ${(r.issues || []).length} issue(s) — see "Needs fixing".`;
            this.state.filter = r.ok ? 'pending' : 'invalid';
            await this.reload();
            await this.select(r.id);
        } catch (e) { this.state.error = (e && e.message) || 'Submit failed.'; }
        this.state.busy = false;
    }
}
