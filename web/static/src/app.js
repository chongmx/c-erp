/**
 * app.js — IR-driven OWL application with Odoo 14-style navigation.
 *
 * Navigation flow:
 *   Home screen → click app tile → app context (horizontal nav)
 *   Top nav section → click direct link → load action
 *   Top nav section (with children) → dropdown → click leaf → load action
 */
const { Component, useState, xml, mount, onMounted } = owl;

// App tile background colors (cycled by index)
const APP_COLORS = [
    '#875A7B', '#00A09D', '#E74C3C', '#1ABC9C',
    '#3498DB', '#F39C12', '#9B59B6', '#2ECC71',
];

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

    onRowClick(id) { this.props.onOpenForm(id); }
    onNew()        { this.props.onOpenForm(null); }
}

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
                <div class="form-body" t-on-change="onFormChange" t-on-input="onFormInput" t-on-click="onFormClick">
                    <t t-foreach="scalarFields" t-as="f" t-key="f.name">
                        <div class="form-row">
                            <label class="form-label" t-esc="f.label"/>
                            <t t-if="f.type === 'many2one'">
                                <select class="form-input" t-att-data-field="f.name">
                                    <option value="0">—</option>
                                    <t t-foreach="state.relOptions[f.name] || []" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="this.getM2oId(state.record[f.name]) === opt.id ? true : undefined">
                                            <t t-esc="opt.display"/>
                                        </option>
                                    </t>
                                </select>
                            </t>
                            <t t-else="">
                                <input class="form-input"
                                       t-att-type="f.type === 'boolean' ? 'checkbox' : 'text'"
                                       t-att-checked="f.type === 'boolean' ? !!state.record[f.name] : undefined"
                                       t-att-value="f.type !== 'boolean' ? formatValue(state.record[f.name]) : undefined"
                                       t-att-data-field="f.name"
                                       t-att-data-type="f.type"/>
                            </t>
                        </div>
                    </t>
                    <t t-foreach="o2mFields" t-as="f" t-key="f.name">
                        <div class="o2m-section">
                            <div class="o2m-title" t-esc="f.label"/>
                            <table class="o2m-table">
                                <thead>
                                    <tr>
                                        <t t-foreach="this.o2mColumns(f.name)" t-as="col" t-key="col.name">
                                            <th t-esc="col.label"/>
                                        </t>
                                        <th></th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <t t-foreach="state.o2mLines[f.name] || []" t-as="line" t-key="line._key">
                                        <tr>
                                            <t t-foreach="this.o2mColumns(f.name)" t-as="col" t-key="col.name">
                                                <td>
                                                    <t t-if="col.type === 'many2one'">
                                                        <select class="o2m-input"
                                                                t-att-data-o2m="f.name"
                                                                t-att-data-key="line._key"
                                                                t-att-data-field="col.name">
                                                            <option value="0">—</option>
                                                            <t t-foreach="state.relOptions[col.name] || []" t-as="opt" t-key="opt.id">
                                                                <option t-att-value="opt.id"
                                                                        t-att-selected="this.getM2oId(line[col.name]) === opt.id ? true : undefined">
                                                                    <t t-esc="opt.display"/>
                                                                </option>
                                                            </t>
                                                        </select>
                                                    </t>
                                                    <t t-elif="col.readonly">
                                                        <span t-esc="line[col.name] !== undefined ? String(line[col.name]) : ''"/>
                                                    </t>
                                                    <t t-else="">
                                                        <input t-att-type="col.type === 'float' || col.type === 'integer' || col.type === 'monetary' ? 'number' : 'text'"
                                                               t-att-step="col.type === 'float' || col.type === 'monetary' ? '0.01' : undefined"
                                                               class="o2m-input"
                                                               t-att-value="line[col.name] !== undefined ? line[col.name] : ''"
                                                               t-att-data-o2m="f.name"
                                                               t-att-data-key="line._key"
                                                               t-att-data-field="col.name"/>
                                                    </t>
                                                </td>
                                            </t>
                                            <td>
                                                <button class="btn btn-sm btn-danger"
                                                        t-att-data-del-o2m="f.name"
                                                        t-att-data-key="line._key">✕</button>
                                            </td>
                                        </tr>
                                    </t>
                                </tbody>
                            </table>
                            <button class="btn btn-sm" t-att-data-add-o2m="f.name">+ Add a line</button>
                        </div>
                    </t>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading: true, record: {}, isNew: !this.props.recordId, error: '',
            relOptions: {},
            o2mLines:   {},
            o2mMeta:    {},
            deletedIds: {},
        });
        this._nextKey = 1;
        onMounted(() => this.load());
    }

    get formFields() {
        const fields = (this.props.viewDef || {}).fields || {};
        return Object.entries(fields).map(([name, meta]) => ({
            name,
            label:         meta.string        || name,
            type:          meta.type          || 'char',
            relation:      meta.relation      || null,
            relationField: meta.relation_field || null,
            readonly:      !!meta.readonly,
        }));
    }

    get scalarFields() {
        return this.formFields.filter(f => f.type !== 'one2many' && f.type !== 'many2many');
    }

    get o2mFields() {
        return this.formFields.filter(f => f.type === 'one2many' && f.relation);
    }

    o2mColumns(fieldName) {
        const meta = this.state.o2mMeta[fieldName];
        if (!meta) return [];
        return Object.entries(meta).map(([name, m]) => ({
            name,
            label:    m.string   || name,
            type:     m.type     || 'char',
            relation: m.relation || null,
            readonly: !!m.readonly,
        }));
    }

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            if (this.props.recordId) {
                const cols = this.scalarFields.map(f => f.name);
                const rows = await RpcService.call(
                    this.props.action.res_model, 'read',
                    [[this.props.recordId]], { fields: cols });
                this.state.record = (Array.isArray(rows) ? rows[0] : rows) || {};
            } else {
                try {
                    const fieldNames = this.scalarFields.map(f => f.name);
                    const defaults = await RpcService.call(
                        this.props.action.res_model, 'default_get',
                        [fieldNames], {});
                    this.state.record = (defaults && typeof defaults === 'object') ? defaults : {};
                } catch (_) {
                    this.state.record = {};
                }
            }
            await this.loadRelOptions();
            await this.loadO2mData();
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async loadRelOptions() {
        const m2oFields = this.scalarFields.filter(f => f.type === 'many2one' && f.relation);
        await Promise.all(m2oFields.map(async f => {
            try {
                const recs = await RpcService.call(f.relation, 'search_read', [[]],
                    { fields: ['id', 'name'], limit: 200 });
                this.state.relOptions[f.name] = (Array.isArray(recs) ? recs : []).map(r => ({
                    id:      r.id,
                    display: r.name || String(r.id),
                }));
            } catch (_) {
                this.state.relOptions[f.name] = [];
            }
        }));
    }

    async loadO2mData() {
        const ALWAYS_SKIP = new Set(['id', 'create_date', 'write_date',
                                     'company_id', 'currency_id', 'sequence',
                                     'tax_ids', 'tax_ids_json']);
        await Promise.all(this.o2mFields.map(async f => {
            this.state.o2mLines[f.name]   = [];
            this.state.deletedIds[f.name] = [];

            // Load child model field metadata
            try {
                const meta = await RpcService.call(f.relation, 'fields_get', [], {});
                const skip = new Set([...ALWAYS_SKIP, f.relationField]);
                const filtered = {};
                for (const [k, v] of Object.entries(meta || {})) {
                    if (skip.has(k)) continue;
                    if (v.type === 'one2many' || v.type === 'many2many') continue;
                    filtered[k] = v;
                }
                this.state.o2mMeta[f.name] = filtered;

                // Load relOptions for Many2one fields within the sublist
                await Promise.all(Object.entries(filtered)
                    .filter(([, m]) => m.type === 'many2one' && m.relation)
                    .map(async ([colName, m]) => {
                        if (this.state.relOptions[colName]) return;
                        try {
                            const recs = await RpcService.call(m.relation, 'search_read', [[]],
                                { fields: ['id', 'name'], limit: 200 });
                            this.state.relOptions[colName] = (Array.isArray(recs) ? recs : []).map(r => ({
                                id:      r.id,
                                display: r.name || String(r.id),
                            }));
                        } catch (_) {
                            this.state.relOptions[colName] = [];
                        }
                    }));
            } catch (_) {
                this.state.o2mMeta[f.name] = {};
            }

            // Load existing lines
            if (this.props.recordId && f.relationField) {
                try {
                    const colNames = Object.keys(this.state.o2mMeta[f.name] || {});
                    const lines = await RpcService.call(f.relation, 'search_read',
                        [[[f.relationField, '=', this.props.recordId]]],
                        { fields: ['id', ...colNames], limit: 500 });
                    this.state.o2mLines[f.name] = (Array.isArray(lines) ? lines : []).map(line => ({
                        _key: String(this._nextKey++),
                        ...line,
                    }));
                } catch (_) {}
            }
        }));
    }

    getM2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        if (typeof val === 'string') return parseInt(val) || 0;
        return 0;
    }

    formatValue(val) {
        if (val === null || val === undefined || val === false) return '';
        if (Array.isArray(val)) return val[1] ?? '';
        return String(val);
    }

    onFieldChange(name, value) { this.state.record[name] = value; }

    onFormChange(e) {
        const o2mField = e.target.dataset.o2m;
        if (o2mField) {
            this.updateO2mLine(o2mField, e.target.dataset.key, e.target.dataset.field,
                e.target.tagName === 'SELECT' ? (parseInt(e.target.value) || 0) : e.target.value);
            return;
        }
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.tagName === 'SELECT') {
            this.onFieldChange(field, parseInt(e.target.value) || 0);
        } else if (e.target.type === 'checkbox') {
            this.onFieldChange(field, e.target.checked);
        }
    }

    onFormInput(e) {
        const o2mField = e.target.dataset.o2m;
        if (o2mField) {
            this.updateO2mLine(o2mField, e.target.dataset.key, e.target.dataset.field, e.target.value);
            return;
        }
        const field = e.target.dataset.field;
        if (field && e.target.type !== 'checkbox') {
            this.onFieldChange(field, e.target.value);
        }
    }

    onFormClick(e) {
        const addField = e.target.dataset.addO2m;
        if (addField) { e.preventDefault(); this.addO2mLine(addField); return; }
        const delField = e.target.dataset.delO2m;
        if (delField) { e.preventDefault(); this.removeO2mLine(delField, e.target.dataset.key); }
    }

    addO2mLine(fieldName) {
        if (!this.state.o2mLines[fieldName]) this.state.o2mLines[fieldName] = [];
        this.state.o2mLines[fieldName].push({ _key: String(this._nextKey++), id: null });
    }

    removeO2mLine(fieldName, key) {
        const lines = this.state.o2mLines[fieldName] || [];
        const line  = lines.find(l => l._key === key);
        if (line && line.id) {
            if (!this.state.deletedIds[fieldName]) this.state.deletedIds[fieldName] = [];
            this.state.deletedIds[fieldName].push(line.id);
        }
        this.state.o2mLines[fieldName] = lines.filter(l => l._key !== key);
    }

    updateO2mLine(fieldName, key, colName, value) {
        const line = (this.state.o2mLines[fieldName] || []).find(l => l._key === key);
        if (!line) return;
        line[colName] = value;
        if (colName === 'product_id' && value > 0) {
            this.applyProductDefaults(fieldName, key, value);
        }
    }

    async applyProductDefaults(fieldName, key, productId) {
        try {
            const rows = await RpcService.call('product.product', 'search_read',
                [[['id', '=', productId]]],
                { fields: ['id', 'name', 'list_price', 'standard_price', 'uom_id', 'uom_po_id'], limit: 1 });
            if (!rows || !rows.length) return;
            const prod = rows[0];
            const line = (this.state.o2mLines[fieldName] || []).find(l => l._key === key);
            if (!line) return;
            const rel = (this.o2mFields.find(f => f.name === fieldName) || {}).relation || '';
            const isPurchase = rel === 'purchase.order.line';
            line.name       = prod.name || '';
            line.price_unit = isPurchase ? (prod.standard_price || 0) : (prod.list_price || 0);
            // uom: purchase uses uom_po_id (fallback uom_id), sale uses uom_id
            const uomRaw = isPurchase ? (prod.uom_po_id || prod.uom_id) : prod.uom_id;
            line.product_uom_id = Array.isArray(uomRaw) ? uomRaw[0] : (uomRaw || 0);
            // default qty to 1 if not already set
            const qtyField = isPurchase ? 'product_qty' : 'product_uom_qty';
            if (!line[qtyField]) line[qtyField] = 1;
        } catch (_) {}
    }

    async syncO2mLines(parentId) {
        for (const f of this.o2mFields) {
            const deletedIds = this.state.deletedIds[f.name] || [];
            if (deletedIds.length)
                await RpcService.call(f.relation, 'unlink', [deletedIds], {});

            for (const line of (this.state.o2mLines[f.name] || [])) {
                const vals = { [f.relationField]: parentId };
                for (const colName of Object.keys(this.state.o2mMeta[f.name] || {})) {
                    const meta = this.state.o2mMeta[f.name][colName];
                    if (!meta.readonly && line[colName] !== undefined)
                        vals[colName] = line[colName];
                }
                if (!line.id) {
                    await RpcService.call(f.relation, 'create', [vals], {});
                } else {
                    await RpcService.call(f.relation, 'write', [[line.id], vals], {});
                }
            }
        }
    }

    async onSave() {
        try {
            await RpcService.call(
                this.props.action.res_model, 'write',
                [[this.state.record.id], this.state.record], {});
            await this.syncO2mLines(this.state.record.id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCreate() {
        try {
            const newId = await RpcService.call(
                this.props.action.res_model, 'create',
                [this.state.record], {});
            await this.syncO2mLines(newId);
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
// ActionView — orchestrates list ↔ form switching
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
// HomeScreen — grid of colored app tiles
// ----------------------------------------------------------------
class HomeScreen extends Component {
    static template = xml`
        <div class="home-screen">
            <div class="home-header">
                <span class="home-title">odoo-cpp</span>
                <UserMenu/>
            </div>
            <div class="app-grid">
                <t t-foreach="props.apps" t-as="app" t-key="app.id">
                    <div class="app-tile"
                         t-on-click="() => props.onSelectApp(app)"
                         t-att-style="'--tile-color:' + appColor(app_index)">
                        <div class="app-tile-icon">
                            <span t-esc="appIcon(app.web_icon)"/>
                        </div>
                        <div class="app-tile-name" t-esc="app.name"/>
                    </div>
                </t>
            </div>
        </div>
    `;

    static components = { UserMenu };

    appColor(index) { return APP_COLORS[index % APP_COLORS.length]; }

    appIcon(webIcon) {
        const icons = { accounting: '📒', contacts: '👥', settings: '⚙', sales: '💰', purchase: '🛒' };
        return icons[webIcon] || '◉';
    }
}

// ----------------------------------------------------------------
// AppTopNav — horizontal nav bar with dropdowns
// ----------------------------------------------------------------
class AppTopNav extends Component {
    static template = xml`
        <nav class="app-nav" t-on-click="onNavClick">
            <div class="app-nav-left">
                <span class="nav-home-btn" t-on-click.stop="props.onHome">⊞</span>
                <span class="nav-separator"/>
                <span class="nav-app-title" t-esc="props.appName"/>
            </div>
            <div class="app-nav-items">
                <t t-foreach="props.sections" t-as="sec" t-key="sec.id">
                    <div class="nav-section-wrap">
                        <div t-attf-class="nav-section-btn{{ isActive(sec) ? ' active' : '' }}"
                             t-on-click.stop="() => this.onSectionClick(sec)">
                            <span t-esc="sec.name"/>
                            <t t-if="sec.children.length > 0">
                                <span class="nav-caret">▾</span>
                            </t>
                        </div>
                        <t t-if="state.openSection === sec.id">
                            <div class="dropdown-menu" t-on-click.stop="">
                                <t t-foreach="sec.children" t-as="leaf" t-key="leaf.id">
                                    <div t-attf-class="dropdown-item{{ props.activeMenuId === leaf.id ? ' active' : '' }}"
                                         t-on-click="() => this.onLeafClick(leaf)"
                                         t-esc="leaf.name"/>
                                </t>
                            </div>
                        </t>
                    </div>
                </t>
            </div>
            <div class="app-nav-right">
                <UserMenu/>
            </div>
        </nav>
    `;

    static components = { UserMenu };

    setup() {
        this.state = useState({ openSection: null });
    }

    isActive(sec) {
        if (this.props.activeMenuId === sec.id) return true;
        return sec.children.some(c => c.id === this.props.activeMenuId);
    }

    onNavClick() {
        // Close dropdown when clicking outside a section-wrap
        this.state.openSection = null;
    }

    onSectionClick(sec) {
        if (sec.children.length > 0) {
            // Toggle dropdown
            this.state.openSection = this.state.openSection === sec.id ? null : sec.id;
        } else {
            // Direct link
            this.state.openSection = null;
            this.props.onSelectLeaf(sec);
        }
    }

    onLeafClick(leaf) {
        this.state.openSection = null;
        this.props.onSelectLeaf(leaf);
    }
}

// ----------------------------------------------------------------
// MainApp — orchestrates home screen ↔ app context
// ----------------------------------------------------------------
class MainApp extends Component {
    static template = xml`
        <div class="shell">
            <t t-if="state.mode === 'home'">
                <HomeScreen apps="state.apps" onSelectApp.bind="selectApp"/>
            </t>
            <t t-else="">
                <AppTopNav
                    appName="state.activeApp ? state.activeApp.name : ''"
                    sections="state.sections"
                    activeMenuId="state.activeMenuId"
                    onHome.bind="goHome"
                    onSelectLeaf.bind="activateLeaf"/>
                <div class="content">
                    <t t-if="state.loadingAction">
                        <div class="loading">Loading…</div>
                    </t>
                    <t t-elif="state.action">
                        <ActionView action="state.action" t-key="state.action.id"/>
                    </t>
                    <t t-else="">
                        <div class="welcome">
                            <h2 t-esc="state.activeApp ? state.activeApp.name : ''"/>
                            <p>Select a menu item to get started.</p>
                        </div>
                    </t>
                </div>
            </t>
        </div>
    `;

    static components = { HomeScreen, AppTopNav, ActionView };

    setup() {
        this.state = useState({
            mode:          'home',
            apps:          [],
            allMenus:      {},
            activeApp:     null,
            sections:      [],
            activeMenuId:  null,
            action:        null,
            loadingAction: false,
        });
        onMounted(() => this.loadMenus());
    }

    async loadMenus() {
        try {
            const data    = await RpcService.loadMenus();
            const rootIds = (data.root || {}).children || [];
            this.state.allMenus = data;
            this.state.apps = rootIds
                .map(id => data[String(id)])
                .filter(Boolean)
                .sort((a, b) => (a.sequence || 0) - (b.sequence || 0));
        } catch (e) {
            console.error('load_menus failed:', e);
        }
    }

    selectApp(app) {
        const data = this.state.allMenus;

        // Build sections: direct children of app, with their own children attached
        const sections = (app.children || [])
            .map(id => {
                const sec = data[String(id)];
                if (!sec) return null;
                const children = (sec.children || [])
                    .map(cid => data[String(cid)])
                    .filter(Boolean)
                    .sort((a, b) => (a.sequence || 0) - (b.sequence || 0));
                return { ...sec, children };
            })
            .filter(Boolean)
            .sort((a, b) => (a.sequence || 0) - (b.sequence || 0));

        this.state.activeApp    = app;
        this.state.sections     = sections;
        this.state.mode         = 'app';
        this.state.activeMenuId = null;
        this.state.action       = null;
    }

    goHome() {
        this.state.mode      = 'home';
        this.state.activeApp = null;
        this.state.action    = null;
    }

    async activateLeaf(leaf) {
        this.state.activeMenuId = leaf.id;
        if (!leaf.action_id) return;

        this.state.loadingAction = true;
        try {
            this.state.action = await RpcService.loadAction(leaf.action_id);
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
