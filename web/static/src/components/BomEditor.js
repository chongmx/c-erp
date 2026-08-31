/**
 * BomEditor.js — Manufacturing → BOM Editor (docs/107).
 *
 * Three columns: pick a BOM, edit its lines by hand, and import one with the
 * assistant on the right.
 *
 * The status colouring is the point of the screen. Every line — typed or
 * imported — carries a severity worked out on the SERVER, so a hand-typed line
 * is checked exactly as hard as an imported one:
 *
 *   red     an error. The BOM cannot be imported or trusted while it stands:
 *           no part chosen, quantity ≤ 0, designator count ≠ quantity, or a
 *           designator used twice on the same board.
 *   yellow  a warning. It will import, but somebody should look: several parts
 *           matched, or a PCBA line with no designators.
 *   plain   resolved and consistent.
 *
 * The assistant panel is where an AI fits. Today it parses deterministically —
 * paste or upload a CSV, the server maps the headers and resolves each row
 * against the catalogue. When a model is wired in it supplies the COLUMN
 * MAPPING for a layout the header matcher does not recognise, or hands over
 * normalised rows. It never supplies a product id: mapping headers is judgement,
 * choosing a part is a lookup that has to be reproducible and reviewable.
 */
class BomEditor extends owl.Component {
    static template = owl.xml`
        <div class="be-shell">

            <div class="be-head">
                <h2 class="be-title">BOM Editor</h2>
                <select class="be-sel" t-on-change="onPickBom">
                    <option value="0">Choose a BOM…</option>
                    <t t-foreach="state.boms" t-as="b" t-key="b.id">
                        <option t-att-value="b.id" t-att-selected="b.id === state.bomId"
                                t-esc="bomLabel(b)"/>
                    </t>
                </select>
                <t t-if="state.bom">
                    <span class="be-kind" t-esc="state.bom.kind"/>
                    <span class="be-rev" t-if="state.bom.revision">Rev <t t-esc="state.bom.revision"/></span>
                    <span class="be-type" t-esc="state.bom.bom_type"/>
                </t>
                <div class="be-spacer"/>
                <t t-if="state.lines.length">
                    <span class="be-tally">
                        <b t-esc="state.lines.length"/> line(s)
                        <span class="be-badge err" t-if="counts.error"><t t-esc="counts.error"/> error</span>
                        <span class="be-badge warn" t-if="counts.warning"><t t-esc="counts.warning"/> warning</span>
                    </span>
                </t>
                <button class="be-btn" t-on-click="() => this.toggleAi()"
                        t-esc="state.aiOpen ? 'Hide assistant »' : '« Import / assistant'"/>
            </div>

            <t t-if="state.error"><div class="be-error" t-esc="state.error"/></t>
            <t t-if="state.notice"><div class="be-notice" t-esc="state.notice"/></t>

            <div class="be-body">

                <div class="be-main">
                    <t t-if="!state.bomId">
                        <div class="be-empty">
                            Choose a BOM above, or create one from a product.
                            <div class="be-new">
                                <select class="be-sel" t-on-change="(ev) => { state.newProduct = parseInt(ev.target.value,10)||0; }">
                                    <option value="0">Product…</option>
                                    <t t-foreach="state.products" t-as="p" t-key="p.id">
                                        <option t-att-value="p.id" t-esc="p.display_name || p.name"/>
                                    </t>
                                </select>
                                <select class="be-sel" t-on-change="(ev) => { state.newKind = ev.target.value; }">
                                    <option value="pcba">PCBA — manufactured</option>
                                    <option value="mechanical">Mechanical assembly</option>
                                    <option value="kit">Kit — packed, not manufactured</option>
                                    <option value="general">General</option>
                                </select>
                                <button class="be-btn primary" t-att-disabled="!state.newProduct"
                                        t-on-click="createBom">Create BOM</button>
                            </div>
                            <p class="be-hint">
                                A <b>kit</b> is always a phantom BOM: its components are picked and
                                packed, never manufactured, so it produces no manufacturing order.
                            </p>
                        </div>
                    </t>

                    <t t-else="">
                        <div class="be-scroll">
                            <table class="be-table">
                                <thead>
                                    <tr>
                                        <th class="be-c-st"/>
                                        <th class="be-c-des">Designators</th>
                                        <th class="be-c-qty">Qty</th>
                                        <th class="be-c-part">Part</th>
                                        <th class="be-c-fp">Package</th>
                                        <th class="be-c-note">Note</th>
                                        <th class="be-c-fit">Fitted</th>
                                        <th class="be-c-act"/>
                                    </tr>
                                </thead>
                                <tbody>
                                    <t t-foreach="state.lines" t-as="l" t-key="l.id">
                                        <tr t-attf-class="be-row sev-{{ l.severity }}">
                                            <td class="be-c-st">
                                                <span t-attf-class="be-dot {{ l.severity }}"
                                                      t-att-title="(l.issues || []).join(' ')"/>
                                            </td>
                                            <td class="be-c-des">
                                                <input class="be-in mono" t-att-value="l.designators"
                                                       placeholder="C1,C2,C5"
                                                       t-on-change="(ev) => this.save(l, 'designators', ev.target.value)"/>
                                            </td>
                                            <td class="be-c-qty">
                                                <input class="be-in num" t-att-value="l.quantity"
                                                       t-on-change="(ev) => this.save(l, 'quantity', parseInt(ev.target.value,10)||0)"/>
                                            </td>
                                            <td class="be-c-part">
                                                <t t-if="l.product_id">
                                                    <span class="be-part" t-esc="l.product_name"/>
                                                    <span class="be-code" t-if="l.product_code" t-esc="l.product_code"/>
                                                </t>
                                                <t t-else="">
                                                    <span class="be-unset">no part chosen</span>
                                                </t>
                                                <button class="be-pick" t-on-click="() => this.openPicker(l)">change</button>
                                            </td>
                                            <td class="be-c-fp mono" t-esc="l.footprint"/>
                                            <td class="be-c-note">
                                                <input class="be-in" t-att-value="l.note"
                                                       t-on-change="(ev) => this.save(l, 'note', ev.target.value)"/>
                                            </td>
                                            <td class="be-c-fit">
                                                <input type="checkbox" t-att-checked="l.fitted"
                                                       title="Untick for do-not-populate"
                                                       t-on-change="(ev) => this.save(l, 'fitted', ev.target.checked)"/>
                                            </td>
                                            <td class="be-c-act">
                                                <button class="be-del" title="Remove"
                                                        t-on-click="() => this.delLine(l)">×</button>
                                            </td>
                                        </tr>
                                        <tr t-if="l.issues and l.issues.length" t-attf-class="be-isrow sev-{{ l.severity }}">
                                            <td/><td colspan="7" class="be-issues">
                                                <t t-foreach="l.issues" t-as="i" t-key="i_index">
                                                    <span class="be-issue" t-esc="i"/>
                                                </t>
                                            </td>
                                        </tr>
                                    </t>
                                    <tr t-if="!state.lines.length">
                                        <td colspan="8" class="be-empty-row">
                                            No lines yet. Add one below, or import with the assistant.
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                        <div class="be-addbar">
                            <button class="be-btn" t-on-click="addLine">+ Add a line</button>
                            <span class="be-hint">
                                Designator ranges are understood: <code>R1-R4</code> becomes four.
                            </span>
                        </div>
                    </t>
                </div>

                <!-- assistant / import rail -->
                <div class="be-ai" t-if="state.aiOpen">
                    <div class="be-ai-h">
                        <span>Import assistant</span>
                        <button class="be-icon" t-on-click="() => this.toggleAi()">»</button>
                    </div>
                    <div class="be-ai-b">
                        <t t-if="!state.bomId">
                            <div class="be-ai-note">Choose a BOM first — an import needs somewhere to go.</div>
                        </t>
                        <t t-else="">
                            <t t-if="!state.staged.length">
                                <div class="be-ai-note">
                                    Paste a BOM, or drop a CSV. Headers are matched automatically —
                                    Designator, Qty, MPN, Value, Footprint and their usual aliases.
                                    Each row is then resolved against the catalogue.
                                </div>
                                <textarea class="be-ai-in" rows="9" t-att-value="state.pasteText"
                                          placeholder="Designator,Qty,Value,Footprint,MPN&#10;C1&#44;C2,2,100nF,0603,GRM188R71C104KA01D&#10;R1-R4,4,10k,0402,"
                                          t-on-input="(ev) => { state.pasteText = ev.target.value; }"/>
                                <div t-attf-class="be-drop{{ state.dragging ? ' over' : '' }}"
                                     t-on-dragover.prevent="() => { state.dragging = true; }"
                                     t-on-dragleave="() => { state.dragging = false; }"
                                     t-on-drop.prevent="onDropFile"
                                     t-on-click="() => this.fileRef.el &amp;&amp; this.fileRef.el.click()">
                                    Drop a CSV here, or click to choose one
                                </div>
                                <input type="file" accept=".csv,.txt,.tsv" class="be-file" t-ref="file"
                                       t-on-change="onFileChosen"/>
                                <div class="be-ai-act">
                                    <button class="be-btn primary" t-att-disabled="state.busy or !state.pasteText.trim()"
                                            t-on-click="() => this.parseText()"
                                            t-esc="state.busy ? 'Parsing…' : 'Parse and resolve'"/>
                                    <!-- Cancel is available WHILE it parses, not only before.
                                         A big file against a slow catalogue is exactly when
                                         somebody realises they dropped the wrong one. -->
                                    <button class="be-btn ghost" t-if="state.pasteText.trim() or state.busy"
                                            t-on-click="cancelUpload">Cancel</button>
                                </div>

                                <!-- The header row was not recognised. This is the only
                                     parse failure a person can actually act on, so it
                                     gets an action rather than just an error. -->
                                <t t-if="state.needsMapping">
                                    <div class="be-map">
                                        <div class="be-map-h">These column names are not ones I know</div>
                                        <t t-if="!state.mapping">
                                            <div class="be-ai-note">
                                                Every EDA tool names them differently. The assistant can
                                                read the header row and say which column is which — you
                                                get to check it before anything is imported.
                                            </div>
                                            <button class="be-btn primary" t-if="state.aiReady"
                                                    t-att-disabled="state.mapBusy" t-on-click="askMapping"
                                                    t-esc="state.mapBusy ? 'Reading the header…' : 'Ask the assistant'"/>
                                            <div class="be-ai-note" t-else="">
                                                No AI provider is set up. An administrator can enable one in
                                                Settings → AI Agent, or you can map the columns by hand below.
                                            </div>
                                            <button class="be-btn ghost" t-on-click="mapByHand">Map by hand</button>
                                        </t>

                                        <!-- Shown, never applied silently: it is a guess about
                                             somebody's column layout, cheap to check here and
                                             expensive to unpick after an import. -->
                                        <t t-else="">
                                            <div class="be-map-note" t-if="state.mapTool">
                                                Looks like <b t-esc="state.mapTool"/>.
                                            </div>
                                            <div class="be-map-note" t-if="state.mapNotes" t-esc="state.mapNotes"/>
                                            <div class="be-map-grid">
                                                <t t-foreach="mapFields" t-as="f" t-key="f.key">
                                                    <label t-esc="f.label"/>
                                                    <select t-on-change="(ev) => this.setMap(f.key, ev)">
                                                        <option value="">— not in this file —</option>
                                                        <t t-foreach="state.mapHeaders" t-as="h" t-key="h_index">
                                                            <option t-att-value="h_index"
                                                                    t-att-selected="state.mapping[f.key] === h_index"
                                                                    t-esc="h_index + ': ' + h"/>
                                                        </t>
                                                    </select>
                                                </t>
                                            </div>
                                            <!-- Getting this backwards populates exactly the parts
                                                 that were meant to be left off the board. -->
                                            <label class="be-map-neg" t-if="state.mapping.fitted !== undefined">
                                                <input type="checkbox" t-att-checked="state.mapFittedNeg"
                                                       t-on-change="(ev) => { state.mapFittedNeg = ev.target.checked; }"/>
                                                A mark in that column means <b>do not populate</b>
                                            </label>
                                            <div class="be-ai-act">
                                                <button class="be-btn primary" t-on-click="applyMapping"
                                                        t-att-disabled="state.busy">Parse with this mapping</button>
                                                <button class="be-btn ghost" t-on-click="cancelUpload">Cancel</button>
                                            </div>
                                        </t>
                                    </div>
                                </t>
                            </t>

                            <t t-else="">
                                <!-- A staged import IS the draft: it survives leaving the
                                     screen and is only lost on commit or discard. Saying
                                     so is the whole feature — coming back to it looked
                                     identical to a fresh start. -->
                                <div t-attf-class="be-draft{{ state.draft and state.draft.stale ? ' stale' : '' }}"
                                     t-if="state.draft">
                                    <b>Draft import</b> started <t t-esc="state.draft.started"/>.
                                    <t t-if="state.draft.stale">
                                        That was a while ago — if the file has moved on since,
                                        discard this and import the newer one.
                                    </t>
                                    <t t-else="">Nothing is written until you import it.</t>
                                </div>
                                <div class="be-ai-sum">
                                    <span class="be-badge ok" t-if="staged.ok"><t t-esc="staged.ok"/> ready</span>
                                    <span class="be-badge warn" t-if="staged.warning"><t t-esc="staged.warning"/> warning</span>
                                    <span class="be-badge err" t-if="staged.error"><t t-esc="staged.error"/> error</span>
                                </div>
                                <div class="be-ai-rows">
                                    <t t-foreach="state.staged" t-as="s" t-key="s.id">
                                        <div t-attf-class="be-srow sev-{{ s.severity }}">
                                            <div class="be-srow-t">
                                                <span class="be-dot" t-attf-class="be-dot {{ s.severity }}"/>
                                                <span class="mono" t-esc="s.designators || '—'"/>
                                                <span class="be-sqty">×<t t-esc="s.quantity"/></span>
                                            </div>
                                            <div class="be-srow-s" t-esc="[s.value, s.footprint, s.mpn].filter(x => x).join(' · ')"/>
                                            <div class="be-srow-p">
                                                <t t-if="s.product_id">
                                                    <span class="be-part" t-esc="s.product_name"/>
                                                </t>
                                                <t t-elif="s.candidates and s.candidates.length">
                                                    <select class="be-sel sm"
                                                            t-on-change="(ev) => this.chooseCandidate(s, ev)">
                                                        <option value="0">Pick one of <t t-esc="s.candidates.length"/>…</option>
                                                        <t t-foreach="s.candidates" t-as="c" t-key="c.id">
                                                            <option t-att-value="c.id" t-esc="c.name"/>
                                                        </t>
                                                    </select>
                                                </t>
                                                <t t-else="">
                                                    <input class="be-in sm" placeholder="Search a part…"
                                                           t-on-change="(ev) => this.searchFor(s, ev.target.value)"/>
                                                </t>
                                            </div>
                                            <div class="be-srow-i" t-if="s.issues and s.issues.length">
                                                <t t-foreach="s.issues" t-as="i" t-key="i_index">
                                                    <span class="be-issue" t-esc="i"/>
                                                </t>
                                            </div>
                                            <div class="be-srow-i" t-if="state.hits[s.id] and state.hits[s.id].length">
                                                <select class="be-sel sm" t-on-change="(ev) => this.chooseCandidate(s, ev)">
                                                    <option value="0">Choose from search…</option>
                                                    <t t-foreach="state.hits[s.id]" t-as="h" t-key="h.id">
                                                        <option t-att-value="h.id" t-esc="h.name"/>
                                                    </t>
                                                </select>
                                            </div>
                                        </div>
                                    </t>
                                </div>
                                <!-- Offered where it pays: rows that did not resolve are
                                     usually rows written in a way the ERP does not read,
                                     not rows for parts you do not stock. -->
                                <div class="be-tidy" t-if="!state.tidy">
                                    <button class="be-btn" t-if="state.aiReady"
                                            t-att-disabled="state.tidyBusy or state.busy"
                                            t-on-click="tidyRows"
                                            t-esc="state.tidyBusy ? 'Tidying…' : 'Tidy up with the assistant'"/>
                                    <div class="be-ai-note">
                                        Rewrites values, packages and designators to this ERP's
                                        conventions — 4.7K to 4k7, C_0603_1608Metric to 0603. It
                                        never chooses a part; rows are resolved again afterwards.
                                    </div>
                                </div>

                                <!-- Shown as a diff, and applied only on request. A
                                     bulk rewrite nobody reads is a bulk rewrite nobody
                                     can undo. -->
                                <div class="be-tidy" t-if="state.tidy">
                                    <div class="be-tidy-h">
                                        <t t-if="state.tidy.changed.length">
                                            <b t-esc="state.tidy.changed.length"/> change(s) proposed
                                        </t>
                                        <t t-else="">Nothing needed changing.</t>
                                    </div>
                                    <div class="be-ai-note" t-if="state.tidy.notes" t-esc="state.tidy.notes"/>
                                    <div class="be-tidy-list" t-if="state.tidy.changed.length">
                                        <t t-foreach="state.tidy.changed" t-as="c" t-key="c_index">
                                            <div class="be-tidy-row">
                                                <span class="mono" t-esc="c.designators || ('row ' + (c.row + 1))"/>
                                                <span class="be-tidy-f" t-esc="c.field"/>
                                                <span class="be-tidy-b" t-esc="c.from || '(empty)'"/>
                                                <span class="be-tidy-a" t-esc="c.to || '(empty)'"/>
                                            </div>
                                        </t>
                                    </div>
                                    <div class="be-ai-act">
                                        <button class="be-btn primary"
                                                t-att-disabled="state.busy or !state.tidy.changed.length"
                                                t-on-click="applyTidy">Apply and re-resolve</button>
                                        <button class="be-btn ghost"
                                                t-on-click="() => { state.tidy = null; }">Keep as it is</button>
                                    </div>
                                </div>

                                <div class="be-ai-act">
                                    <button class="be-btn primary" t-att-disabled="staged.error > 0 or state.busy"
                                            t-on-click="commit"
                                            t-att-title="staged.error ? 'Fix the errors first' : ''">
                                        Import <t t-esc="staged.ok + staged.warning"/> line(s)
                                    </button>
                                    <button class="be-btn ghost" t-on-click="discard">Discard</button>
                                </div>
                                <div class="be-ai-note" t-if="staged.error">
                                    Lines in error are not imported and cannot be — a half-imported BOM
                                    is worse than none, because it looks complete.
                                </div>
                            </t>
                        </t>
                    </div>
                </div>
            </div>

            <!-- part picker -->
            <div class="be-modal" t-if="state.picker">
                <div class="be-modal-box">
                    <div class="be-modal-h">
                        <span>Choose a part</span>
                        <button class="be-icon" t-on-click="() => { state.picker = null; }">×</button>
                    </div>
                    <input class="be-in" placeholder="Search by name, code or MPN…"
                           t-on-input="(ev) => this.pickerSearch(ev.target.value)"/>
                    <div class="be-modal-list">
                        <t t-foreach="state.pickerHits" t-as="h" t-key="h.id">
                            <button class="be-modal-row" t-on-click="() => this.choosePart(h)">
                                <span class="be-part" t-esc="h.name"/>
                                <span class="be-code" t-if="h.code" t-esc="h.code"/>
                                <span class="mono" t-if="h.footprint" t-esc="h.footprint"/>
                            </button>
                        </t>
                        <div class="be-empty-row" t-if="!state.pickerHits.length">Type at least two characters.</div>
                    </div>
                </div>
            </div>
        </div>`;

