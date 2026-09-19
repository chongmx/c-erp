/**
 * ApiKeys.js — Settings → Users & Access → API Keys.
 *
 * A key lets a script, a CI job or an AI agent use the REST API (/api/v1) AS
 * the person who created it, limited to the permissions (scopes) and projects
 * ticked here. The token is shown ONCE, right after it is created — the server
 * keeps only its SHA-256 — so the screen says so plainly and offers Copy.
 *
 * Everyone manages their own keys. An administrator also sees everyone's, to
 * revoke a key that leaked or belongs to someone who left.
 *
 * Keys are created and revoked over JSON-RPC (model api.key), which needs a
 * real signed-in session: a key can never be used to make another key.
 */
/** One table of keys — yours, or (for an administrator) everyone's. */
class ApiKeyTable extends owl.Component {
    static props = ['rows', 'showOwner?', 'onRevoke'];
    static template = owl.xml`
        <table class="ak-table">
            <thead><tr>
                <th>Name</th><th t-if="props.showOwner">Owner</th><th>Key</th><th>Permissions</th>
                <th>Projects</th><th>Expires</th><th>Last used</th><th>Status</th><th/>
            </tr></thead>
            <tbody>
                <tr t-foreach="props.rows" t-as="k" t-key="k.id" t-att-data-key-id="k.id" t-att-class="{dead: k.status !== 'active'}">
                    <td t-esc="k.name"/>
                    <td t-if="props.showOwner" t-esc="k.user"/>
                    <td><code t-esc="k.prefix + '…'"/></td>
                    <td><t t-foreach="k.scopes" t-as="s" t-key="s"><span class="ak-chip" t-esc="s"/></t></td>
                    <td>
                        <t t-if="k.projects.length"><t t-foreach="k.projects" t-as="p" t-key="p.id"><span class="ak-chip" t-esc="p.prefix"/></t></t>
                        <t t-else=""><span class="ak-muted">all</span></t>
                    </td>
                    <td t-esc="k.expires ? day(k.expires) : 'never'"/>
                    <td>
                        <t t-if="k.last_used"><t t-esc="when(k.last_used)"/>
                            <span class="ak-muted" t-esc="' · ' + k.use_count + ' calls'"/></t>
                        <t t-else=""><span class="ak-muted">never</span></t>
                    </td>
                    <td><span t-attf-class="ak-status {{ k.status }}" t-esc="k.status"/></td>
                    <td><button class="btn btn-sm btn-danger" t-if="k.status === 'active'" data-ak="revoke"
                                t-on-click="() => this.props.onRevoke(k)">Revoke</button></td>
                </tr>
                <tr t-if="!props.rows.length"><td colspan="9" class="ak-muted">No keys.</td></tr>
            </tbody>
        </table>`;
    day(iso) { return String(iso || '').slice(0, 10); }
    when(iso) {
        const d = new Date(iso);
        if (isNaN(d.getTime())) return iso;
        const sec = (Date.now() - d.getTime()) / 1000;
        if (sec < 60) return 'just now';
        if (sec < 3600) return Math.floor(sec / 60) + ' min ago';
        if (sec < 86400) return Math.floor(sec / 3600) + ' h ago';
        return this.day(iso);
    }
}

