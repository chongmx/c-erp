/**
 * ParamKeywords.js — Products ▸ Configuration ▸ Parameter Keywords.  (CERP-8)
 *
 * The list of quantities this catalogue measures, and every spelling that
 * resolves to each. It exists because a datasheet writes the same quantity a
 * dozen ways — "Resistance", "Ohms", "resistance (Ω)", "R" — and left alone
 * each becomes its own parameter, which is how a parametric search quietly
 * stops finding siblings.
 *
 * Two panels, in the order the work actually happens:
 *
 *   NEEDS A DECISION — every name in the catalogue the vocabulary does not
 *   know, with what the server thinks it is. Each row offers the two answers
 *   to "should we create a new one, or put it under an existing parameter?":
 *   Add as new, or Merge into ▸. Merging renames the parameter on every
 *   product that used the old name and keeps the old spelling as an alias, so
 *   the question is never asked twice.
 *
 *   THE VOCABULARY — the keywords themselves, their aliases, and how many
 *   parameters use each. Renaming a keyword renames it across the catalogue.
 *
 * The suggestion is the server's: it matches on the normalised name first
 * (case and punctuation are not different names) and then by edit distance,
 * so "Tolerence" lands on "Tolerance". The screen never guesses on its own,
 * and never acts without a click — a merge rewrites product data.
 */
class ParamKeywords extends owl.Component {
    static template = owl.xml`
        <div class="pk-shell">
            <div class="pk-head">
                <h2>Parameter Keywords</h2>
                <p class="pk-sub">
                    The quantities this catalogue measures, and the spellings that mean each of
                    them. A lookup that sends an unknown name is never rejected — it lands here,
                    with a suggestion.
                </p>
            </div>
            <div class="pk-error" t-if="state.error" t-esc="state.error"/>
            <div class="pk-notice" t-if="state.notice" t-esc="state.notice"/>

            <t t-if="state.loading"><div class="pk-hint">Loading…</div></t>
            <t t-else="">
                <!-- needs a decision -->
                <div class="pk-card">
                    <div class="pk-card-h">
                        <span>Needs a decision</span>
                        <span class="pk-count" t-esc="state.unmatched.length + ' name(s)'"/>
                    </div>
                    <t t-if="!state.unmatched.length">
                        <div class="pk-hint">Every parameter name in the catalogue is in the vocabulary.</div>
                    </t>
                    <table class="pk-table" t-else="">
                        <thead><tr>
                            <th>Name in the catalogue</th><th>Used by</th>
                            <th>What this looks like</th><th>Decide</th>
                        </tr></thead>
                        <tbody>
                            <tr t-foreach="state.unmatched" t-as="u" t-key="u.name"
                                t-att-data-unmatched="u.name">
                                <td><code t-esc="u.name"/></td>
                                <td t-esc="u.uses + ' part(s)'"/>
                                <td>
                                    <t t-if="u.kind === 'similar'">
                                        <span class="pk-verdict similar">close to</span>
                                        <b t-esc="u.suggestion"/>
                                        <span class="pk-score" t-esc="scoreOf(u)"/>
                                    </t>
                                    <t t-else="">
                                        <span class="pk-verdict unknown">new</span>
                                        <span class="pk-muted" t-if="u.candidates.length"
                                              t-esc="'nearest: ' + u.candidates.map(c => c.name).join(', ')"/>
                                        <span class="pk-muted" t-else="">nothing like it in the vocabulary</span>
                                    </t>
                                </td>
                                <td class="pk-actions">
                                    <button class="btn btn-sm btn-primary" data-pk="adopt"
                                            t-att-disabled="state.busy"
                                            t-on-click="() => this.adopt(u)">Add as new</button>
                                    <select class="pk-select" t-att-disabled="state.busy"
                                            t-on-change="(ev) => this.mergeInto(u, ev.target.value)">
                                        <option value="">Merge into…</option>
                                        <t t-foreach="mergeOptions(u)" t-as="k" t-key="k.id">
                                            <option t-att-value="k.id" t-esc="k.name"/>
                                        </t>
                                    </select>
                                </td>
                            </tr>
                        </tbody>
                    </table>
                </div>

                <!-- try a name -->
                <div class="pk-card">
                    <div class="pk-card-h"><span>Try a name</span></div>
                    <div class="pk-try">
                        <input class="pk-in" data-pk="try" placeholder="e.g. Rds(on), Ohms, Tolerence"
                               t-on-input="onTry"/>
                        <span class="pk-try-out" t-if="state.tryVerdict" t-esc="state.tryVerdict"/>
                    </div>
                    <div class="pk-hint">
                        The same answer a lookup gets. Case and punctuation are never a different
                        name: "RDS_ON", "Rds(on)" and "rds on" are one.
                    </div>
                </div>

                <!-- the vocabulary -->
                <div class="pk-card">
                    <div class="pk-card-h">
                        <span>The vocabulary</span>
                        <button class="btn btn-sm" data-pk="new" t-if="!state.formOpen"
                                t-on-click="() => this.openForm()">Add a parameter…</button>
                    </div>
                    <t t-if="state.formOpen">
                        <div class="pk-row">
                            <label for="pk-new-name">Name</label>
                            <input id="pk-new-name" class="pk-in" data-pk="new-name"
                                   placeholder="e.g. Channel Resistance"
                                   t-att-value="state.form.name" t-on-input="onFormName"/>
                        </div>
                        <div class="pk-row">
                            <label for="pk-new-alias">Other spellings</label>
                            <input id="pk-new-alias" class="pk-in" data-pk="new-alias"
                                   placeholder="separate with commas — Rds(on), RDS ON"
                                   t-att-value="state.form.aliases" t-on-input="onFormAliases"/>
                        </div>
                        <div class="pk-row">
                            <label for="pk-new-kind">Measures</label>
                            <select id="pk-new-kind" class="pk-select" data-pk="new-kind"
                                    t-on-change="onFormKind">
                                <option value="">(dimensionless)</option>
                                <t t-foreach="state.kinds" t-as="q" t-key="q">
                                    <option t-att-value="q" t-esc="q"/>
                                </t>
                            </select>
                        </div>
                        <div class="pk-editbar">
                            <button class="btn btn-sm btn-primary" data-pk="save-new"
                                    t-att-disabled="state.busy" t-on-click="createKeyword">Add</button>
                            <button class="btn btn-sm" t-on-click="() => this.closeForm()">Cancel</button>
                        </div>
                    </t>

                    <table class="pk-table">
                        <thead><tr>
                            <th>Parameter</th><th>Measures</th><th>Used by</th>
                            <th>Also written as</th><th/>
                        </tr></thead>
                        <tbody>
                            <tr t-foreach="state.keywords" t-as="k" t-key="k.id"
                                t-att-data-keyword="k.name" t-att-class="{inactive: !k.active}">
                                <td>
                                    <b t-esc="k.name"/>
                                    <div class="pk-advice" t-if="k.advice" t-esc="k.advice"/>
                                </td>
                                <td class="pk-muted" t-esc="k.quantity_kind || '—'"/>
                                <td t-esc="k.uses"/>
                                <td>
                                    <t t-foreach="k.aliases" t-as="a" t-key="a.id">
                                        <span class="pk-chip">
                                            <t t-esc="a.alias"/>
                                            <button class="pk-x" title="Remove this spelling"
                                                    t-on-click="() => this.removeAlias(a)">×</button>
                                        </span>
                                    </t>
                                    <button class="pk-add" t-on-click="() => this.addAlias(k)">+ add</button>
                                </td>
                                <td class="pk-actions">
                                    <button class="btn btn-sm" t-on-click="() => this.rename(k)">Rename</button>
                                </td>
                            </tr>
                        </tbody>
                    </table>
                </div>
            </t>
        </div>`;