    setup() {
        this.fileRef = owl.useRef('file');
        // Bumped by every parse and by cancel. A parse whose token has moved on
        // drops its answer — the closest thing to recalling an RPC in flight.
        this._parseToken = 0;
        this._reader = null;
        this.state = owl.useState({
            boms: [], products: [], bomId: 0, bom: null, lines: [],
            staged: [], hits: {}, draft: null,
            aiOpen: true, pasteText: '', dragging: false,
            aiReady: false, needsMapping: false, mapBusy: false,
            tidy: null, tidyBusy: false,
            mapping: null, mapHeaders: [], mapFittedNeg: false, mapNotes: '', mapTool: '',
            picker: null, pickerHits: [],
            newProduct: 0, newKind: 'pcba',
            busy: false, error: '', notice: '',
        });
        owl.onWillStart(async () => {
            // Whether an agent exists decides whether to offer the button at
            // all. Offering it and then failing is worse than not offering it.
            try {
                const s = await RpcService.call('ir.ai.settings', 'status', [{}], {}) || {};
                this.state.aiReady = !!s.ready;
            } catch (e) { this.state.aiReady = false; }
            await this.loadBoms();
            try {
                this.state.products = await RpcService.call('product.product', 'search_read', [[]],
                    { fields: ['name', 'display_name'], limit: 300 }) || [];
            } catch (e) { /* the picker is a convenience */ }
        });
    }