class ApiKeys extends owl.Component {
    static components = { ApiKeyTable };
    static template = owl.xml`
        <div class="ak-shell">
            <div class="ak-head">
                <h2>API Keys</h2>
                <p class="ak-sub">
                    For scripts and tools that use the c-erp API. A key acts as you, and only
                    with the permissions and projects you choose here.
                </p>
            </div>
            <div class="ak-error" t-if="state.error" t-esc="state.error"/>

            <!-- the token, once -->
            <div class="ak-new-token" t-if="state.created">
                <div class="ak-new-h">Key "<t t-esc="state.created.name"/>" created</div>
                <p>Copy it now. It will <b>not</b> be shown again — c-erp keeps only a fingerprint of it.</p>
                <div class="ak-token-row">
                    <code class="ak-token" data-ak="token" t-esc="state.created.token"/>
                    <button class="btn btn-primary btn-sm" data-ak="copy" t-on-click="copyToken"
                            t-esc="state.copied ? 'Copied' : 'Copy'"/>
                </div>
                <p class="ak-hint">Use it as a header:
                    <code>Authorization: Bearer <t t-esc="state.created.prefix"/>…</code>
                </p>
                <button class="btn btn-sm" t-on-click="dismissToken">I have stored it</button>
            </div>

            <!-- create -->
            <div class="ak-card">
                <div class="ak-card-h">
                    <span>New key</span>
                    <button class="btn btn-sm" t-if="!state.formOpen" data-ak="open-form" t-on-click="openForm">Create key…</button>
                </div>
                <t t-if="state.formOpen">
                    <div class="ak-row">
                        <label for="ak-name">Name</label>
                        <input id="ak-name" class="ak-in" data-ak="name" placeholder="e.g. Claude — c-erp development"
                               t-att-value="state.form.name" t-on-input="onName"/>
                    </div>
                    <div class="ak-row top">
                        <label>Permissions</label>
                        <div class="ak-scopes">
                            <t t-foreach="scopeGroups" t-as="grp" t-key="grp.area">
                                <div class="ak-area" t-esc="grp.area"/>
                                <t t-foreach="grp.scopes" t-as="s" t-key="s.name">
                                    <label class="ak-scope" t-att-class="{locked: isLocked(s.name)}">
                                        <input type="checkbox" t-att-data-scope="s.name"
                                               t-att-checked="hasScope(s.name)"
                                               t-att-disabled="isLocked(s.name)"
                                               t-on-change="(ev) => this.toggleScope(s.name, ev.target.checked)"/>
                                        <span class="ak-scope-b">
                                            <b t-esc="s.label"/> <code t-esc="s.name"/>
                                            <span class="ak-scope-d" t-esc="s.description"/>
                                        </span>
                                    </label>
                                </t>
                            </t>
                        </div>
                    </div>
                    <div class="ak-row top">
                        <label>Projects</label>
                        <div class="ak-projects">
                            <label class="ak-scope">
                                <input type="radio" name="ak-proj" data-ak="all-projects"
                                       t-att-checked="!state.form.restrict" t-on-change="() => this.setRestrict(false)"/>
                                <span>Every project you can see</span>
                            </label>
                            <label class="ak-scope">
                                <input type="radio" name="ak-proj" data-ak="some-projects"
                                       t-att-checked="state.form.restrict" t-on-change="() => this.setRestrict(true)"/>
                                <span>Only these:</span>
                            </label>
                            <div class="ak-proj-list" t-if="state.form.restrict">
                                <t t-foreach="state.projects" t-as="p" t-key="p.id">
                                    <label class="ak-proj">
                                        <input type="checkbox" t-att-data-project="p.task_prefix"
                                               t-att-checked="state.form.projectIds.includes(p.id)"
                                               t-on-change="(ev) => this.toggleProject(p.id, ev.target.checked)"/>
                                        <b t-esc="p.task_prefix"/> <t t-esc="p.name"/>
                                    </label>
                                </t>
                            </div>
                        </div>
                    </div>
                    <div class="ak-row">
                        <label for="ak-exp">Expires</label>
                        <select id="ak-exp" class="ak-in ak-sel" data-ak="expires" t-on-change="onExpires">
                            <option value="30"  t-att-selected="state.form.days === 30">in 30 days</option>
                            <option value="90"  t-att-selected="state.form.days === 90">in 90 days</option>
                            <option value="365" t-att-selected="state.form.days === 365">in 1 year</option>
                            <option value="0"   t-att-selected="state.form.days === 0">never</option>
                        </select>
                    </div>
                    <div class="ak-actions">
                        <button class="btn btn-primary" data-ak="create" t-att-disabled="state.busy" t-on-click="create">Create key</button>
                        <button class="btn" t-on-click="closeForm">Cancel</button>
                    </div>
                </t>
            </div>

            <!-- the keys -->
            <div class="ak-card">
                <div class="ak-card-h"><span>Your keys</span></div>
                <ApiKeyTable rows="state.mine" showOwner="false" onRevoke.bind="revoke"/>
            </div>
            <div class="ak-card" t-if="state.isAdmin">
                <div class="ak-card-h"><span>All keys</span><span class="ak-hint">administrators only</span></div>
                <ApiKeyTable rows="state.all" showOwner="true" onRevoke.bind="revoke"/>
            </div>

            <div class="ak-card ak-help">
                <div class="ak-card-h"><span>Using a key</span></div>
                <pre class="ak-pre" t-esc="example"/>
                <p class="ak-hint">The full reference is docs/reference/api-v1.md in the repository.</p>
            </div>
        </div>`;



    setup() {
        this.state = owl.useState({
            error: '', busy: false, formOpen: false, copied: false,
            scopes: [], projects: [], mine: [], all: [], isAdmin: false, created: null,
            form: this.blankForm(),
        });
        owl.onWillStart(() => this.load());
    }

    blankForm() {
        return { name: '', scopes: ['tickets:read'], restrict: false, projectIds: [], days: 90 };
    }

