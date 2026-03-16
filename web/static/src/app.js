/**
 * app.js — IR-driven OWL application.
 *
 * Flow after login:
 *   1. load_menus()  → build sidebar from ir.ui.menu
 *   2. Click menu    → loadAction(actionId) → get action dict
 *   3. Action loaded → getViews(model, [[false,'list']]) → render list
 *   4. Click record  → getViews(model, [[false,'form']]) → render form
 */
const { Component, useState, xml, mount, onMounted } = owl;

// ----------------------------------------------------------------
// ListView — renders a list from get_views + search_read
// ----------------------------------------------------------------
class ListView extends Component {
    static template = xml`
        <div class="view-list">
            <div class="view-toolbar">
                <button class="btn btn-primary" t-on-click="onNew">New</button>
            </div>
            <t t-if="state.loading">
                <div class="loading">Loading…</div>
            </t>
            <t t-elif="state.error">
                <div class="error" t-esc="state.error"/>
            </t>
            <t t-else="">
                <table class="list-table">
                    <thead>
                        <tr>
                            <t t-foreach="columns" t-as="col" t-key="col.name">
                                <th t-esc="col.label"/>
                            </t>
                        </tr>
                    </thead>
                    <tbody>
                        <t t-foreach="state.records" t-as="rec" t-key="rec.id">
                            <tr class="list-row" t-on-click="() => this.onRowClick(rec.id)">
                                <t t-foreach="columns" t-as="col" t-key="col.name">
                                    <td><t t-esc="formatCell(rec[col.name])"/></td>
                                </t>
                            </tr>
                        </t>
                        <t t-if="state.records.length === 0">
                            <tr><td t-att-colspan="columns.length" class="empty-row">No records found.</td></tr>
                        </t>
                    </tbody>
                </table>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ loading: true, records: [], error: '' });
        onMounted(() => this.load());
    }

    get columns() {
        const viewDef = this.props.viewDef || {};
        const fields  = viewDef.fields || {};
        return Object.entries(fields).map(([name, meta]) => ({
            name,
            label: meta.string || name,
        }));
    }

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const action = this.props.action;
            const cols   = this.columns.map(c => c.name);
            const recs   = await RpcService.call(
                action.res_model, 'search_read',
                [[]], { fields: cols, limit: 80 });
            this.state.records = Array.isArray(recs) ? recs : [];
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    formatCell(val) {
        if (val === null || val === undefined || val === false) return '';
        if (Array.isArray(val)) return val[1] ?? val[0] ?? '';
        return String(val);
    }

    onRowClick(id) {
        this.props.onOpenForm(id);
    }

    onNew() {
        this.props.onOpenForm(null);
    }
}

// ----------------------------------------------------------------
// FormView — renders a form from get_views + read
// ----------------------------------------------------------------
class FormView extends Component {
    static template = xml`
        <div class="view-form">
            <div class="view-toolbar">
                <button class="btn" t-on-click="onBack">← Back</button>
                <button t-if="!state.isNew" class="btn btn-primary" t-on-click="onSave">Save</button>
                <button t-if="!state.isNew" class="btn btn-danger" t-on-click="onDelete">Delete</button>
                <button t-if="state.isNew"  class="btn btn-primary" t-on-click="onCreate">Create</button>
            </div>
            <t t-if="state.loading">
                <div class="loading">Loading…</div>
            </t>
            <t t-elif="state.error">
                <div class="error" t-esc="state.error"/>
            </t>
            <t t-else="">
                <div class="form-body">
                    <t t-foreach="formFields" t-as="f" t-key="f.name">
                        <div class="form-row">
                            <label class="form-label" t-esc="f.label"/>
                            <input class="form-input"
                                   t-att-type="f.type === 'boolean' ? 'checkbox' : 'text'"
                                   t-att-checked="f.type === 'boolean' ? !!state.record[f.name] : undefined"
                                   t-att-value="f.type !== 'boolean' ? formatValue(state.record[f.name]) : undefined"
                                   t-on-input="(e) => this.onFieldChange(f.name, e.target.value)"
                                   t-on-change="(e) => f.type === 'boolean' ? this.onFieldChange(f.name, e.target.checked) : null"/>
                        </div>
                    </t>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ loading: true, record: {}, isNew: !this.props.recordId, error: '' });
        onMounted(() => this.load());
    }

    get formFields() {
        const fields = (this.props.viewDef || {}).fields || {};
        return Object.entries(fields).map(([name, meta]) => ({
            name,
            label: meta.string || name,
            type:  meta.type   || 'char',
        }));
    }

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            if (this.props.recordId) {
                const cols = this.formFields.map(f => f.name);
                const rows = await RpcService.call(
                    this.props.action.res_model, 'read',
                    [[this.props.recordId]], { fields: cols });
                this.state.record = (Array.isArray(rows) ? rows[0] : rows) || {};
            } else {
                this.state.record = {};
            }
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    formatValue(val) {
        if (val === null || val === undefined || val === false) return '';
        if (Array.isArray(val)) return val[1] ?? '';
        return String(val);
    }

    onFieldChange(name, value) {
        this.state.record[name] = value;
    }

    async onSave() {
        try {
            await RpcService.call(
                this.props.action.res_model, 'write',
                [[this.state.record.id], this.state.record], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCreate() {
        try {
            await RpcService.call(
                this.props.action.res_model, 'create',
                [this.state.record], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onDelete() {
        try {
            await RpcService.call(
                this.props.action.res_model, 'unlink',
                [[this.state.record.id]], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// ActionView — orchestrates list ↔ form switching for one action
// ----------------------------------------------------------------
class ActionView extends Component {
    static template = xml`
        <div class="action-view">
            <t t-if="state.loading">
                <div class="loading">Loading views…</div>
            </t>
            <t t-elif="state.mode === 'list'">
                <ListView action="props.action"
                          viewDef="state.listView"
                          onOpenForm.bind="openForm"/>
            </t>
            <t t-elif="state.mode === 'form'">
                <FormView action="props.action"
                          viewDef="state.formView"
                          recordId="state.recordId"
                          onBack.bind="backToList"/>
            </t>
        </div>
    `;

    static components = { ListView, FormView };

    setup() {
        this.state = useState({
            loading:  true,
            mode:     'list',
            recordId: null,
            listView: null,
            formView: null,
        });
        onMounted(() => this.loadViews());
    }

    async loadViews() {
        this.state.loading = true;
        try {
            const result = await RpcService.getViews(
                this.props.action.res_model,
                [[false, 'list'], [false, 'form']]);
            this.state.listView = result.views?.list || null;
            this.state.formView = result.views?.form || null;
        } catch (e) {
            console.error('get_views failed:', e);
        } finally {
            this.state.loading = false;
        }
    }

    openForm(recordId) {
        this.state.recordId = recordId;
        this.state.mode     = 'form';
    }

    backToList() {
        this.state.recordId = null;
        this.state.mode     = 'list';
    }
}

// ----------------------------------------------------------------
// MainApp — IR-driven shell with sidebar from load_menus()
// ----------------------------------------------------------------
class MainApp extends Component {
    static template = xml`
        <div class="shell">
            <nav class="sidebar">
                <div class="sidebar-logo">odoo-cpp</div>
                <t t-foreach="state.menus" t-as="menu" t-key="menu.id">
                    <div t-attf-class="nav-item {{ state.activeMenuId === menu.id ? 'active' : '' }}"
                         t-on-click="() => this.activateMenu(menu)"
                         t-esc="menu.name"/>
                </t>
                <t t-if="state.loadingMenus">
                    <div class="nav-item">Loading…</div>
                </t>
            </nav>
            <div class="main">
                <div class="topbar">
                    <h1 t-esc="state.currentTitle || 'odoo-cpp'"/>
                    <UserMenu/>
                </div>
                <div class="content">
                    <t t-if="state.loadingAction">
                        <div class="loading">Loading…</div>
                    </t>
                    <t t-elif="state.action">
                        <ActionView action="state.action" t-key="state.action.id"/>
                    </t>
                    <t t-else="">
                        <div class="welcome">
                            <h2>Welcome to odoo-cpp</h2>
                            <p>Select a menu item to get started.</p>
                        </div>
                    </t>
                </div>
            </div>
        </div>
    `;

    static components = { ActionView, UserMenu };

    setup() {
        this.state = useState({
            menus:         [],
            loadingMenus:  true,
            activeMenuId:  null,
            currentTitle:  '',
            action:        null,
            loadingAction: false,
        });
        onMounted(() => this.loadMenus());
    }

    async loadMenus() {
        try {
            const data = await RpcService.loadMenus();
            // data is flat dict keyed by id string + "root"
            const root     = data.root || {};
            const rootIds  = root.children || [];
            this.state.menus = rootIds
                .map(id => data[String(id)])
                .filter(Boolean)
                .sort((a, b) => (a.sequence || 0) - (b.sequence || 0));
        } catch (e) {
            console.error('load_menus failed:', e);
        } finally {
            this.state.loadingMenus = false;
        }
    }

    async activateMenu(menu) {
        this.state.activeMenuId  = menu.id;
        this.state.currentTitle  = menu.name;
        this.state.action        = null;

        if (!menu.action_id) return;

        this.state.loadingAction = true;
        try {
            this.state.action = await RpcService.loadAction(menu.action_id);
        } catch (e) {
            console.error('loadAction failed:', e);
        } finally {
            this.state.loadingAction = false;
        }
    }
}

// ----------------------------------------------------------------
// Root App — authentication gate
// ----------------------------------------------------------------
class App extends Component {
    static template = xml`
        <div>
            <t t-if="state.loading">
                <div class="boot-screen">
                    <span class="spinner" style="width:32px;height:32px;border-width:3px"/>
                </div>
            </t>
            <t t-elif="state.authenticated">
                <MainApp/>
            </t>
            <t t-else="">
                <LoginPage/>
            </t>
        </div>
    `;

    static components = { MainApp, LoginPage };

    state = useState({ loading: true, authenticated: false });

    setup() {
        onMounted(async () => {
            const ok = await RpcService.restoreSession();
            this.state.authenticated = ok;
            this.state.loading       = false;
        });

        document.addEventListener('login-success', () => {
            this.state.authenticated = true;
        });
        document.addEventListener('logout', () => {
            this.state.authenticated = false;
        });
    }
}

mount(App, document.getElementById('app'));