    bomLabel(b) {
        const bits = [b.code || ('BOM #' + b.id)];
        if (b.product_name) bits.push(b.product_name);
        if (b.revision) bits.push('Rev ' + b.revision);
        return bits.join(' — ') + '  (' + b.line_count + ')';
    }

    get counts() {
        const c = { ok: 0, warning: 0, error: 0 };
        for (const l of this.state.lines) c[l.severity] = (c[l.severity] || 0) + 1;
        return c;
    }
    get staged() {
        const c = { ok: 0, warning: 0, error: 0 };
        for (const s of this.state.staged) c[s.severity] = (c[s.severity] || 0) + 1;
        return c;
    }

    async loadBoms() {
        try { this.state.boms = await RpcService.call('bom.editor', 'boms', [{}], {}) || []; }
        catch (e) { this.state.error = (e && e.message) || 'Could not load the BOM list.'; }
    }

    async onPickBom(ev) {
        this.state.bomId = parseInt(ev.target.value, 10) || 0;
        this.state.staged = [];
        this.state.hits = {};
        // An abandoned mapping must not follow you to another BOM.
        this.cancelUpload();
        await this.reload();
    }

    async reload() {
        if (!this.state.bomId) { this.state.bom = null; this.state.lines = []; return; }
        this.state.error = '';
        try {
            const d = await RpcService.call('bom.editor', 'lines', [{ bom_id: this.state.bomId }], {});
            this.state.bom = d.bom;
            this.state.lines = d.lines || [];
            const st = await RpcService.call('bom.import', 'staged', [{ bom_id: this.state.bomId }], {});
            this.state.staged = (st && st.rows) || [];
            this.state.draft  = (st && st.draft) || null;
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load this BOM.';
        }
    }