    async load() {
        this.state.error = '';
        try {
            const [scopes, keys, projects] = await Promise.all([
                RpcService.call('api.key', 'scopes', [], {}),
                RpcService.call('api.key', 'list', [], {}),
                RpcService.call('project.project', 'search_read', [[]],
                                { fields: ['id', 'name', 'task_prefix'], limit: 500 }),
            ]);
            this.state.scopes = scopes || [];
            this.state.mine = (keys && keys.mine) || [];
            this.state.all = (keys && keys.all) || [];
            this.state.isAdmin = !!(keys && keys.is_admin);
            this.state.projects = (projects || []).sort((a, b) =>
                String(a.task_prefix || '').localeCompare(String(b.task_prefix || '')));
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load the keys.';
        }
    }

    get scopeGroups() {
        const groups = [];
        for (const s of this.state.scopes) {
            let g = groups.find(x => x.area === s.area);
            if (!g) { g = { area: s.area, scopes: [] }; groups.push(g); }
            g.scopes.push(s);
        }
        return groups;
    }
    hasScope(name) { return this.state.form.scopes.includes(name); }
    /** Any write implies read: the answer to a write is the ticket. */
    isLocked(name) {
        return name === 'tickets:read' && this.state.form.scopes.some(s => s !== 'tickets:read');
    }
    toggleScope(name, on) {
        const set = new Set(this.state.form.scopes);
        if (on) set.add(name); else set.delete(name);
        if ([...set].some(s => s !== 'tickets:read')) set.add('tickets:read');
        this.state.form.scopes = [...set];
    }
    setRestrict(on) { this.state.form.restrict = on; }
    toggleProject(id, on) {
        const ids = this.state.form.projectIds.filter(x => x !== id);
        if (on) ids.push(id);
        this.state.form.projectIds = ids;
    }
    onName(ev)    { this.state.form.name = ev.target.value; }
    onExpires(ev) { this.state.form.days = parseInt(ev.target.value, 10) || 0; }
    openForm()    { this.state.form = this.blankForm(); this.state.formOpen = true; }
    closeForm()   { this.state.formOpen = false; }

    async create() {
        const f = this.state.form;
        if (!f.name.trim()) { this.state.error = 'Give the key a name — what will use it?'; return; }
        if (f.restrict && !f.projectIds.length) { this.state.error = 'Tick at least one project.'; return; }
        this.state.busy = true;
        this.state.error = '';
        try {
            const out = await RpcService.call('api.key', 'create_key', [{
                name: f.name.trim(), scopes: f.scopes,
                project_ids: f.restrict ? f.projectIds : [], expires_days: f.days }], {});
            this.state.created = { name: f.name.trim(), token: out.token, prefix: out.prefix };
            this.state.copied = false;
            this.state.formOpen = false;
            await this.load();
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not create the key.';
        } finally { this.state.busy = false; }
    }
    async copyToken() {
        try {
            await navigator.clipboard.writeText(this.state.created.token);
            this.state.copied = true;
        } catch (e) {
            this.state.error = 'Copy did not work here — select the key and copy it by hand.';
        }
    }
    dismissToken() { this.state.created = null; }

    async revoke(k) {
        if (!window.confirm('Revoke "' + k.name + '"? Anything using it stops working immediately.')) return;
        try {
            await RpcService.call('api.key', 'revoke', [{ id: k.id }], {});
            await this.load();
        } catch (e) { this.state.error = (e && e.message) || 'Could not revoke the key.'; }
    }

    get example() {
        const base = window.location.origin;
        return [
            '# who am I, and what may this key do?',
            'curl -H "Authorization: Bearer $CERP_KEY" ' + base + '/api/v1/me',
            '',
            '# open bugs in project CERP',
            'curl -H "Authorization: Bearer $CERP_KEY" "' + base + '/api/v1/tickets?project=CERP&type=bug"',
            '',
            '# move a ticket and comment on it',
            'curl -X PATCH -H "Authorization: Bearer $CERP_KEY" -H "Content-Type: application/json" \\',
            '     -d \'{"status": "Done"}\' ' + base + '/api/v1/tickets/CERP-12',
            'curl -X POST -H "Authorization: Bearer $CERP_KEY" -H "Content-Type: application/json" \\',
            '     -d \'{"body": "Fixed in 5ff39e6."}\' ' + base + '/api/v1/tickets/CERP-12/comments',
        ].join('\n');
    }
    day(iso) { return String(iso || '').slice(0, 10); }
    when(iso) {
        const d = new Date(iso);
        if (isNaN(d.getTime())) return iso;
        const sec = (Date.now() - d.getTime()) / 1000;
        if (sec < 60) return 'just now';
        if (sec < 3600) return Math.floor(sec / 60) + ' min ago';
        if (sec < 86400) return Math.floor(sec / 3600) + ' h ago';
        return this.day(iso);
    }
}

window.ApiKeys = ApiKeys;
