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
                                <button class="be-btn primary" t-att-disabled="state.busy or !state.pasteText.trim()"
                                        t-on-click="parseText"
                                        t-esc="state.busy ? 'Parsing…' : 'Parse and resolve'"/>
                            </t>

                            <t t-else="">
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
        this.state = owl.useState({
            boms: [], products: [], bomId: 0, bom: null, lines: [],
            staged: [], hits: {},
            aiOpen: true, pasteText: '', dragging: false,
            picker: null, pickerHits: [],
            newProduct: 0, newKind: 'pcba',
            busy: false, error: '', notice: '',
        });
        owl.onWillStart(async () => {
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
        const r = new FileReader();
        r.onload = () => { this.state.pasteText = String(r.result || ''); this.parseText(); };
        r.onerror = () => { this.state.error = 'That file could not be read.'; };
        r.readAsText(file);
    }

    async parseText() {
        if (!this.state.bomId || !this.state.pasteText.trim()) return;
        this.state.busy = true;
        this.state.error = '';
        this.state.notice = '';
        try {
            const d = await RpcService.call('bom.import', 'parse',
                [{ bom_id: this.state.bomId, text: this.state.pasteText }], {});
            this.state.staged = (d && d.rows) || [];
            this.state.pasteText = '';
        } catch (e) {
            this.state.error = (e && e.message) || 'That BOM could not be parsed.';
        } finally {
            this.state.busy = false;
        }
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