    // ---- hand editing ------------------------------------------------------
    async save(line, field, value) {
        this.state.error = '';
        try {
            const patch = { id: line.id };
            patch[field] = value;
            await RpcService.call('bom.editor', 'set_line', [patch], {});
            await this.reload();          // severities are recomputed server-side
        } catch (e) {
            this.state.error = (e && e.message) || 'That change could not be saved.';
            await this.reload();
        }
    }
    async addLine() {
        try {
            await RpcService.call('bom.editor', 'add_line',
                [{ bom_id: this.state.bomId, quantity: 1 }], {});
            await this.reload();
        } catch (e) { this.state.error = (e && e.message) || 'Could not add a line.'; }
    }
    async delLine(line) {
        try {
            await RpcService.call('bom.editor', 'del_line', [{ id: line.id }], {});
            await this.reload();
        } catch (e) { this.state.error = (e && e.message) || 'Could not remove the line.'; }
    }
    async createBom() {
        try {
            const r = await RpcService.call('bom.editor', 'create_bom',
                [{ product_id: this.state.newProduct, kind: this.state.newKind }], {});
            await this.loadBoms();
            this.state.bomId = r.id;
            this.state.notice = r.kind === 'kit'
                ? 'Kit created as a phantom BOM — it will be picked and packed, not manufactured.'
                : '';
            await this.reload();
        } catch (e) { this.state.error = (e && e.message) || 'Could not create the BOM.'; }
    }