    setup() {
        this.state = owl.useState({
            loading: true, busy: false, error: '', notice: '',
            keywords: [], unmatched: [], kinds: [],
            tryVerdict: '',
            formOpen: false, form: { name: '', aliases: '', kind: '' },
        });
        owl.onWillStart(() => this.load());
    }

    // --- data ------------------------------------------------------
    async rpc(method, args) {
        return RpcService.call('part.parameter.keyword', method, args || [{}], {});
    }

    async load() {
        this.state.error = '';
        try {
            const [kw, un] = await Promise.all([this.rpc('list'), this.rpc('unmatched')]);
            this.state.keywords  = kw || [];
            this.state.unmatched = un || [];
            // The quantities on offer are the ones the unit table actually
            // knows — a keyword that measures something with no units is a
            // dead end, and inventing a kind here would create one.
            const units = await RpcService.call('part.unit', 'search_read', [[]],
                { fields: ['quantity_kind'], limit: 500 });
            const kinds = new Set();
            (units || []).forEach(u => { if (u.quantity_kind) kinds.add(u.quantity_kind); });
            this.state.kinds = [...kinds].sort();
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load the parameter vocabulary.';
        } finally {
            this.state.loading = false;
        }
    }

    async reload(notice) {
        this.state.notice = notice || '';
        await this.load();
    }

