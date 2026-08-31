/**
 * AiSettings.js — Settings → AI Agent (docs/110).
 *
 * The screen for a credential, which makes it different from every other
 * settings screen here: the API key is WRITE-ONLY. It is sent to be stored and
 * never sent back, so this component cannot display it and does not try. What
 * it shows instead is `configured` and the last four characters — enough to
 * tell two keys apart, useless to anyone reading over a shoulder.
 *
 * The one exception is "Show setup lines", which calls reveal_for_setup. That
 * path exists because the operator sometimes genuinely has to paste the key
 * into a systemd unit, and it is audited on the server for exactly that
 * reason. The button says so before it does it.
 */
class AiSettings extends owl.Component {
    static template = owl.xml`
        <div class="ai-shell">
            <div class="ai-head">
                <h2 class="ai-title">AI Agent</h2>
                <span class="ai-sub">
                    Configuration lives in the database, so a restore carries it with everything else.
                </span>
            </div>

            <t t-if="state.loading"><div class="ai-note">Loading…</div></t>
            <t t-else="">
                <div class="ai-body">

                    <!-- status -->
                    <div class="ai-card">
                        <div class="ai-row">
                            <span class="ai-label">Status</span>
                            <span class="ai-val">
                                <span t-if="state.s.enabled and activeProvider.configured" class="ai-pill ok">Active</span>
                                <span t-elif="activeProvider.configured" class="ai-pill off">Configured, disabled</span>
                                <span t-else="" class="ai-pill none">No key</span>
                            </span>
                        </div>
                        <div class="ai-row">
                            <span class="ai-label">API key</span>
                            <span class="ai-val">
                                <t t-if="activeProvider.configured">
                                    <code class="ai-key">··················<t t-esc="activeProvider.key_tail"/></code>
                                    <button class="ai-btn ghost" t-on-click="clearKey">Remove</button>
                                </t>
                                <t t-else=""><span class="ai-muted">not set</span></t>
                            </span>
                        </div>
                        <div class="ai-row" t-if="state.s.last_ok_at">
                            <span class="ai-label">Last good check</span>
                            <span class="ai-val ai-muted" t-esc="state.s.last_ok_at.slice(0,19)"/>
                        </div>
                        <div class="ai-row" t-if="state.s.last_error">
                            <span class="ai-label">Last error</span>
                            <span class="ai-val ai-err" t-esc="state.s.last_error"/>
                        </div>
                    </div>

                    <!-- the key -->
                    <div class="ai-card">
                        <h3 class="ai-h3">API key for <t t-esc="activeLabel"/></h3>
                        <p class="ai-help">
                            Each provider keeps its own key, so switching does not mean re-entering
                            one. Stored in the database and never sent back to this screen — leave
                            it blank to keep the current one.
                        </p>
                        <div class="ai-form">
                            <input class="ai-input" type="password" autocomplete="off" spellcheck="false"
                                   placeholder="sk-ant-..."
                                   t-att-value="state.keyInput"
                                   t-on-input="ev => state.keyInput = ev.target.value"/>
                            <button class="ai-btn primary" t-on-click="save" t-att-disabled="state.busy">Save</button>
                        </div>
                    </div>

                    <!-- workspace id: Anthropic only, and long enough to need its own row -->
                    <div class="ai-card" t-if="state.s.provider === 'anthropic'">
                        <h3 class="ai-h3">Anthropic workspace</h3>
                        <p class="ai-help">
                            Identity-linked keys must name the workspace the request acts in.
                            Find it in the Console under <b>Settings → Workspaces</b> (it looks like
                            <code>wrkspc_…</code>). Leave blank for a workspace-scoped key.
                        </p>
                        <div class="ai-form">
                            <input class="ai-ws" type="text" spellcheck="false"
                                   placeholder="wrkspc_..."
                                   t-att-value="state.wsInput"
                                   t-on-input="ev => state.wsInput = ev.target.value"
                                   t-on-keydown="onWsKey"/>
                            <button class="ai-btn primary" t-on-click="saveWorkspace"
                                    t-att-disabled="state.busy">Save workspace id</button>
                            <span class="ai-result ok" t-if="state.wsSaved">Saved</span>
                        </div>
                    </div>

                    <!-- behaviour -->
                    <div class="ai-card">
                        <h3 class="ai-h3">Behaviour</h3>
                        <div class="ai-grid">
                            <label class="ai-field">
                                <span class="ai-label">Enabled</span>
                                <input type="checkbox" t-att-checked="state.s.enabled"
                                       t-on-change="ev => this.set('enabled', ev.target.checked)"/>
                            </label>
                            <label class="ai-field">
                                <span class="ai-label">Provider</span>
                                <!-- From the provider table, not a hardcoded list: adding one is
                                     a row, not a template edit. -->
                                <select t-on-change="ev => this.set('provider', ev.target.value)">
                                    <option t-foreach="state.providers" t-as="p" t-key="p.name"
                                            t-att-value="p.name"
                                            t-att-selected="state.s.provider === p.name"
                                            t-esc="p.label + (p.configured ? '' : '  — no key')"/>
                                </select>
                            </label>
                            <label class="ai-field">
                                <span class="ai-label">Model</span>
                                <!-- The model belongs to the PROVIDER, not to the settings row.
                                     A shared dropdown offered Claude models while Grok was
                                     selected, which is a setting you cannot act on. -->
                                <input class="ai-num wide" type="text" spellcheck="false"
                                       placeholder="model id"
                                       t-att-value="state.modelInput"
                                       t-on-input="ev => state.modelInput = ev.target.value"
                                       t-on-keydown="ev => ev.key === 'Enter' &amp;&amp; this.saveModel()"
                                       t-on-blur="saveModel"/>
                            </label>
                            <label class="ai-field">
                                <span class="ai-label">Max output tokens</span>
                                <input class="ai-num" type="number" min="256" max="16384"
                                       t-att-value="state.s.max_output_tokens"
                                       t-on-change="ev => this.set('max_output_tokens', parseInt(ev.target.value, 10))"/>
                            </label>
                            <label class="ai-field">
                                <span class="ai-label">Daily call cap</span>
                                <input class="ai-num" type="number" min="1" max="10000"
                                       t-att-value="state.s.daily_call_cap"
                                       t-on-change="ev => this.set('daily_call_cap', parseInt(ev.target.value, 10))"/>
                            </label>
                            <div class="ai-field">
                                <span class="ai-label">Used today</span>
                                <span class="ai-val ai-muted">
                                    <t t-esc="state.s.calls_today"/> / <t t-esc="state.s.daily_call_cap"/>
                                </span>
                            </div>
                        </div>
                    </div>

                    <!-- test -->
                    <div class="ai-card">
                        <h3 class="ai-h3">Check it</h3>
                        <div class="ai-form">
                            <button class="ai-btn" t-on-click="test" t-att-disabled="state.busy">Test connection</button>
                            <span t-if="state.test" t-att-class="'ai-result ' + (state.test.ok ? 'ok' : 'bad')"
                                  t-esc="state.test.detail"/>
                        </div>
                    </div>

                    <!-- reveal -->
                    <div class="ai-card">
                        <h3 class="ai-h3">Use the key elsewhere</h3>
                        <p class="ai-help">
                            For a systemd unit or a container, which this process cannot configure from
                            the inside. <b>This shows the key on screen and records who revealed it.</b>
                        </p>
                        <button class="ai-btn ghost" t-on-click="reveal" t-att-disabled="state.busy || !activeProvider.configured">
                            Show setup lines
                        </button>
                        <t t-if="state.reveal">
                            <pre class="ai-pre"><t t-esc="state.reveal.systemd"/>
<t t-esc="state.reveal.docker"/>
<t t-esc="state.reveal.shell"/></pre>
                            <button class="ai-btn ghost" t-on-click="() => state.reveal = null">Hide</button>
                        </t>
                    </div>

                    <!-- prompts -->
                    <div class="ai-card">
                        <h3 class="ai-h3">Prompts</h3>
                        <p class="ai-help">
                            What the agent is actually told, for each job it does. These ship as
                            files under <code>prompts/</code> so a deployment team can edit them in
                            git. Editing here stores an override in the database instead — good for
                            trying something, not for keeping it.
                        </p>

                        <div class="ai-tasks">
                            <button t-foreach="state.prompts" t-as="p" t-key="p.task"
                                    t-attf-class="ai-task{{ state.promptSel === p.task ? ' active' : '' }}"
                                    t-on-click="() => this.pickPrompt(p.task)">
                                <span class="ai-task-n" t-esc="p.label"/>
                                <span t-attf-class="ai-src {{ p.source }}" t-esc="p.source"/>
                            </button>
                        </div>

                        <t t-if="currentPrompt">
                            <p class="ai-help" t-esc="currentPrompt.about"/>

                            <!-- Which text is LIVE, said plainly. Somebody debugging a
                                 strange answer needs to know whether they are reading
                                 what is sent or a file that is being ignored. -->
                            <div class="ai-note warn" t-if="currentPrompt.source === 'override'">
                                An override in the database is being used. The file
                                <code t-esc="currentPrompt.file"/> is <b>not</b> in effect.
                                <t t-if="currentPrompt.edited_at">
                                    Edited <t t-esc="currentPrompt.edited_at"/>.
                                </t>
                            </div>
                            <div class="ai-note bad" t-elif="currentPrompt.source === 'compiled'">
                                <b><code t-esc="currentPrompt.file"/> is missing.</b>
                                A short built-in copy is being used so the feature still works —
                                restore the file or set ERP_PROMPT_DIR.
                            </div>
                            <div class="ai-note" t-else="">
                                Live from <code t-esc="currentPrompt.file"/>.
                            </div>

                            <div class="ai-ph">
                                <t t-foreach="currentPrompt.placeholders" t-as="ph" t-key="ph.name">
                                    <code t-attf-class="ai-chip{{ ph.required ? ' req' : '' }}"
                                          t-att-title="ph.required ? 'Required — the prompt cannot be saved without it' : 'Optional'"
                                          t-esc="'{{' + ph.name + '}}'"/>
                                </t>
                                <span class="ai-muted">replaced when the prompt is sent</span>
                            </div>

                            <textarea class="ai-prompt" spellcheck="false"
                                      t-att-value="state.promptBody"
                                      t-on-input="(ev) => { state.promptBody = ev.target.value; }"/>

                            <div class="ai-form">
                                <button class="ai-btn" t-on-click="savePrompt"
                                        t-att-disabled="state.busy or state.promptBody === currentPrompt.body">
                                    Save override
                                </button>
                                <button class="ai-btn ghost" t-on-click="resetPrompt"
                                        t-att-disabled="state.busy or !currentPrompt.overridden">
                                    Reset to file
                                </button>
                                <button class="ai-btn ghost"
                                        t-att-disabled="state.promptBody === currentPrompt.file_body"
                                        t-on-click="() => { state.promptBody = currentPrompt.file_body; }">
                                    Load the file's text
                                </button>
                                <span t-if="state.promptMsg" class="ai-result ok" t-esc="state.promptMsg"/>
                            </div>
                        </t>
                    </div>

                    <div class="ai-warn">
                        The key is stored in the database, so <b>every backup carries it</b>.
                        Treat a database dump as a credential.
                    </div>
                    <div class="ai-warn">
                        A prompt is instructions to a model whose answers reach your catalogue.
                        The code's own safeguards — the unit checks, the staging queue, the rule
                        that the agent never picks a part — are <b>not</b> in these files and
                        cannot be edited away.
                    </div>
                </div>
            </t>

            <div t-if="state.error" class="ai-error" t-esc="state.error"/>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            loading: true, busy: false, error: '',
            s: {}, keyInput: '', test: null, reveal: null,
            wsInput: '', wsSaved: false,
            providers: [], modelInput: '',
            prompts: [], promptSel: '', promptBody: '', promptMsg: '',
        });
        owl.onWillStart(() => this.load());
    }

    // ---- prompts ------------------------------------------------------------
    get currentPrompt() {
        return this.state.prompts.find(p => p.task === this.state.promptSel) || null;
    }

    async loadPrompts(keepSelection) {
        try {
            this.state.prompts = await RpcService.call('ir.ai.settings', 'prompts', [{}], {}) || [];
            if (!keepSelection || !this.currentPrompt)
                this.state.promptSel = (this.state.prompts[0] || {}).task || '';
            const p = this.currentPrompt;
            this.state.promptBody = p ? p.body : '';
        } catch (e) {
            this.state.prompts = [];
        }
    }

    pickPrompt(task) {
        this.state.promptSel = task;
        this.state.promptMsg = '';
        const p = this.currentPrompt;
        this.state.promptBody = p ? p.body : '';
    }

    async savePrompt() {
        const p = this.currentPrompt;
        if (!p) return;
        this.state.busy = true; this.state.error = ''; this.state.promptMsg = '';
        try {
            await RpcService.call('ir.ai.settings', 'save_prompt',
                [{ task: p.task, body: this.state.promptBody }], {});
            await this.loadPrompts(true);
            this.state.promptMsg = 'Saved. This override is now what gets sent.';
        } catch (e) {
            this.state.error = (e && e.message) || 'That prompt could not be saved.';
        }
        this.state.busy = false;
    }

    async resetPrompt() {
        const p = this.currentPrompt;
        if (!p) return;
        this.state.busy = true; this.state.error = ''; this.state.promptMsg = '';
        try {
            await RpcService.call('ir.ai.settings', 'reset_prompt', [{ task: p.task }], {});
            await this.loadPrompts(true);
            this.state.promptMsg = 'Override removed — the file is in effect again.';
        } catch (e) {
            this.state.error = (e && e.message) || 'That prompt could not be reset.';
        }
        this.state.busy = false;
    }

    async load() {
        this.state.loading = true;
        try {
            this.state.s = await RpcService.call('ir.ai.settings', 'get', [{}], {});
            // Seed the box from the stored value, so it shows what is in effect
            // rather than an empty field that looks like nothing is set.
            this.state.wsInput = this.state.s.workspace_id || '';
            this.state.providers = await RpcService.call('ir.ai.settings', 'providers', [{}], {});
            this.state.modelInput = this.activeProvider.model || '';
            await this.loadPrompts(false);
            this.state.error = '';
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.loading = false;
    }

    async write(vals) {
        this.state.busy = true;
        try {
            this.state.s = await RpcService.call('ir.ai.settings', 'save', [vals], {});
            this.state.error = '';
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.busy = false;
    }

    // Each control saves on change. A settings screen with a single Save
    // button invites half-applied configuration when somebody navigates away,
    // and there is no partial state here worth protecting.
    async set(field, value) {
        await this.write({ [field]: value });
        // Provider changed: the key status, model and workspace all belong to
        // the new one, so re-read rather than showing the previous provider's.
        if (field === 'provider') { this.state.test = null; await this.load(); }
    }

    // Never undefined: the template reads .configured and .model on every
    // render, including the first one before providers have loaded.
    get activeProvider() {
        return this.state.providers.find(p => p.name === this.state.s.provider)
            || { name: this.state.s.provider || '', label: this.state.s.provider || '', configured: false, key_tail: '', model: '' };
    }
    get activeLabel() { return this.activeProvider.label || 'this provider'; }

    async saveProvider(vals) {
        this.state.busy = true;
        try {
            this.state.providers = await RpcService.call('ir.ai.settings', 'save_provider',
                [Object.assign({ name: this.state.s.provider }, vals)], {});
            this.state.error = '';
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.busy = false;
    }

    async saveModel() {
        const m = (this.state.modelInput || '').trim();
        if (!m || m === this.activeProvider.model) return;
        await this.saveProvider({ model: m });
        this.state.test = null;
    }

    onWsKey(ev) { if (ev.key === 'Enter') this.saveWorkspace(); }

    // Explicit, not save-on-blur: this is a value pasted from another system,
    // and a field that saves as you tab past it gives no confirmation that it
    // took — which is exactly the thing you want to be sure of here.
    async saveWorkspace() {
        await this.saveProvider({ workspace_id: (this.state.wsInput || '').trim() });
        this.state.wsSaved = true;
        this.state.test = null;                       // any earlier result is now stale
        setTimeout(() => { this.state.wsSaved = false; }, 2500);
    }

    async save() {
        const k = (this.state.keyInput || '').trim();
        if (k) await this.saveProvider({ api_key: k });
        this.state.keyInput = '';        // never keep it in client memory
        this.state.test = null;
    }

    async clearKey() {
        if (!window.confirm('Remove the stored API key? The agent will stop working until a new one is set.')) return;
        this.state.busy = true;
        try {
            this.state.s = await RpcService.call('ir.ai.settings', 'clear_key', [{}], {});
            this.state.reveal = null;
            this.state.test = null;
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.busy = false;
    }

    async test() {
        this.state.busy = true;
        this.state.test = null;
        try {
            this.state.test = await RpcService.call('ir.ai.settings', 'test_connection', [{}], {});
            await this.load();
        } catch (e) {
            this.state.test = { ok: false, detail: String((e && e.message) || e) };
        }
        this.state.busy = false;
    }

    async reveal() {
        if (!window.confirm('Show the API key on screen? This is recorded in the audit log.')) return;
        this.state.busy = true;
        try {
            this.state.reveal = await RpcService.call('ir.ai.settings', 'reveal_for_setup', [{}], {});
        } catch (e) {
            this.state.error = String((e && e.message) || e);
        }
        this.state.busy = false;
    }
}