    // ---- part picker -------------------------------------------------------
    openPicker(line) { this.state.picker = line; this.state.pickerHits = []; }
    async pickerSearch(q) {
        if ((q || '').trim().length < 2) { this.state.pickerHits = []; return; }
        try {
            this.state.pickerHits = await RpcService.call('bom.import', 'search_parts',
                [{ q: q.trim() }], {}) || [];
        } catch (e) { this.state.pickerHits = []; }
    }
    async choosePart(hit) {
        const line = this.state.picker;
        this.state.picker = null;
        if (line) await this.save(line, 'product_id', hit.id);
    }

    // ---- import ------------------------------------------------------------
    toggleAi() { this.state.aiOpen = !this.state.aiOpen; }

    onDropFile(ev) {
        this.state.dragging = false;
        const f = ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files[0];
        if (f) this.readFile(f);
    }
    onFileChosen(ev) {
        const f = ev.target.files && ev.target.files[0];
        if (f) this.readFile(f);
    }
    readFile(file) {
        const token = this._parseToken;
        const r = new FileReader();
        this._reader = r;
        r.onload = () => {
            if (token !== this._parseToken) return;      // cancelled while reading
            this.state.pasteText = String(r.result || '');
            this.parseText();
        };
        r.onerror = () => { this.state.error = 'That file could not be read.'; };
        r.readAsText(file);
    }

