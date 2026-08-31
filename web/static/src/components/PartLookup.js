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

        <!-- Ask the agent. The answer is only ever a PROPOSAL: this button
             cannot write to the catalogue, only offer candidates for one. -->
        <div class="pl-ask">
            <input class="pl-ask-in" type="text" spellcheck="false"
                   placeholder="Ask the agent about a part — e.g. RC0805FR-074K7L, or '4.7k 0805 1% resistor'"
                   t-att-value="state.askText"
                   t-on-input="ev => state.askText = ev.target.value"
                   t-on-keydown="ev => ev.key === 'Enter' &amp;&amp; this.ask()"/>
            <button class="pl-btn primary" t-on-click="ask" t-att-disabled="state.asking">
                <t t-esc="state.asking ? 'Searching…' : 'Ask the agent'"/>
            </button>
        </div>

        <!-- What the agent came back with, directly under what was asked.
             NOTHING here has been staged: these are candidates, and picking
             one is a decision a person makes. -->
        <div t-if="state.askRes" class="pl-agent">
            <div class="pl-agent-head">
                <strong>
                    <t t-esc="state.askRes.candidates.length"/>
                    candidate<t t-if="state.askRes.candidates.length !== 1">s</t>
                </strong>
                <!-- Whether it actually READ anything is the first thing worth
                     knowing. A model answering from memory will still give you
                     a part number and a URL; only this says whether it looked. -->
                <span class="pl-badge ok" t-if="state.askRes.searched">searched the web</span>
                <span class="pl-badge warn" t-else="">from memory — did not search</span>
                <span class="pl-agent-model"
                      t-esc="state.askRes.mocked ? 'mock provider' : state.askRes.model"/>
                <span class="pl-spacer"/>
                <button class="pl-btn ghost" t-on-click="() => state.askRes = null">Dismiss</button>
            </div>

            <div class="pl-notes" t-if="state.askRes.notes" t-esc="state.askRes.notes"/>

            <!-- What it typed into a search box. This is how you tell a good
                 answer from a lucky one — a bad search explains a bad result. -->
            <div class="pl-searches" t-if="state.askRes.searches.length">
                <span class="pl-sources-h">Searched for</span>
                <code t-foreach="state.askRes.searches" t-as="q" t-key="q_index" t-esc="q"/>
            </div>

            <div class="pl-sources" t-if="state.askRes.sources.length">
                <div class="pl-sources-h">Pages it read — cited ones first</div>
                <a t-foreach="state.askRes.sources" t-as="s" t-key="s_index"
                   t-att-href="s.url" target="_blank" rel="noopener noreferrer" t-esc="s.title"/>
            </div>

            <div class="pl-cands">
                <div t-foreach="state.askRes.candidates" t-as="c" t-key="c_index" class="pl-cand">
                    <div class="pl-cand-top">
                        <span class="pl-mpn" t-esc="c.mpn || '(no part number)'"/>
                        <span t-attf-class="pl-cbadge c-{{ confLevel(c.confidence) }}"
                              t-att-title="confHint(c.confidence)" t-esc="pct(c.confidence)"/>
                    </div>
                    <div class="pl-cand-name" t-esc="c.name || ''"/>
                    <div class="pl-cand-sub">
                        <t t-esc="c.manufacturer || '—'"/> ·
                        <t t-esc="(c.parameters || []).length"/> parameter(s)
                    </div>
                    <div class="pl-cand-why" t-if="c.why" t-esc="c.why"/>
                    <div class="pl-cand-adj" t-if="c.adjusted and c.adjusted.length">
                        <div t-foreach="c.adjusted" t-as="a" t-key="a_index" t-esc="a"/>
                    </div>
                    <div class="pl-cand-links">
                        <a t-if="c.source" t-att-href="c.source" target="_blank"
                           rel="noopener noreferrer">source</a>
                        <a t-if="c.datasheet_url" t-att-href="c.datasheet_url" target="_blank"
                           rel="noopener noreferrer">datasheet</a>
                    </div>
                    <button class="pl-btn primary" t-on-click="() => this.stage(c)"
                            t-att-disabled="state.busy">Stage for review</button>
                </div>
            </div>
        </div>

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
                            <t t-if="r.confidence"> · <span t-attf-class="pl-cdot c-{{ confLevel(r.confidence) }}"
                                 t-att-title="confHint(r.confidence)"><t t-esc="pct(r.confidence)"/></span></t>
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
                        <!-- Confidence is the AGENT'S claim about its own
                             certainty, so it is reported, never edited. Editing
                             it would not make the data better — it would only
                             destroy the one signal saying how hard to check. -->
                        <span t-attf-class="pl-cbadge c-{{ confLevel(state.detail.confidence) }}"
                              t-att-title="confHint(state.detail.confidence)">
                            <t t-esc="pct(state.detail.confidence)"/> confident
                        </span>
                        <span t-attf-class="pl-state s-{{ state.detail.state }}" t-esc="state.detail.state"/>
                    </div>

                    <!-- Every field is editable until the proposal is applied.
                         Without this, disagreeing with one number meant
                         rejecting the whole proposal and retyping it — so the
                         realistic alternative was not a better catalogue, it
                         was applying something known to be slightly wrong. -->
                    <div class="pl-facts">
                        <div><span>Query</span><b t-esc="state.detail.query"/></div>
                    </div>
                    <div class="pl-edit" t-att-class="{ro: state.detail.state === 'applied'}">
                        <label>Part number</label>
                        <input t-model="state.edit.mpn"
                               t-att-disabled="state.detail.state === 'applied'"/>
                        <label>Manufacturer</label>
                        <input t-model="state.edit.manufacturer"
                               t-att-disabled="state.detail.state === 'applied'"/>
                        <label>Name</label>
                        <input t-model="state.edit.name"
                               t-att-disabled="state.detail.state === 'applied'"/>
                        <label>Source</label>
                        <div class="pl-url">
                            <input t-model="state.edit.source" placeholder="https://…"
                                   t-att-disabled="state.detail.state === 'applied'"/>
                            <a t-if="state.edit.source" t-att-href="state.edit.source"
                               target="_blank" rel="noopener noreferrer">open</a>
                        </div>
                        <label>Datasheet</label>
                        <div class="pl-url">
                            <input t-model="state.edit.datasheet_url" placeholder="https://…"
                                   t-att-disabled="state.detail.state === 'applied'"/>
                            <a t-if="state.edit.datasheet_url" t-att-href="state.edit.datasheet_url"
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
                    <!-- The unit list is the SAME vocabulary the agent was told
                         to target, so a hand-typed unit is picked from what the
                         ERP will actually accept rather than guessed at. -->
                    <datalist id="pl-units">
                        <option t-foreach="state.units" t-as="u" t-key="u" t-att-value="u"/>
                    </datalist>
                    <div class="pl-scroll">
                        <table class="pl-table">
                            <thead><tr><th>Name</th><th>Value</th><th>Unit</th><th/></tr></thead>
                            <tbody>
                                <tr t-foreach="state.edit.parameters" t-as="p" t-key="p_index">
                                    <td><input t-att-value="p.name" t-att-disabled="state.detail.state === 'applied'"
                                               t-on-input="ev => p.name = ev.target.value"/></td>
                                    <td><input class="pl-mono" t-att-value="p.value"
                                               t-att-disabled="state.detail.state === 'applied'"
                                               t-on-input="ev => p.value = ev.target.value"/></td>
                                    <td><input class="pl-mono" list="pl-units" t-att-value="p.unit || ''"
                                               t-att-disabled="state.detail.state === 'applied'"
                                               t-on-input="ev => p.unit = ev.target.value"/></td>
                                    <td><button class="pl-x" title="Remove this parameter"
                                                t-if="state.detail.state !== 'applied'"
                                                t-on-click="() => this.delParam(p_index)">×</button></td>
                                </tr>
                                <tr t-if="!state.edit.parameters.length">
                                    <td colspan="4" class="pl-hint">No parameters proposed.</td>
                                </tr>
                            </tbody>
                        </table>
                    </div>
                    <div class="pl-editbar" t-if="state.detail.state !== 'applied'">
                        <button class="pl-btn ghost" t-on-click="addParam">Add a parameter</button>
                        <span class="pl-spacer"/>
                        <button class="pl-btn" t-on-click="save" t-att-disabled="state.busy">
                            Save changes
                        </button>
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

                    <!-- The button offers only what the server will actually
                         accept. Enabled on an invalid proposal it promised an
                         apply that was always going to be refused, which reads
                         as a broken screen rather than as the rule it is. -->
                    <div class="pl-actions">
                        <button class="pl-btn primary" t-on-click="apply"
                                t-att-disabled="state.busy or !canApply"
                                t-att-title="applyHint">
                            <t t-esc="applyLabel"/>
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
            askText: '', asking: false,
            rows: [], detail: null, sel: 0, categories: [],
            filter: 'pending', categId: '0', productId: '0',
            loading: true, busy: false, error: '', notice: '',
            askRes: null, units: [],
            edit: { mpn: '', manufacturer: '', name: '', source: '',
                    datasheet_url: '', parameters: [] },
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
    pct(v) { return v ? Math.round(Number(v) * 100) + '%' : '—'; }

    // Only a pending proposal may be applied. The other three states are
    // decisions already taken, and the server refuses each of them — so the
    // button says so rather than letting the click fail.
    get canApply() { return !!this.state.detail && this.state.detail.state === 'pending'; }

    get applyLabel() {
        switch (this.state.detail && this.state.detail.state) {
            case 'applied':  return 'Already applied';
            case 'rejected': return 'Rejected';
            case 'invalid':  return 'Fix the errors first';
            default:         return 'Apply to catalogue';
        }
    }

    get applyHint() {
        switch (this.state.detail && this.state.detail.state) {
            case 'applied':  return 'This proposal is already in the catalogue.';
            case 'rejected': return 'Rejected. Edit it to reopen it before applying.';
            case 'invalid':  return 'Something above could not be read. Correct it and save.';
            default:         return 'Write this proposal to the catalogue.';
        }
    }

    /**
     * Confidence as a band, for colour.
     *
     * The bands are about REVIEW EFFORT, not accuracy: high means the usual
     * check, low means read the datasheet before you believe any of it. They
     * are deliberately pessimistic — a model's 0.7 is not a calibrated 70%,
     * and treating it as one is how a plausible guess gets applied.
     */
    confLevel(v) {
        const n = Number(v) || 0;
        if (!n) return 'none';
        if (n >= 0.85) return 'high';
        if (n >= 0.6)  return 'med';
        return 'low';
    }

    confHint(v) {
        switch (this.confLevel(v)) {
            case 'high': return 'The agent is fairly sure. Check it the way you would check anyone.';
            case 'med':  return 'The agent is unsure. Open the source before applying this.';
            case 'low':  return 'The agent is guessing. Verify every field against the datasheet.';
            default:     return 'The agent gave no confidence at all — treat it as a guess.';
        }
    }

    async init() {
        try {
            // The same vocabulary the agent is told to target, so the reviewer
            // picks from exactly the list the agent was choosing from.
            const d = await RpcService.call('part.lookup', 'describe', [{}], {});
            this.state.categories = (d && d.categories) || [];
            this.state.units = ((d && d.units) || []).map(u => u.symbol || u).filter(Boolean);
        } catch (e) { this.state.error = (e && e.message) || 'Could not load the category list.'; }
        await this.reload();
    }

    /**
     * Ask the configured provider and SHOW what came back.
     *
     * Three steps on purpose: `ask` produces candidates, a person picks one,
     * `submit` stages it. The bridge has no write access, so a wrong or
     * hijacked answer cannot reach the catalogue on its own — which is the
     * whole point of the contract.
     *
     * This used to stage the first answer automatically. That was wrong for an
     * incomplete query: asking about "4.7k 0805" has several right answers, and
     * silently picking one buries the ambiguity instead of showing it.
     */
    async ask() {
        const q = (this.state.askText || '').trim();
        if (!q) return;
        this.state.asking = true;
        this.state.error = '';
        this.state.notice = '';
        this.state.askRes = null;
        try {
            const r = await RpcService.call('ir.ai.settings', 'ask', [{ query: q }], {});
            if (!r || !r.ok) {
                this.state.error = 'The agent could not answer: ' + ((r && r.detail) || 'unknown error');
            } else {
                this.state.askRes = {
                    notes: r.notes || '',
                    sources: r.sources || [],
                    searches: r.searches || [],
                    candidates: r.candidates || [],
                    searched: !!r.searched,
                    mocked: !!r.mocked,
                    model: r.model || r.provider || '',
                };
            }
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.asking = false;
    }

    /** Stage one candidate. The others stay on screen — a second one is often
     *  worth keeping, and re-asking to get it back costs another call. */
    async stage(c) {
        this.state.busy = true; this.state.error = ''; this.state.notice = '';
        try {
            const body = Object.assign({}, c);
            delete body.adjusted;          // ours, not part of the LookupResult
            delete body.why;
            const r = await RpcService.call('part.lookup', 'submit', [body], {});
            this.state.notice = r.ok
                ? `Staged as #${r.id}. Review it below.`
                : `Staged as #${r.id} with ${(r.issues || []).length} issue(s) — see "Needs fixing".`;
            this.state.filter = r.ok ? 'pending' : 'invalid';
            await this.reload();
            await this.select(r.id);
        } catch (e) { this.state.error = (e && e.message) || 'Could not stage that candidate.'; }
        this.state.busy = false;
    }

    addParam() { this.state.edit.parameters.push({ name: '', value: '', unit: '' }); }
    delParam(i) { this.state.edit.parameters.splice(i, 1); }

    /** Save the edits back to the staged proposal, re-validating server-side. */
    async save() {
        if (!this.state.detail) return;
        this.state.busy = true; this.state.error = ''; this.state.notice = '';
        try {
            const e = this.state.edit;
            const r = await RpcService.call('part.lookup', 'update', [{
                id: this.state.detail.id,
                mpn: e.mpn, manufacturer: e.manufacturer, name: e.name,
                source: e.source, datasheet_url: e.datasheet_url,
                // confidence is NOT sent: it is the agent's statement, not the
                // reviewer's. update() merges onto the stored payload, so
                // leaving it out preserves what the agent actually claimed.
                parameters: e.parameters.filter(p => (p.name || '').trim()),
            }], {});
            this.state.notice = r.ok
                ? 'Saved. The proposal is ready to apply.'
                : `Saved, but ${(r.issues || []).length} issue(s) remain — see below.`;
            await this.reload();
            await this.select(this.state.detail.id);
        } catch (e) { this.state.error = (e && e.message) || 'Could not save.'; }
        this.state.busy = false;
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
        // Only clear the message when moving to a DIFFERENT proposal.
        //
        // Every action here ends `…; await reload(); await select(sameId)`, so
        // clearing unconditionally wiped the message the action had just set —
        // "Saved.", "Applied to product #123.", "Rejected." were all set and
        // then destroyed a few milliseconds later. The work happened; the
        // screen simply never admitted it.
        if (this.state.sel !== id) this.state.notice = '';
        this.state.sel = id;
        try {
            const d = await RpcService.call('part.lookup', 'read', [[id]], {});
            this.state.detail = d;
            this.state.categId = String(d.categ_id || 0);
            this.state.productId = String(d.product_id || 0);
            // The editor works on a COPY. Editing state.detail directly would
            // mean an abandoned edit still looks saved until the next reload.
            const p = d.payload || {};
            this.state.edit = {
                mpn: d.mpn || p.mpn || '',
                manufacturer: d.manufacturer || p.manufacturer || '',
                name: p.name || '',
                source: d.source || p.source || '',
                datasheet_url: p.datasheet_url || '',
                parameters: JSON.parse(JSON.stringify(
                    Array.isArray(p.parameters) ? p.parameters : [])),
            };
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