    // --- needs a decision ------------------------------------------
    scoreOf(u) {
        const c = (u.candidates || [])[0];
        return c ? '(' + Math.round(c.score * 100) + '% alike)' : '';
    }

    /** Keywords worth offering first: the server's candidates, then the rest. */
    mergeOptions(u) {
        const first = (u.candidates || []).map(c => ({ id: c.keyword_id, name: c.name }));
        const seen = new Set(first.map(f => f.id));
        const rest = this.state.keywords.filter(k => !seen.has(k.id))
                         .map(k => ({ id: k.id, name: k.name }));
        return first.concat(rest);
    }

    async adopt(u) {
        await this.act(() => this.rpc('create_keyword', [{ name: u.name }]),
                       '"' + u.name + '" is now a parameter of its own.');
    }

    async mergeInto(u, keywordId) {
        const id = Number(keywordId);
        if (!id) return;
        const target = this.state.keywords.find(k => k.id === id);
        if (!confirm('Rename "' + u.name + '" to "' + (target ? target.name : '') + '" on ' +
                     u.uses + ' part(s)?\n\nThe old spelling is kept, so a lookup that sends it ' +
                     'again lands on the right parameter.')) {
            await this.load();      // put the select back
            return;
        }
        await this.act(() => this.rpc('merge', [{ from_name: u.name, into_keyword_id: id }]),
                       null, r => 'Renamed on ' + (r && r.renamed) + ' part(s).');
    }

    // --- the vocabulary --------------------------------------------
    openForm()  { this.state.formOpen = true; }
    closeForm() { this.state.formOpen = false; this.state.form = { name: '', aliases: '', kind: '' }; }
    onFormName(ev)    { this.state.form.name = ev.target.value; }
    onFormAliases(ev) { this.state.form.aliases = ev.target.value; }
    onFormKind(ev)    { this.state.form.kind = ev.target.value; }

    async createKeyword() {
        const name = (this.state.form.name || '').trim();
        if (!name) { this.state.error = 'A parameter needs a name.'; return; }
        const aliases = (this.state.form.aliases || '')
            .split(',').map(s => s.trim()).filter(Boolean);
        await this.act(() => this.rpc('create_keyword',
            [{ name, aliases, quantity_kind: this.state.form.kind || '' }]),
            '"' + name + '" added.');
        this.closeForm();
    }

    async addAlias(k) {
        const alias = prompt('Another way "' + k.name + '" is written:');
        if (!alias) return;
        await this.act(() => this.rpc('add_alias', [{ keyword_id: k.id, alias }]),
                       '"' + alias + '" now resolves to ' + k.name + '.');
    }

    async removeAlias(a) {
        if (!confirm('Stop treating "' + a.alias + '" as a spelling of this parameter?')) return;
        await this.act(() => this.rpc('remove_alias', [{ id: a.id }]), 'Spelling removed.');
    }

    async rename(k) {
        const name = prompt('Rename "' + k.name + '" to:\n\nThis renames it on every part that ' +
                            'uses it. The old name is kept as a spelling.', k.name);
        if (!name || name === k.name) return;
        await this.act(() => this.rpc('rename', [{ keyword_id: k.id, name }]),
                       null, r => 'Renamed on ' + (r && r.renamed) + ' part(s).');
    }

    // --- try a name ------------------------------------------------
    async onTry(ev) {
        const name = ev.target.value.trim();
        if (!name) { this.state.tryVerdict = ''; return; }
        try {
            const r = await this.rpc('suggest', [[name]]);
            const v = (r || [])[0];
            if (!v) { this.state.tryVerdict = ''; return; }
            // Only answer the question that is still on screen: a slow reply
            // for "Ohm" must not overwrite the verdict for "Ohms".
            if (ev.target.value.trim() !== name) return;
            this.state.tryVerdict =
                v.kind === 'exact'   ? 'already a parameter' :
                v.kind === 'alias'   ? 'a spelling of ' + v.canonical :
                v.kind === 'similar' ? 'new — closest is ' + v.canonical :
                                       'new — nothing like it yet';
        } catch (_) { /* the hint is a nicety; the screen still works */ }
    }

    /** Run one action, report it, and reload. Errors land on screen, not in the console. */
    async act(fn, notice, noticeFrom) {
        this.state.busy = true; this.state.error = ''; this.state.notice = '';
        try {
            const r = await fn();
            await this.reload(noticeFrom ? noticeFrom(r) : notice);
        } catch (e) {
            this.state.error = (e && e.message) || 'That did not work.';
        } finally {
            this.state.busy = false;
        }
    }
}

window.ParamKeywords = ParamKeywords;