    /**
     * Abandon whatever is in progress.
     *
     * There is no way to recall an RPC that is already on the wire, so the
     * result is ignored instead: every parse carries a token, and a stale one
     * drops its answer on the floor. Cancelling has to work DURING the parse,
     * not only before it — a large file against a slow catalogue is exactly
     * when somebody notices they dropped the wrong export.
     *
     * Nothing staged is touched. Cancel abandons an attempt; Discard throws
     * away a draft. Conflating them would make one of the two a trap.
     */
    cancelUpload() {
        this._parseToken++;
        if (this._reader) { try { this._reader.abort(); } catch (_) {} this._reader = null; }
        this.state.pasteText = '';
        this.state.needsMapping = false;
        this.state.mapping = null;
        this.state.mapNotes = '';
        this.state.mapTool = '';
        this.state.dragging = false;
        this.state.busy = false;
        this.state.error = '';
        // Without this the same file cannot be chosen twice — the input still
        // holds it, so `change` never fires again.
        if (this.fileRef.el) this.fileRef.el.value = '';
    }

    async parseText(mapping) {
        if (!this.state.bomId || !this.state.pasteText.trim()) return;
        const token = ++this._parseToken;
        this.state.busy = true;
        this.state.error = '';
        this.state.notice = '';
        try {
            const args = { bom_id: this.state.bomId, text: this.state.pasteText };
            if (mapping) { args.mapping = mapping; args.skip_header = true; }
            const d = await RpcService.call('bom.import', 'parse', [args], {});
            if (token !== this._parseToken) return;
            this.state.staged = (d && d.rows) || [];
            this.state.draft = (d && d.draft) || null;
            this.state.needsMapping = false;
            this.state.mapping = null;
            this.state.pasteText = '';
            if (this.fileRef.el) this.fileRef.el.value = '';
        } catch (e) {
            if (token !== this._parseToken) return;
            const msg = (e && e.message) || 'That BOM could not be parsed.';
            this.state.error = msg;
            // The one parse failure a person can do something about. Keep the
            // text — the mapping step needs it.
            if (/header row was not recognised/i.test(msg)) this.state.needsMapping = true;
        } finally {
            if (token === this._parseToken) this.state.busy = false;
        }
    }

