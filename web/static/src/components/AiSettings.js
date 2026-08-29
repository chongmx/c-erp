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
                                <span t-if="state.s.enabled and state.s.configured" class="ai-pill ok">Active</span>
                                <span t-elif="state.s.configured" class="ai-pill off">Configured, disabled</span>
                                <span t-else="" class="ai-pill none">No key</span>
                            </span>
                        </div>
                        <div class="ai-row">
                            <span class="ai-label">API key</span>
                            <span class="ai-val">
                                <t t-if="state.s.configured">
                                    <code class="ai-key">sk-ant-··················<t t-esc="state.s.key_tail"/></code>
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
                        <h3 class="ai-h3">Set the API key</h3>
                        <p class="ai-help">
                            From <b>console.anthropic.com</b> → API keys. Stored in the database and
                            never sent back to this screen — leave it blank to keep the current one.
                        </p>
                        <div class="ai-form">
                            <input class="ai-input" type="password" autocomplete="off" spellcheck="false"
                                   placeholder="sk-ant-..."
                                   t-att-value="state.keyInput"
                                   t-on-input="ev => state.keyInput = ev.target.value"/>
                            <button class="ai-btn primary" t-on-click="save" t-att-disabled="state.busy">Save</button>
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
                                <select t-on-change="ev => this.set('provider', ev.target.value)">
                                    <option value="anthropic" t-att-selected="state.s.provider === 'anthropic'">Anthropic (Claude)</option>
                                    <option value="mock"      t-att-selected="state.s.provider === 'mock'">Mock (tests, no network)</option>
                                </select>
                            </label>
                            <label class="ai-field">
                                <span class="ai-label">Model</span>
                                <select t-on-change="ev => this.set('model', ev.target.value)">
                                    <option value="claude-sonnet-5"          t-att-selected="state.s.model === 'claude-sonnet-5'">Sonnet 5 — balanced (default)</option>
                                    <option value="claude-opus-5"            t-att-selected="state.s.model === 'claude-opus-5'">Opus 5 — hardest lookups</option>
                                    <option value="claude-haiku-4-5-20251001" t-att-selected="state.s.model === 'claude-haiku-4-5-20251001'">Haiku 4.5 — cheap and fast</option>
                                </select>
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
                        <button class="ai-btn ghost" t-on-click="reveal" t-att-disabled="state.busy || !state.s.configured">
                            Show setup lines
                        </button>
                        <t t-if="state.reveal">
                            <pre class="ai-pre"><t t-esc="state.reveal.systemd"/>
<t t-esc="state.reveal.docker"/>
<t t-esc="state.reveal.shell"/></pre>
                            <button class="ai-btn ghost" t-on-click="() => state.reveal = null">Hide</button>
                        </t>
                    </div>

                    <div class="ai-warn">
                        The key is stored in the database, so <b>every backup carries it</b>.
                        Treat a database dump as a credential.
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
        });
        owl.onWillStart(() => this.load());
    }

    async load() {
        this.state.loading = true;
        try {
            this.state.s = await RpcService.call('ir.ai.settings', 'get', [{}], {});
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
    set(field, value) { return this.write({ [field]: value }); }

    async save() {
        const k = (this.state.keyInput || '').trim();
        await this.write(k ? { api_key: k } : {});
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