    // ---- column mapping -----------------------------------------------------
    get mapFields() {
        return [
            { key: 'designators',  label: 'Designators' },
            { key: 'quantity',     label: 'Quantity' },
            { key: 'value',        label: 'Value' },
            { key: 'footprint',    label: 'Footprint' },
            { key: 'mpn',          label: 'Part number' },
            { key: 'manufacturer', label: 'Manufacturer' },
            { key: 'description',  label: 'Description' },
            { key: 'fitted',       label: 'Fitted / DNP' },
        ];
    }

    /** Split a header row the same way the server does — , ; and tab. */
    splitRow(line) {
        return String(line || '').split(/[,;\t]/).map(s => s.trim().replace(/^"|"$/g, ''));
    }

    async askMapping() {
        const lines = this.state.pasteText.split(/\r?\n/).filter(l => l.trim());
        if (!lines.length) return;
        this.state.mapBusy = true;
        this.state.error = '';
        try {
            // Only the header and a couple of rows: the mapping is decidable
            // from the shape, and shipping the whole BOM to a vendor costs
            // tokens and gives away the part list for nothing.
            const r = await RpcService.call('ir.ai.settings', 'map_bom_headers',
                [{ header: lines[0], samples: lines.slice(1, 4) }], {});
            if (!r || !r.ok) {
                this.state.error = (r && r.detail) || 'The assistant could not map those columns.';
            } else {
                this.state.mapHeaders = this.splitRow(lines[0]);
                this.state.mapping = r.mapping || {};
                this.state.mapFittedNeg = !!r.fitted_negated;
                this.state.mapNotes = r.notes || '';
                this.state.mapTool = r.mocked ? 'the mock provider' : (r.tool || '');
            }
        } catch (e) {
            this.state.error = (e && e.message) || String(e);
        }
        this.state.mapBusy = false;
    }

    /** Map the columns without asking anyone — an empty grid to fill in. */
    mapByHand() {
        const lines = this.state.pasteText.split(/\r?\n/).filter(l => l.trim());
        this.state.mapHeaders = this.splitRow(lines[0] || '');
        this.state.mapping = {};
        this.state.mapFittedNeg = false;
        this.state.mapNotes = '';
        this.state.mapTool = '';
    }

    // ---- tidy up ------------------------------------------------------------
    /**
     * Ask the assistant to normalise the staged rows to house conventions.
     *
     * Proposes only. The rewritten rows go back through `parse`, which
     * re-resolves every one of them against the catalogue — so tidying changes
     * how a row is WRITTEN, never which part it becomes.
     */
    async tidyRows() {
        if (!this.state.staged.length) return;
        this.state.tidyBusy = true;
        this.state.error = '';
        try {
            const rows = this.state.staged.map(s => ({
                designators: s.designators || '', quantity: s.quantity || 0,
                mpn: s.mpn || '', manufacturer: s.manufacturer || '',
                value: s.value || '', footprint: s.footprint || '',
                description: s.description || '', fitted: s.fitted !== false,
            }));
            const r = await RpcService.call('ir.ai.settings', 'clean_bom_rows', [{ rows }], {});
            if (!r || !r.ok) {
                this.state.error = (r && r.detail) || 'The assistant could not tidy these rows.';
            } else {
                this.state.tidy = { rows: r.rows || [], changed: r.changed || [], notes: r.notes || '' };
            }
        } catch (e) {
            this.state.error = (e && e.message) || String(e);
        }
        this.state.tidyBusy = false;
    }

    async applyTidy() {
        if (!this.state.tidy) return;
        this.state.busy = true;
        this.state.error = '';
        try {
            // `rows` is the importer's own agent path — the tidied rows are
            // validated and resolved exactly like any other import.
            const d = await RpcService.call('bom.import', 'parse',
                [{ bom_id: this.state.bomId, rows: this.state.tidy.rows }], {});
            this.state.staged = (d && d.rows) || [];
            this.state.draft = (d && d.draft) || null;
            this.state.notice = 'Tidied and re-resolved.';
            this.state.tidy = null;
        } catch (e) {
            this.state.error = (e && e.message) || 'Those rows could not be re-imported.';
        }
        this.state.busy = false;
    }

    setMap(field, ev) {
        const v = ev.target.value;
        const m = Object.assign({}, this.state.mapping);
        if (v === '') delete m[field]; else m[field] = parseInt(v, 10);
        this.state.mapping = m;
    }

    applyMapping() {
        const m = Object.assign({}, this.state.mapping);
        if (m.fitted !== undefined) m.fitted_negated = this.state.mapFittedNeg;
        this.parseText(m);
    }

    async chooseCandidate(row, ev) {
        const pid = parseInt(ev.target.value, 10) || 0;
        if (!pid) return;
        try {
            const d = await RpcService.call('bom.import', 'set_line',
                [{ id: row.id, product_id: pid }], {});
            this.state.staged = (d && d.rows) || [];
        } catch (e) { this.state.error = (e && e.message) || 'Could not set that part.'; }
    }

    async searchFor(row, q) {
        if ((q || '').trim().length < 2) return;
        try {
            const hits = await RpcService.call('bom.import', 'search_parts', [{ q: q.trim() }], {}) || [];
            this.state.hits[row.id] = hits;
        } catch (e) { /* leave the row as it is */ }
    }

    async commit() {
        this.state.busy = true;
        this.state.error = '';
        try {
            const r = await RpcService.call('bom.import', 'commit',
                [{ bom_id: this.state.bomId, replace: true }], {});
            this.state.notice = 'Imported ' + r.lines + ' line(s).';
            this.state.staged = [];
            await this.loadBoms();
            await this.reload();
        } catch (e) {
            this.state.error = (e && e.message) || 'The import could not be completed.';
        } finally {
            this.state.busy = false;
        }
    }

    async discard() {
        try {
            await RpcService.call('bom.import', 'discard', [{ bom_id: this.state.bomId }], {});
            this.state.staged = [];
            this.state.hits = {};
        } catch (e) { this.state.error = (e && e.message) || 'Could not discard.'; }
    }
}
