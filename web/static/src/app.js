/**
 * app.js — IR-driven OWL application with Odoo 14-style navigation.
 *
 * Navigation flow:
 *   Home screen → click app tile → app context (horizontal nav)
 *   Top nav section → click direct link → load action
 *   Top nav section (with children) → dropdown → click leaf → load action
 */
const { Component, useState, xml, mount, onMounted, useRef, onWillUnmount, onWillUpdateProps } = owl;

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
                <button class="btn" t-on-click="onExport">Export CSV</button>
                <button class="btn" t-on-click="onImportClick">Import CSV</button>
                <span t-if="state.importMsg"
                      t-att-class="'import-msg ' + (state.importOk ? 'import-ok' : 'import-err')"
                      t-esc="state.importMsg"/>
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
        this.state = useState({
            loading: true, records: [], error: '',
            importMsg: '', importOk: false,
        });
        onMounted(() => this.load());
        onWillUpdateProps((np) => {
            const oa = this.props.action;
            const na = np.action;
            if (na?.res_model !== oa?.res_model ||
                JSON.stringify(na?.domain) !== JSON.stringify(oa?.domain)) {
                this.load(na);
            }
        });
    }

    get columns() {
        const viewDef = this.props.viewDef || {};
        const fields  = viewDef.fields || {};
        return Object.entries(fields).map(([name, meta]) => ({
            name,
            label: meta.string || name,
        }));
    }

    async load(action) {
        action = action || this.props.action;
        this.state.loading = true;
        this.state.error   = '';
        try {
            const cols   = this.columns.map(c => c.name);
            let domain   = action.domain || [];
            if (typeof domain === 'string') {
                try { domain = JSON.parse(domain); } catch(_) { domain = []; }
            }
            const recs   = await RpcService.call(
                action.res_model, 'search_read',
                [domain], { fields: cols, limit: 80 });
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

    onExport() {
        const model  = this.props.action?.res_model;
        if (!model) return;
        const fields = this.columns.map(c => c.name).join(',');
        window.open(`/web/export/${encodeURIComponent(model)}?fields=${encodeURIComponent(fields)}`, '_blank');
    }

    onImportClick() {
        const input = document.createElement('input');
        input.type  = 'file';
        input.accept = '.csv,text/csv';
        input.addEventListener('change', (ev) => this.onImportFile(ev));
        input.click();
    }

    async onImportFile(ev) {
        const file = ev.target?.files?.[0];
        if (!file) return;
        const model = this.props.action?.res_model;
        if (!model) return;
        this.state.importMsg = 'Importing…';
        this.state.importOk  = true;
        try {
            const fd = new FormData();
            fd.append('file', file);
            const res  = await fetch(`/web/import/${encodeURIComponent(model)}`,
                { method: 'POST', credentials: 'include', body: fd });
            const data = await res.json();
            if (!res.ok) {
                this.state.importMsg = data.error || 'Import failed';
                this.state.importOk  = false;
            } else {
                const errCount = (data.errors || []).length;
                this.state.importMsg =
                    `Imported ${data.imported} record(s)` +
                    (errCount ? ` (${errCount} error(s))` : '');
                this.state.importOk = errCount === 0;
                await this.load();
            }
        } catch (_) {
            this.state.importMsg = 'Import failed';
            this.state.importOk  = false;
        }
        // Auto-clear message after 8 s
        setTimeout(() => { this.state.importMsg = ''; }, 8000);
    }
}

// FormView — renders a form from get_views + read
// ----------------------------------------------------------------
class FormView extends Component {
    static template = xml`
        <div class="gf-shell">
            <div class="gf-header">
                <div class="gf-breadcrumb">
                    <span class="gf-bc-link" t-on-click="onBack" t-esc="listTitle"/>
                    <span class="gf-bc-sep">›</span>
                    <span class="gf-bc-cur" t-esc="recordTitle"/>
                </div>
                <div class="gf-actions">
                    <button t-if="state.isNew"  class="btn btn-primary" t-on-click="onCreate">Create</button>
                    <button t-if="!state.isNew" class="btn btn-primary" t-on-click="onSave">Save</button>
                    <button t-if="!state.isNew" class="btn btn-danger" t-on-click="onDelete">Delete</button>
                    <button class="btn" t-on-click="onBack">Discard</button>
                </div>
            </div>

            <t t-if="state.loading">
                <div class="loading">Loading…</div>
            </t>
            <t t-elif="state.conflictError">
                <div class="alert alert-warning">
                    <strong>Conflict:</strong> <t t-esc="state.conflictError"/>
                    <button class="btn btn-sm" style="margin-left:8px" t-on-click="onReload">Reload</button>
                </div>
            </t>
            <t t-elif="state.error">
                <div class="error" t-esc="state.error"/>
            </t>
            <t t-else="">
                <div class="gf-card" t-on-change="onFormChange" t-on-input="onFormInput" t-on-click="onFormClick">
                    <h1 class="gf-title" t-esc="recordTitle"/>
                    <div class="gf-grid">
                        <t t-foreach="scalarFields" t-as="f" t-key="f.name">
                            <div t-attf-class="gf-field{{ f.type === 'text' ? ' gf-field-wide' : '' }}{{ f.type === 'boolean' ? ' gf-field-bool' : '' }}">
                                <label class="gf-label" t-esc="f.label"/>
                                <div class="gf-control">
                                    <t t-if="f.type === 'many2one'">
                                        <div class="gf-m2o">
                                            <select class="form-input" t-att-data-field="f.name">
                                                <option value="0">—</option>
                                                <t t-foreach="state.relOptions[f.name] || []" t-as="opt" t-key="opt.id">
                                                    <option t-att-value="opt.id"
                                                            t-att-selected="this.getM2oId(state.record[f.name]) === opt.id ? true : undefined">
                                                        <t t-esc="opt.display"/>
                                                    </option>
                                                </t>
                                            </select>
                                            <button class="gf-addnew" t-att-data-addnew="f.name" title="Add new…">＋</button>
                                        </div>
                                    </t>
                                    <t t-elif="f.type === 'boolean'">
                                        <input type="checkbox" class="gf-check"
                                               t-att-checked="!!state.record[f.name]"
                                               t-att-data-field="f.name" t-att-data-type="'boolean'"/>
                                    </t>
                                    <t t-elif="f.type === 'text'">
                                        <textarea class="form-input gf-textarea"
                                                  t-att-data-field="f.name" t-att-data-type="'text'"><t t-esc="formatValue(state.record[f.name])"/></textarea>
                                    </t>
                                    <t t-else="">
                                        <input class="form-input"
                                               t-att-type="f.type === 'date' || f.type === 'datetime' ? 'date' : (f.type === 'integer' || f.type === 'float' || f.type === 'monetary' ? 'number' : 'text')"
                                               t-att-step="f.type === 'float' || f.type === 'monetary' ? '0.01' : undefined"
                                               t-att-value="formatValue(state.record[f.name])"
                                               t-att-readonly="f.readonly ? true : undefined"
                                               t-att-data-field="f.name"
                                               t-att-data-type="f.type"/>
                                    </t>
                                </div>
                            </div>
                        </t>
                    </div>

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
                                                        <div class="gf-m2o">
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
                                                            <button class="gf-addnew"
                                                                    t-att-data-addnew-o2m="f.name"
                                                                    t-att-data-addnew-field="col.name"
                                                                    t-att-data-key="line._key" title="Add new…">＋</button>
                                                        </div>
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
                            <button class="btn so-add-line" t-att-data-add-o2m="f.name">+ Add a line</button>
                        </div>
                    </t>
                </div>
            </t>

            <!-- Add-new related record modal (combobox "Add new…") -->
            <t t-if="state.addNew">
                <div class="gf-modal" t-on-click="onModalBackdrop">
                    <div class="gf-modal-card">
                        <h3 class="gf-modal-title">New <t t-esc="state.addNew.label"/></h3>
                        <label class="gf-modal-lbl">Name</label>
                        <input class="gf-modal-input" t-att-value="state.addNew.name"
                               t-on-input="onAddNewNameInput" t-on-keydown="onAddNewKey"/>
                        <div t-if="state.addNew.error" class="gf-modal-err" t-esc="state.addNew.error"/>
                        <div class="gf-modal-actions">
                            <button class="btn" t-on-click="closeAddNew">Cancel</button>
                            <button class="btn btn-primary" t-on-click="submitAddNew">Create</button>
                        </div>
                    </div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading: true, record: {}, isNew: !this.props.recordId, error: '',
            conflictError: '',
            relOptions: {},
            o2mLines:   {},
            o2mMeta:    {},
            deletedIds: {},
            addNew:     null,   // { field, relation, label, name, error, o2mField?, targetKey? }
        });
        this._nextKey = 1;
        onMounted(() => this.load());
    }

    // Title for the header breadcrumb (list name) and the current record.
    get listTitle() {
        const a = this.props.action || {};
        return a.name || a.res_model || 'Records';
    }
    get recordTitle() {
        if (this.state.isNew) return 'New';
        const r = this.state.record || {};
        return this.formatValue(r.name) || this.formatValue(r.display_name) ||
               this.formatValue(r.code) || this.formatValue(r.reference) ||
               ('#' + (r.id || ''));
    }

    // ---- combobox "Add new…" — create a related record inline ----
    openAddNew(ctx) { this.state.addNew = { name: '', error: '', ...ctx }; }
    onAddNewNameInput(e) { if (this.state.addNew) this.state.addNew.name = e.target.value; }
    onAddNewKey(e) { if (e.key === 'Enter') { e.preventDefault(); this.submitAddNew(); } }
    closeAddNew() { this.state.addNew = null; }
    onModalBackdrop(e) { if (e.target === e.currentTarget) this.state.addNew = null; }

    async submitAddNew() {
        const ctx = this.state.addNew;
        if (!ctx) return;
        const name = (ctx.name || '').trim();
        if (!name) { ctx.error = 'Please enter a name.'; return; }
        ctx.error = '';
        try {
            const id = await RpcService.call(ctx.relation, 'create', [{ name }], {});
            // Make the new record selectable and select it immediately.
            const opts = this.state.relOptions[ctx.field] ? [...this.state.relOptions[ctx.field]] : [];
            opts.push({ id, display: name });
            this.state.relOptions[ctx.field] = opts;
            if (ctx.o2mField) {
                const line = (this.state.o2mLines[ctx.o2mField] || []).find(l => l._key === ctx.targetKey);
                if (line) line[ctx.field] = id;
            } else {
                this.state.record[ctx.field] = id;
            }
            this.state.addNew = null;
        } catch (e) {
            // Some models need more than a name; say so rather than failing silently.
            ctx.error = (e && e.message) || 'Could not create this record here — open its own menu to add all details.';
        }
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
        // Hide the plumbing columns — they are never user-editable and only
        // clutter the form (the id is still kept on the record for write()).
        const HIDE = new Set(['id', 'create_date', 'write_date', '__last_update', 'display_name']);
        return this.formFields.filter(f =>
            f.type !== 'one2many' && f.type !== 'many2many' && !HIDE.has(f.name));
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
        if (delField) { e.preventDefault(); this.removeO2mLine(delField, e.target.dataset.key); return; }
        // "Add new…" beside a scalar many2one combobox
        const newField = e.target.dataset.addnew;
        if (newField) {
            e.preventDefault();
            const f = this.formFields.find(x => x.name === newField);
            if (f && f.relation) this.openAddNew({ field: f.name, relation: f.relation, label: f.label });
            return;
        }
        // "Add new…" beside a many2one inside an o2m line
        const newO2m = e.target.dataset.addnewO2m;
        if (newO2m) {
            e.preventDefault();
            const colName = e.target.dataset.addnewField;
            const meta = (this.state.o2mMeta[newO2m] || {})[colName];
            if (meta && meta.relation)
                this.openAddNew({ field: colName, relation: meta.relation,
                                  label: meta.string || colName,
                                  o2mField: newO2m, targetKey: e.target.dataset.key });
        }
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
            this.triggerLineOnchange(fieldName, key, colName);
        }
    }

    // Trigger server-side onchange for an o2m line field change.
    // Merges the returned { value: {...} } into the line.
    async triggerLineOnchange(fieldName, key, changedField) {
        const f = this.o2mFields.find(f => f.name === fieldName);
        if (!f) return;
        const line = (this.state.o2mLines[fieldName] || []).find(l => l._key === key);
        if (!line) return;
        try {
            const res = await RpcService.call(
                f.relation, 'onchange', [line, changedField, {}], {});
            if (res && res.value) {
                // Unwrap many2one tuples [id, name] → keep as-is for display;
                // store id for fields named product_uom_id etc.
                Object.assign(line, res.value);
            }
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
        this.state.conflictError = '';
        try {
            const vals = Object.assign({}, this.state.record);
            // OCC: send current write_date so server can detect concurrent saves
            if (this.state.record.write_date)
                vals.__expected_write_date = this.state.record.write_date;
            await RpcService.call(
                this.props.action.res_model, 'write',
                [[this.state.record.id], vals], {});
            await this.syncO2mLines(this.state.record.id);
            this.props.onBack();
        } catch (e) {
            if (e.type === 'odoo.exceptions.ConcurrencyConflict')
                this.state.conflictError = e.message;
            else
                this.state.error = e.message;
        }
    }

    async onReload() {
        this.state.conflictError = '';
        await this.load();
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
// ChatterPanel — shared audit-log / message feed component
//   Props:
//     model      {string}  — res.model string (e.g. 'sale.order')
//     recordId   {number}  — record id (0 / null → don't load)
//     refreshKey {number?} — change this to force a reload
// ----------------------------------------------------------------
class ChatterPanel extends Component {
    static props = ['model', 'recordId', 'refreshKey?'];
    static template = xml`
        <div class="chatter-panel">
            <div class="chatter-head"><span>Log</span></div>
            <t t-if="props.recordId">
                <t t-if="state.loading">
                    <div class="chatter-loading">Loading…</div>
                </t>
                <t t-else="">
                    <div class="chatter-feed">
                        <t t-foreach="state.messages" t-as="msg" t-key="msg.id">
                            <div class="chatter-entry">
                                <div class="chatter-avatar" t-esc="avatarChar(msg.author_name)"/>
                                <div class="chatter-body">
                                    <span class="chatter-author" t-esc="msg.author_name"/>
                                    <span class="chatter-date" t-esc="msg.date"/>
                                    <div class="chatter-msg" t-esc="msg.body"/>
                                </div>
                            </div>
                        </t>
                        <t t-if="state.messages.length === 0">
                            <div class="chatter-empty">No log entries yet.</div>
                        </t>
                    </div>
                    <div class="chatter-compose">
                        <textarea class="chatter-input"
                                  placeholder="Log a note…"
                                  t-att-value="state.noteText"
                                  t-on-input="onNoteInput"/>
                        <button class="btn btn-sm" t-on-click="onPost"
                                t-att-disabled="state.posting ? true : undefined">
                            <t t-if="state.posting">Posting…</t>
                            <t t-else="">Post</t>
                        </button>
                    </div>
                </t>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ messages: [], loading: false, noteText: '', posting: false });
        const { onMounted, onWillUpdateProps } = owl;
        onMounted(() => { if (this.props.recordId) this.load(this.props.recordId); });
        onWillUpdateProps(np => {
            if (np.recordId !== this.props.recordId ||
                np.refreshKey !== this.props.refreshKey) {
                if (np.recordId) this.load(np.recordId);
            }
        });
    }

    async load(recordId) {
        this.state.loading = true;
        try {
            const msgs = await RpcService.call('mail.message', 'search_read',
                [[['res_model', '=', this.props.model], ['res_id', '=', recordId]]],
                { fields: ['body', 'author_name', 'subtype', 'date'], limit: 50 });
            this.state.messages = Array.isArray(msgs) ? msgs : [];
        } catch (e) { console.error('ChatterPanel load:', e); }
        this.state.loading = false;
    }

    onNoteInput(e) { this.state.noteText = e.target.value; }

    async onPost() {
        const body = this.state.noteText.trim();
        if (!body || !this.props.recordId) return;
        this.state.posting = true;
        try {
            const uid = RpcService.getSession().uid || 0;
            await RpcService.call('mail.message', 'create',
                [{ res_model: this.props.model, res_id: this.props.recordId,
                   author_id: uid, body, subtype: 'note' }], {});
            this.state.noteText = '';
            await this.load(this.props.recordId);
        } catch (e) { console.error('ChatterPanel post:', e); }
        this.state.posting = false;
    }

    avatarChar(name) { return name ? name[0].toUpperCase() : 'S'; }
}

// ----------------------------------------------------------------
// AttachmentPanel — files on a record (docs/091)
//
//   Props:
//     model     {string} — e.g. 'hr.expense.sheet'
//     recordId  {number} — 0 / null → the panel says to save first
//     title     {string?} — panel heading, default 'Attachments'
//     readonly  {boolean?} — hide upload and delete
//
// ir.attachment, its content-addressed filestore and both HTTP routes were
// already built and tested; what was missing was any way for a user to reach
// them. Deliberately generic: adding files to another form is one tag.
//
// The bytes go over the dedicated multipart route, never through JSON-RPC —
// a base64 blob in a search_read would defeat storing the file out of the row.
// ----------------------------------------------------------------
class AttachmentPanel extends Component {
    static props = ['model', 'recordId', 'title?', 'readonly?', 'refreshKey?'];
    static template = xml`
        <div class="att-panel">
            <div class="chatter-head">
                <span t-esc="props.title || 'Attachments'"/>
                <span t-if="state.files.length" class="att-count" t-esc="state.files.length"/>
            </div>

            <t t-if="!props.recordId">
                <div class="chatter-empty">Save this record first, then attach files to it.</div>
            </t>
            <t t-else="">
                <t t-if="state.loading">
                    <div class="chatter-loading">Loading…</div>
                </t>
                <t t-else="">
                    <!-- Grouped, not flat. A PCB fabrication package is a dozen
                         files called top.gtl / bot.gbl / outline.gm1, and a flat
                         list of those is a directory listing, not a UI. -->
                    <div class="att-list">
                        <t t-foreach="groups" t-as="g" t-key="g.key">
                            <div class="att-group">
                                <div class="att-group-h">
                                    <span t-esc="g.label"/>
                                    <span class="att-count" t-esc="g.files.length"/>
                                </div>
                                <t t-foreach="g.files" t-as="f" t-key="f.id">
                                    <div class="att-row">
                                        <span class="att-icon" t-esc="iconFor(f.mimetype)"/>
                                        <a class="att-name" t-att-href="f.url" target="_blank"
                                           t-att-title="f.name" t-esc="f.name"/>
                                        <span class="att-kind" t-if="f.document_type"
                                              t-esc="f.document_type"/>
                                        <span class="att-meta" t-esc="f.size_human"/>
                                        <span class="att-meta att-when" t-esc="f.created"/>
                                        <a class="att-dl" t-att-href="f.url + '?download=1'"
                                           title="Download">&#8615;</a>
                                        <span t-if="!props.readonly" class="att-del" title="Remove"
                                              t-on-click.stop="() => this.onDelete(f)">&#x2715;</span>
                                    </div>
                                </t>
                            </div>
                        </t>
                        <t t-if="!state.files.length">
                            <div class="chatter-empty">No files attached yet.</div>
                        </t>
                    </div>

                    <!-- Answers the question a fab package actually raises: is it
                         complete? Only shown once there is fabrication data, so it
                         never nags a record that has none. -->
                    <div class="att-fab" t-if="fabStatus">
                        <span class="att-fab-l">Fabrication package</span>
                        <t t-foreach="fabStatus" t-as="s" t-key="s.key">
                            <span t-attf-class="att-fab-i {{ s.present ? 'yes' : 'no' }}">
                                <t t-esc="s.present ? '✓' : '✕'"/> <t t-esc="s.label"/>
                            </span>
                        </t>
                    </div>

                    <t t-if="!props.readonly">
                        <div t-attf-class="att-drop{{ state.dragging ? ' over' : '' }}"
                             t-on-dragover.prevent="onDragOver"
                             t-on-dragleave="onDragLeave"
                             t-on-drop.prevent="onDrop"
                             t-on-click="onPick">
                            <t t-if="state.uploading">Uploading <t t-esc="state.uploadName"/>…</t>
                            <t t-else="">Drop a file here, or click to choose one</t>
                        </div>
                        <input type="file" class="att-file-input" t-ref="fileInput"
                               t-on-change="onFileChosen"/>
                        <div t-if="state.error" class="att-error" t-esc="state.error"/>
                        <div class="att-hint">
                            Documents (PDF, images, CSV, TXT, XLSX, DOCX, ZIP) and manufacturing
                            data — Gerber, drill, pick-and-place, STEP/STL/DXF, KiCad. Up to 25 MB.
                            The kind is detected from the file name.
                        </div>
                    </t>
                </t>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ files: [], loading: false, uploading: false,
                                uploadName: '', error: '', dragging: false });
        this.fileInputRef = useRef('fileInput');
        const { onMounted, onWillUpdateProps } = owl;
        onMounted(() => { if (this.props.recordId) this.load(this.props.recordId); });
        onWillUpdateProps(np => {
            if (np.recordId !== this.props.recordId ||
                np.refreshKey !== this.props.refreshKey) {
                if (np.recordId) this.load(np.recordId); else this.state.files = [];
            }
        });
    }

    async load(recordId) {
        this.state.loading = true;
        try {
            const files = await RpcService.call('ir.attachment', 'search_read',
                [[['res_model', '=', this.props.model], ['res_id', '=', recordId]]],
                { limit: 100 });
            this.state.files = Array.isArray(files) ? files : [];
        } catch (e) {
            this.state.error = e.message || 'Could not load the attachments';
        }
        this.state.loading = false;
    }

    // Sections, in the order someone actually works through a build: make the
    // board, then the enclosure, then the paperwork. Types not listed fall into
    // Documents rather than being dropped.
    static SECTIONS = [
        { key: 'fabrication', label: 'Fabrication', types: ['gerber', 'drill', 'placement'] },
        { key: 'design',      label: 'Design source', types: ['pcb-design', 'schematic', 'netlist'] },
        { key: 'mechanical',  label: 'Mechanical', types: ['3d-model', 'drawing'] },
        { key: 'documents',   label: 'Documents', types: null },   // null = everything else
    ];

    get groups() {
        const out = [];
        const claimed = new Set();
        for (const s of AttachmentPanel.SECTIONS) {
            if (!s.types) continue;
            const files = this.state.files.filter(f => s.types.includes(f.document_type));
            files.forEach(f => claimed.add(f.id));
            if (files.length) out.push({ key: s.key, label: s.label, files });
        }
        const rest = this.state.files.filter(f => !claimed.has(f.id));
        if (rest.length) out.push({ key: 'documents', label: 'Documents', files: rest });
        return out;
    }

    // Null unless there is fabrication data, so a product with a datasheet and
    // nothing else is not told it has an incomplete PCB package.
    get fabStatus() {
        const have = new Set(this.state.files.map(f => f.document_type));
        if (!have.has('gerber') && !have.has('drill') && !have.has('placement')) return null;
        return [
            { key: 'gerber',    label: 'Gerber',    present: have.has('gerber') },
            { key: 'drill',     label: 'Drill',     present: have.has('drill') },
            { key: 'placement', label: 'Placement', present: have.has('placement') },
        ];
    }

    iconFor(mime) {
        const m = mime || '';
        if (m.startsWith('image/'))            return '\u{1F5BC}';
        if (m === 'application/pdf')           return '\u{1F4C4}';
        if (m === 'application/zip')           return '\u{1F5DC}';
        if (m.startsWith('text/'))             return '\u{1F4DD}';
        return '\u{1F4CE}';
    }

    onPick()     { if (this.fileInputRef.el) this.fileInputRef.el.click(); }
    onDragOver() { this.state.dragging = true; }
    onDragLeave(){ this.state.dragging = false; }

    onDrop(ev) {
        this.state.dragging = false;
        const f = ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files[0];
        if (f) this.upload(f);
    }
    onFileChosen(ev) {
        const f = ev.target.files && ev.target.files[0];
        if (f) this.upload(f);
        // Reset, or choosing the same file twice fires no change event.
        ev.target.value = '';
    }

    async upload(file) {
        if (!this.props.recordId) return;
        this.state.uploading = true;
        this.state.uploadName = file.name;
        this.state.error = '';
        try {
            const form = new FormData();
            form.append('file', file);
            form.append('res_model', this.props.model);
            form.append('res_id', String(this.props.recordId));
            form.append('name', file.name);
            // The upload route is multipart and session-cookie authenticated,
            // so it does not go through RpcService.
            const resp = await fetch('/web/attachment/upload', {
                method: 'POST', body: form, credentials: 'same-origin',
            });
            const out = await resp.json().catch(() => ({}));
            if (!resp.ok) throw new Error(out.error || `Upload failed (${resp.status})`);
            await this.load(this.props.recordId);
        } catch (e) {
            this.state.error = e.message || 'Upload failed';
        } finally {
            this.state.uploading = false;
            this.state.uploadName = '';
        }
    }

    async onDelete(f) {
        if (!confirm(`Remove "${f.name}"?`)) return;
        try {
            await RpcService.call('ir.attachment', 'unlink', [[f.id]], {});
            await this.load(this.props.recordId);
        } catch (e) {
            this.state.error = e.message || 'Could not remove the file';
        }
    }
}

// ----------------------------------------------------------------
// AuditLogPanel — shows audit trail for the current record
//   Props:
//     model    {string} — e.g. 'sale.order'
//     recordId {number} — 0 / null → hide panel
// ----------------------------------------------------------------
class AuditLogPanel extends Component {
    static props = ['model', 'recordId'];
    static template = xml`
        <div class="audit-panel" t-if="props.recordId">
            <div class="chatter-head"><span>Audit Trail</span></div>
            <t t-if="state.loading">
                <div class="chatter-loading">Loading…</div>
            </t>
            <t t-else="">
                <div class="chatter-feed">
                    <t t-foreach="state.entries" t-as="e" t-key="e.id">
                        <div class="chatter-entry">
                            <div class="chatter-avatar" t-esc="opIcon(e.operation)"/>
                            <div class="chatter-body">
                                <span class="chatter-author" t-esc="e.operation"/>
                                <span class="chatter-date" t-esc="e.created_at || ''"/>
                                <div class="chatter-msg">uid=<t t-esc="e.uid"/> ids=<t t-esc="String(e.record_ids)"/></div>
                            </div>
                        </div>
                    </t>
                    <t t-if="state.entries.length === 0">
                        <div class="chatter-empty">No audit entries yet.</div>
                    </t>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ entries: [], loading: false });
        onMounted(() => { if (this.props.recordId) this.load(); });
        onWillUpdateProps(np => {
            if (np.recordId !== this.props.recordId && np.recordId) this.load(np);
        });
    }

    async load(props) {
        const p = props || this.props;
        if (!p.recordId) return;
        this.state.loading = true;
        try {
            const rows = await RpcService.call('audit.log', 'search_read',
                [[['model', '=', p.model]]],
                { fields: ['id', 'operation', 'record_ids', 'uid', 'created_at'], limit: 50 });
            // Filter client-side to entries that contain this record's id
            this.state.entries = (Array.isArray(rows) ? rows : []).filter(r => {
                const ids = Array.isArray(r.record_ids) ? r.record_ids : [];
                return ids.includes(p.recordId);
            });
        } catch (_) { this.state.entries = []; }
        this.state.loading = false;
    }

    opIcon(op) {
        if (op === 'create') return '+';
        if (op === 'write')  return '✎';
        if (op === 'unlink') return '✕';
        return '?';
    }
}

// ----------------------------------------------------------------
// InvoiceFormView — Odoo 14-style Invoice (account.move) form
// ----------------------------------------------------------------
class InvoiceFormView extends Component {
    static components = { DatePicker, ChatterPanel, AttachmentPanel };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput"
             t-on-click="onAnyClick">

            <!-- Register Payment dialog -->
            <t t-if="state.payDialogOpen">
                <div class="pay-overlay" t-on-click.stop="onClosePayDialog">
                    <div class="pay-dialog" t-on-click.stop="">
                        <div class="pay-dialog-title">Register Payment</div>
                        <div class="so-field-row" style="margin-bottom:4px;">
                            <label class="so-field-lbl">Journal</label>
                            <select class="form-input"
                                    t-att-value="state.payJournalId"
                                    t-on-change="ev => state.payJournalId = parseInt(ev.target.value) || null">
                                <option value="">— select —</option>
                                <t t-foreach="state.payJournals" t-as="j" t-key="j.id">
                                    <option t-att-value="j.id"
                                            t-att-selected="state.payJournalId === j.id ? true : undefined"
                                            t-esc="j.name"/>
                                </t>
                            </select>
                        </div>
                        <div class="so-field-row" style="margin-bottom:4px;">
                            <label class="so-field-lbl">Amount</label>
                            <input class="form-input" type="number" step="0.01" min="0.01"
                                   t-att-value="state.payAmount"
                                   t-on-input="ev => state.payAmount = ev.target.value"/>
                        </div>
                        <!-- P1/FX: only for a foreign-currency invoice.
                             The bank converts on receipt, so we ask for the
                             base-currency amount that actually landed rather
                             than a rate — the bank's spread makes any quoted
                             rate wrong, and the statement figure is the one
                             the user can actually verify. -->
                        <t t-if="state.payIsForeign">
                            <div class="so-field-row" style="margin-bottom:4px;">
                                <label class="so-field-lbl"
                                       t-esc="'Received (' + state.payBaseCode + ')'"/>
                                <input class="form-input" type="number" step="0.01" min="0"
                                       t-att-value="state.payReceivedBase"
                                       t-att-placeholder="state.payExpectedBase"
                                       t-on-input="ev => state.payReceivedBase = ev.target.value"/>
                            </div>
                            <div class="so-field-row" style="margin-bottom:4px;">
                                <label class="so-field-lbl">Effective rate</label>
                                <span class="so-field-val" t-esc="effectiveRateText"/>
                            </div>
                            <div class="so-field-row" style="margin-bottom:8px;">
                                <label class="so-field-lbl">FX difference</label>
                                <span class="so-field-val" t-esc="fxDiffText"/>
                            </div>
                        </t>
                        <div class="so-field-row" style="margin-bottom:4px;">
                            <label class="so-field-lbl">Payment Date</label>
                            <DatePicker value="state.payDate" onSelect.bind="setPayDate"/>
                        </div>
                        <div class="so-field-row" style="margin-bottom:4px;">
                            <label class="so-field-lbl">Memo</label>
                            <input class="form-input" type="text"
                                   t-att-value="state.payMemo"
                                   t-on-input="ev => state.payMemo = ev.target.value"/>
                        </div>
                        <t t-if="state.payError">
                            <div class="pay-dialog-error" t-esc="state.payError"/>
                        </t>
                        <div class="pay-dialog-actions">
                            <button class="btn btn-primary" t-on-click.stop="onConfirmPayment">Validate</button>
                            <button class="btn"             t-on-click.stop="onClosePayDialog">Cancel</button>
                        </div>
                    </div>
                </div>
            </t>

            <!-- Page header -->
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack" t-esc="backLabel"/>
                        <span class="so-bc-sep">›</span>
                        <t t-if="props.navTotal and props.navTotal > 1">
                            <span class="so-bc-link" t-on-click.stop="onBack">Invoices</span>
                            <span class="so-bc-sep">›</span>
                        </t>
                        <span class="so-bc-cur" t-esc="state.record.name || ('New ' + docLabel)"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="props.navTotal and props.navTotal > 1">
                            <span style="font-size:.8rem;color:var(--muted);margin-right:4px;"
                                  t-esc="(props.navIdx + 1) + ' / ' + props.navTotal"/>
                            <button class="btn" t-att-disabled="props.navIdx === 0 ? true : undefined"
                                    t-on-click.stop="props.onPrev">&#8592;</button>
                            <button class="btn" t-att-disabled="props.navIdx === props.navTotal - 1 ? true : undefined"
                                    t-on-click.stop="props.onNext">&#8594;</button>
                        </t>
                        <t t-if="isDraft">
                            <button class="btn btn-primary" t-on-click.stop="onPost">Confirm</button>
                            <button class="btn"             t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                        </t>
                        <t t-if="canRegisterPayment">
                            <button class="btn btn-success" t-on-click.stop="onOpenPayDialog">Register Payment</button>
                        </t>
                        <t t-if="isPosted">
                            <button class="btn"           t-on-click.stop="onSave">Save</button>
                            <button t-if="canReverse" class="btn" t-on-click.stop="onReverse" t-esc="reverseLabel"/>
                            <button class="btn btn-danger" t-on-click.stop="onCancel">Cancel</button>
                        </t>
                        <t t-if="isCancelled">
                            <button class="btn" t-on-click.stop="onResetDraft">Reset to Draft</button>
                        </t>
                        <t t-if="!isNew">
                            <button class="btn" t-on-click.stop="onPrint">Print</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Back</button>
                    </div>
                </div>
            </div>

            <!-- Status bar -->
            <div class="so-statusbar">
                <div class="so-sb-left">
                    <span t-if="state.record.payment_state === 'paid'"
                          style="font-size:.8rem;color:var(--ok);font-weight:600;">&#10003; Paid</span>
                    <span t-if="state.record.payment_state === 'partial'"
                          style="font-size:.8rem;color:var(--muted);font-weight:600;">Partial</span>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Draft</div>
                    <div t-attf-class="so-step{{stepClass('posted')}}">Posted</div>
                    <div t-attf-class="so-step{{stepClass('cancel')}}">Cancelled</div>
                </div>
            </div>

            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'Draft Invoice'"/>
                    </div>

                    <!-- Two-column info grid -->
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Customer</label>
                                <select class="form-input" data-field="partner_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.partners" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.partner_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Invoice Date</label>
                                <DatePicker value="formatDate(state.record.invoice_date)"
                                            onSelect.bind="setInvoiceDate"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Due Date</label>
                                <DatePicker value="formatDate(state.record.due_date)"
                                            onSelect.bind="setDueDate"/>
                            </div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Source</label>
                                <input class="form-input" readonly="readonly"
                                       t-att-value="state.record.invoice_origin || ''"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Journal</label>
                                <select class="form-input" data-field="journal_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.journals" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.journal_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Payment Terms</label>
                                <select class="form-input" data-field="payment_term_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.paymentTerms" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.payment_term_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Reference</label>
                                <input class="form-input" data-field="ref"
                                       t-att-value="formatVal(state.record.ref)"/>
                            </div>
                        </div>
                    </div>

                    <!-- Invoice lines table -->
                    <table class="so-lines-table inv-lines-table">
                        <thead>
                            <tr>
                                <th class="so-col-desc">Description</th>
                                <th class="so-col-num">Qty</th>
                                <th class="so-col-num">Unit Price</th>
                                <th class="so-col-num">Tax</th>
                                <th class="so-col-subtotal">Subtotal</th>
                                <th class="so-col-del"/>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.lines" t-as="ln" t-key="ln._key">
                                <t t-if="ln.display_type === 'line_section'">
                                    <tr class="inv-row-section">
                                        <td colspan="4">
                                            <input class="inv-sect-input"
                                                   t-att-data-key="ln._key"
                                                   data-line-field="name"
                                                   t-att-value="ln.name || ''"
                                                   placeholder="Section title…"/>
                                        </td>
                                        <td class="so-col-del">
                                            <button class="btn btn-sm" t-att-data-key="ln._key" data-del-line="1">&#x2715;</button>
                                        </td>
                                    </tr>
                                </t>
                                <t t-elif="ln.display_type === 'line_note'">
                                    <tr class="inv-row-note">
                                        <td colspan="4">
                                            <input class="inv-note-input"
                                                   t-att-data-key="ln._key"
                                                   data-line-field="name"
                                                   t-att-value="ln.name || ''"
                                                   placeholder="Note…"/>
                                        </td>
                                        <td class="so-col-del">
                                            <button class="btn btn-sm" t-att-data-key="ln._key" data-del-line="1">&#x2715;</button>
                                        </td>
                                    </tr>
                                </t>
                                <t t-else="">
                                    <tr>
                                        <td class="so-col-desc">
                                            <input class="inv-line-input"
                                                   t-att-data-key="ln._key"
                                                   data-line-field="name"
                                                   t-att-value="ln.name || ''"
                                                   placeholder="Description"/>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="inv-line-input" type="number" min="0"
                                                   t-att-data-key="ln._key"
                                                   data-line-field="quantity"
                                                   t-att-value="ln.quantity"/>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="inv-line-input" type="number" min="0" step="0.01"
                                                   t-att-data-key="ln._key"
                                                   data-line-field="price_unit"
                                                   t-att-value="ln.price_unit"/>
                                        </td>
                                        <!-- P3: tax picker. Writes tax_ids_json,
                                             which the server's TaxEngine reads to
                                             generate the invoice's tax lines. -->
                                        <td class="so-col-num">
                                            <select class="inv-line-input"
                                                    t-att-data-key="ln._key"
                                                    data-line-field="tax_ids_json">
                                                <option value="[]">No tax</option>
                                                <t t-foreach="state.taxOptions" t-as="tx" t-key="tx.id">
                                                    <option t-att-value="'[' + tx.id + ']'"
                                                            t-att-selected="ln.tax_ids_json === ('[' + tx.id + ']') ? true : undefined"
                                                            t-esc="tx.display"/>
                                                </t>
                                            </select>
                                        </td>
                                        <td class="so-col-subtotal" t-esc="formatMoney(ln.credit)"/>
                                        <td class="so-col-del">
                                            <button class="btn btn-sm" t-att-data-key="ln._key" data-del-line="1">&#x2715;</button>
                                        </td>
                                    </tr>
                                </t>
                            </t>
                        </tbody>
                        <tfoot>
                            <tr class="inv-add-row">
                                <td colspan="5">
                                    <button class="btn so-add-line" data-add-line="normal">+ Add a line</button>
                                    <button class="btn so-add-line" data-add-line="section">+ Add a section</button>
                                    <button class="btn so-add-line" data-add-line="note">+ Add a note</button>
                                </td>
                            </tr>
                        </tfoot>
                    </table>

                    <!-- Footer: notes + totals -->
                    <div class="so-footer">
                        <div class="so-notes-wrap">
                            <label class="so-notes-lbl">Notes</label>
                            <textarea class="so-notes-ta" data-field="narration"
                                      t-att-value="state.record.narration || ''"/>
                        </div>
                        <div class="so-totals">
                            <div class="so-total-row">
                                <span class="so-total-lbl">Untaxed</span>
                                <span class="so-total-val" t-esc="formatMoney(state.record.amount_untaxed)"/>
                            </div>
                            <div class="so-total-row">
                                <span class="so-total-lbl">Taxes</span>
                                <span class="so-total-val" t-esc="formatMoney(state.record.amount_tax)"/>
                            </div>
                            <div class="so-total-row so-total-grand">
                                <span class="so-total-lbl">Total</span>
                                <span class="so-total-val" t-esc="formatMoney(state.record.amount_total)"/>
                            </div>
                            <div class="so-total-row">
                                <span class="so-total-lbl">Amount Due</span>
                                <span class="so-total-val" t-esc="formatMoney(state.record.amount_residual)"/>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Supporting documents -->
                <t t-if="!isNew">
                    <AttachmentPanel model="'account.move'"
                                     recordId="props.recordId"
                                     title="'Supporting Documents'"/>
                </t>

                <!-- Chatter -->
                <t t-if="!isNew">
                    <ChatterPanel model="'account.move'"
                                  recordId="props.recordId"
                                  refreshKey="state.chatRefreshKey"/>
                </t>
            </t>
        </div>
    `;

    get backLabel()    { return this.props.backLabel || 'Invoices'; }
    get isDraft()      { return this.state.record.state === 'draft'; }
    get isPosted()     { return this.state.record.state === 'posted'; }
    get isCancelled()  { return this.state.record.state === 'cancel'; }
    get isPaid()       { return this.state.record.payment_state === 'paid'; }
    get canRegisterPayment() { return this.isPosted && !this.isPaid; }

    // Credit note / refund support (docs/082)
    get moveType()  { return this.state.record.move_type || 'out_invoice'; }
    get docLabel()  {
        return { out_invoice: 'Invoice', out_refund: 'Credit Note',
                 in_invoice: 'Vendor Bill', in_refund: 'Vendor Refund' }[this.moveType] || 'Invoice';
    }
    get isRefund()  { return this.moveType === 'out_refund' || this.moveType === 'in_refund'; }
    // A posted invoice/bill can be reversed into a credit note / refund.
    get canReverse()   { return this.isPosted && (this.moveType === 'out_invoice' || this.moveType === 'in_invoice'); }
    get reverseLabel() { return this.moveType === 'in_invoice' ? 'Add Refund' : 'Add Credit Note'; }

    stepClass(step) {
        const order = { draft: 0, posted: 1, cancel: 2 };
        const cur   = order[this.state.record.state] ?? -1;
        const s     = order[step];
        if (s === cur) return ' active';
        if (s < cur)   return ' done';
        return '';
    }

    get isNew() { return !this.props.recordId; }

    setup() {
        this.state = useState({
            loading:        true,
            error:          '',
            record:         {},
            lines:          [],
            deletedLineIds: [],
            partners:       [],
            journals:       [],
            paymentTerms:   [],
            taxOptions:     [],   // P3: tax picker on invoice lines
            chatRefreshKey: 0,
            payDialogOpen:  false,
            payDate:        '',
            payError:       '',
            payJournals:    [],
            payJournalId:   null,
            payAmount:      '',
            // P1/FX (docs/048 §4.6)
            payIsForeign:    false,
            payBaseCode:     '',
            payReceivedBase: '',
            payExpectedBase: '',
            payBookedRate:   1,
            payMemo:        '',
        });
        this._nextKey   = 1;
        this._lineDefaults = {};   // account_id, journal_id, company_id, date, partner_id
        onMounted(() => this.load());
        onWillUpdateProps((np) => {
            if (np.recordId !== this.props.recordId) this.load(np.recordId);
        });
    }

    async load(overrideId) {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const fields = [
                'name', 'state', 'move_type', 'partner_id', 'journal_id',
                'invoice_date', 'due_date', 'date', 'ref', 'narration',
                'payment_term_id', 'invoice_origin', 'payment_state',
                'amount_untaxed', 'amount_tax', 'amount_total', 'amount_residual',
                // currency_id + currency_rate drive the FX field in the payment
                // dialog; line_precision governs how many decimals lines show.
                'currency_id', 'currency_rate', 'line_precision',
            ];
            const recId = overrideId ?? this.props.recordId;
            const [rec] = await Promise.all([
                recId
                    ? RpcService.call('account.move', 'read', [[recId]], { fields })
                          .then(r => (Array.isArray(r) ? r[0] : r) || {})
                    : Promise.resolve({}),
                this.loadOpts('res.partner',          'partners',     ['id', 'name']),
                this.loadOpts('account.journal',      'journals',     ['id', 'name']),
                this.loadOpts('account.payment.term', 'paymentTerms', ['id', 'name']),
                this.loadOpts('account.tax',          'taxOptions',   ['id', 'name']),
            ]);
            this.state.record = rec;
            this.state.deletedLineIds = [];
            if (recId) await this.loadLines(recId);
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async loadOpts(model, key, fields) {
        try {
            const recs = await RpcService.call(model, 'search_read', [[]], { fields, limit: 500 });
            this.state[key] = (Array.isArray(recs) ? recs : []).map(r => ({
                id: r.id, display: r.name || String(r.id),
            }));
        } catch (_) { this.state[key] = []; }
    }

    async loadLines(overrideId) {
        try {
            const recId = overrideId ?? this.props.recordId;
            // Load income lines + section/note lines (exclude AR/AP debit-only lines).
            //
            // tax_line_id = null excludes the SERVER-GENERATED tax lines. They
            // are credit lines with debit = 0 too, so without this filter they
            // would appear in the editable grid — and saving would then write
            // them back as product lines, doubling the invoice.
            // Note `null`, not `false`: the domain compiler turns null into
            // `IS NULL`, whereas false binds the string 'false' to an integer
            // column and errors.
            const raw = await RpcService.call('account.move.line', 'search_read',
                [[['move_id', '=', recId],
                  ['debit', '=', 0],
                  ['tax_line_id', '=', null]]],
                { fields: ['id', 'name', 'quantity', 'price_unit', 'credit',
                            'display_type', 'account_id', 'journal_id',
                            'company_id', 'date', 'partner_id',
                            'tax_ids_json', 'tax_line_id'], limit: 200 });

            const lines = (Array.isArray(raw) ? raw : []).map(ln => {
                // Fix missing price_unit for old invoices created before the column was added
                let pu = parseFloat(ln.price_unit) || 0;
                const qty = parseFloat(ln.quantity) || 0;
                const credit = parseFloat(ln.credit) || 0;
                if (pu === 0 && qty > 0 && credit > 0) pu = Math.round(credit / qty * 100) / 100;
                return {
                    _key:         'db_' + ln.id,
                    _isNew:       false,
                    id:           ln.id,
                    name:         ln.name || '',
                    quantity:     qty,
                    price_unit:   pu,
                    credit:       credit,
                    display_type: ln.display_type || '',
                    account_id:   ln.account_id,
                    journal_id:   ln.journal_id,
                    company_id:   ln.company_id,
                    date:         ln.date,
                    partner_id:   ln.partner_id,
                    // P3: which taxes this line is subject to
                    tax_ids_json: ln.tax_ids_json || '[]',
                };
            });

            // Store defaults from the first income line for use when creating new lines
            const firstIncome = lines.find(l => !l.display_type);
            if (firstIncome) {
                this._lineDefaults = {
                    account_id: this.getM2oId(firstIncome.account_id),
                    journal_id: this.getM2oId(firstIncome.journal_id),
                    company_id: this.getM2oId(firstIncome.company_id),
                    date:       firstIncome.date || this.formatDate(this.state.record.date),
                    partner_id: this.getM2oId(firstIncome.partner_id),
                };
            } else {
                // Fallback: derive from move header
                this._lineDefaults = {
                    account_id: 0,
                    journal_id: this.getM2oId(this.state.record.journal_id),
                    company_id: 1,
                    date:       this.formatDate(this.state.record.date) || this.formatDate(this.state.record.invoice_date),
                    partner_id: this.getM2oId(this.state.record.partner_id),
                };
            }

            this.state.lines = lines;
        } catch (_) { this.state.lines = []; }
    }

    // ---- helpers ----

    getM2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        return 0;
    }

    formatVal(val) {
        if (val === null || val === undefined || val === false) return '';
        if (Array.isArray(val)) return val[1] ?? '';
        return String(val);
    }

    formatDate(val) {
        if (!val || val === false) return '';
        return String(val).substring(0, 10);
    }

    formatMoney(val) {
        const n = parseFloat(val) || 0;
        return n.toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    }

    recalcLine(line) {
        if (line.display_type) { line.credit = 0; return; }
        const qty   = parseFloat(line.quantity)   || 0;
        const price = parseFloat(line.price_unit) || 0;
        line.credit = Math.round(qty * price * 100) / 100;
    }

    recalcTotals() {
        const untaxed = this.state.lines
            .filter(l => !l.display_type)
            .reduce((s, l) => s + (parseFloat(l.credit) || 0), 0);
        const tax   = parseFloat(this.state.record.amount_tax) || 0;
        const total = Math.round((untaxed + tax) * 100) / 100;
        this.state.record.amount_untaxed  = Math.round(untaxed * 100) / 100;
        this.state.record.amount_total    = total;
        this.state.record.amount_residual = total;
    }

    // ---- event handlers ----

    onAnyChange(e) {
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            const key = e.target.dataset.key;
            const val = e.target.tagName === 'SELECT' ? (parseInt(e.target.value) || 0) : e.target.value;
            this.updateLine(key, lineField, val);
            return;
        }
        const field = e.target.dataset.field;
        if (field && e.target.tagName === 'SELECT') {
            this.state.record[field] = parseInt(e.target.value) || 0;
        }
    }

    onAnyInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            this.updateLine(e.target.dataset.key, lineField, e.target.value);
            return;
        }
        const field = e.target.dataset.field;
        if (field) this.state.record[field] = e.target.value;
    }

    onAnyClick(e) {
        const addType = e.target.dataset.addLine;
        if (addType) {
            e.preventDefault();
            const key = String(this._nextKey++);
            const dt  = addType === 'section' ? 'line_section'
                      : addType === 'note'    ? 'line_note' : '';
            this.state.lines.push({
                _key: key, _isNew: true, id: null,
                name: '', quantity: 1, price_unit: 0, credit: 0, tax_ids_json: '[]',
                display_type: dt,
            });
            return;
        }
        const delKey = e.target.dataset.delLine ? e.target.dataset.key : null;
        if (delKey) {
            e.preventDefault();
            const line = this.state.lines.find(l => l._key === delKey);
            if (line && line.id) this.state.deletedLineIds.push(line.id);
            this.state.lines = this.state.lines.filter(l => l._key !== delKey);
            this.recalcTotals();
        }
    }

    updateLine(key, field, val) {
        const line = this.state.lines.find(l => l._key === key);
        if (!line) return;
        line[field] = val;
        if (field === 'quantity' || field === 'price_unit') {
            this.recalcLine(line);
            this.recalcTotals();
        }
    }

    setInvoiceDate(v) { this.state.record.invoice_date = v; }
    setDueDate(v)     { this.state.record.due_date      = v; }

    collectRecord() {
        const r = this.state.record;
        return {
            partner_id:      this.getM2oId(r.partner_id)      || null,
            journal_id:      this.getM2oId(r.journal_id)      || null,
            payment_term_id: this.getM2oId(r.payment_term_id) || null,
            invoice_date:    r.invoice_date || null,
            due_date:        r.due_date     || null,
            ref:             r.ref          || null,
            narration:       r.narration    || null,
        };
    }

    async syncLines(moveId) {
        // Delete removed lines
        if (this.state.deletedLineIds.length > 0) {
            await RpcService.call('account.move.line', 'unlink',
                [this.state.deletedLineIds], {});
            this.state.deletedLineIds = [];
        }
        const d = this._lineDefaults;
        for (const ln of this.state.lines) {
            const isSection = ln.display_type === 'line_section';
            const isNote    = ln.display_type === 'line_note';
            const vals = {
                move_id:      moveId,
                name:         ln.name || '',
                quantity:     isSection || isNote ? 0 : (parseFloat(ln.quantity) || 0),
                price_unit:   isSection || isNote ? 0 : (parseFloat(ln.price_unit) || 0),
                debit:        0,
                credit:       isSection || isNote ? 0 : (parseFloat(ln.credit) || 0),
                display_type: ln.display_type || '',
                account_id:   d.account_id || null,
                journal_id:   d.journal_id || null,
                company_id:   d.company_id || 1,
                date:         d.date       || null,
                partner_id:   d.partner_id || null,
                // P3: the server's TaxEngine reads this to build the tax lines
                tax_ids_json: isSection || isNote ? '[]' : (ln.tax_ids_json || '[]'),
            };
            if (ln._isNew) {
                await RpcService.call('account.move.line', 'create', [vals], {});
            } else if (ln.id) {
                await RpcService.call('account.move.line', 'write', [[ln.id], vals], {});
            }
        }
        // Recalculate AR line + move totals on server
        await RpcService.call('account.move', 'recompute_totals', [[moveId]], {});
    }

    async onSave() {
        try {
            const id = this.state.record.id;
            if (id) {
                await RpcService.call('account.move', 'write', [[id], this.collectRecord()], {});
                await this.syncLines(id);
                await this.load();
            }
        } catch (e) { this.state.error = e.message; }
    }

    async onPost() {
        try {
            const id = this.state.record.id;
            if (id) {
                await RpcService.call('account.move', 'write', [[id], this.collectRecord()], {});
                await this.syncLines(id);
            }
            await RpcService.call('account.move', 'action_post', [[this.state.record.id]], {});
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    async onCancel() {
        try {
            await RpcService.call('account.move', 'button_cancel', [[this.state.record.id]], {});
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    async onResetDraft() {
        try {
            await RpcService.call('account.move', 'button_draft', [[this.state.record.id]], {});
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    // Create a credit note / vendor refund by reversing this posted document.
    async onReverse() {
        try {
            await RpcService.call('account.move', 'action_reverse', [[this.state.record.id]], {});
            const where = this.moveType === 'in_invoice' ? 'Vendors ▸ Refunds' : 'Customers ▸ Credit Notes';
            const what  = this.moveType === 'in_invoice' ? 'Refund' : 'Credit note';
            alert(what + ' created as a draft (reversal of ' + (this.state.record.name || 'this document') +
                  ').\nReview and Confirm it under ' + where + '.');
            this.props.onBack();
        } catch (e) {
            alert('Could not create the credit note: ' + (e.message || e));
        }
    }

    async onDelete() {
        if (!confirm('Delete this draft invoice? This cannot be undone.')) return;
        try {
            await RpcService.call('account.move', 'unlink', [[this.state.record.id]], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }

    onPrint() { window.open('/report/pdf/account.move/' + this.state.record.id, '_blank'); }

    // ---- Register Payment ----
    async onOpenPayDialog() {
        const today = new Date().toISOString().slice(0, 10);
        this.state.payDate      = today;
        this.state.payError     = '';
        this.state.payAmount    = String(this.state.record.amount_residual || 0);
        this.state.payMemo      = this.state.record.name || '';
        this.state.payJournalId = null;
        this.state.payJournals  = [];
        this.state.payDialogOpen = true;

        // FX (docs/048 §4.6): decide whether this invoice needs the
        // "amount received in base currency" field.
        this.state.payIsForeign     = false;
        this.state.payBaseCode      = '';
        this.state.payReceivedBase  = '';
        this.state.payExpectedBase  = '';
        this.state.payBookedRate    = 1;

        try {
            const journals = await RpcService.searchRead(
                'account.journal',
                [['type', 'in', ['bank', 'cash']]],
                ['id', 'name'], 0, 50);
            this.state.payJournals  = journals;
            if (journals.length) this.state.payJournalId = journals[0].id;
        } catch (_) {}

        try {
            const [companies, currencies] = await Promise.all([
                RpcService.searchRead('res.company', [], ['id', 'currency_id'], 0, 1),
                RpcService.searchRead('res.currency', [], ['id', 'name', 'rate'], 0, 50),
            ]);
            const baseRaw = companies?.[0]?.currency_id;
            const baseId  = Array.isArray(baseRaw) ? baseRaw[0] : baseRaw;
            const base    = currencies.find(c => c.id === baseId);
            this.state.payBaseCode = base ? base.name : '';

            const invRaw = this.state.record.currency_id;
            const invId  = Array.isArray(invRaw) ? invRaw[0] : invRaw;

            if (invId && baseId && invId !== baseId) {
                this.state.payIsForeign  = true;
                // The rate the INVOICE was booked at, not today's — that is
                // what the FX difference is measured against.
                this.state.payBookedRate = Number(this.state.record.currency_rate) || 1;
                const expected = (parseFloat(this.state.payAmount) || 0) * this.state.payBookedRate;
                this.state.payExpectedBase = expected.toFixed(2);
            }
        } catch (_) { /* currency lookup is advisory; payment still works */ }
    }

    // Derived preview so the user sees what will be booked before validating.
    get effectiveRateText() {
        const amt  = parseFloat(this.state.payAmount) || 0;
        const recv = parseFloat(this.state.payReceivedBase);
        if (!amt || !Number.isFinite(recv) || recv <= 0) return '—';
        return (recv / amt).toFixed(6);
    }

    get fxDiffText() {
        const amt  = parseFloat(this.state.payAmount) || 0;
        const recv = parseFloat(this.state.payReceivedBase);
        if (!amt || !Number.isFinite(recv) || recv <= 0) return '—';
        const diff = recv - amt * (this.state.payBookedRate || 1);
        const sign = diff < 0 ? 'loss' : 'gain';
        return `${diff.toFixed(2)} ${this.state.payBaseCode} (${sign})`;
    }

    onClosePayDialog() {
        this.state.payDialogOpen = false;
        this.state.payError      = '';
    }

    setPayDate(v) { this.state.payDate = v; }

    async onConfirmPayment() {
        if (!this.state.payDate)      { this.state.payError = 'Please select a payment date.'; return; }
        if (!this.state.payJournalId) { this.state.payError = 'Please select a journal.'; return; }
        const amount = parseFloat(this.state.payAmount);
        if (isNaN(amount) || amount <= 0) { this.state.payError = 'Enter a valid amount.'; return; }
        try {
            this.state.payError = '';
            await RpcService.call(
                'account.move', 'action_register_payment',
                [[this.state.record.id]],
                {
                    payment_date: this.state.payDate,
                    journal_id:   this.state.payJournalId,
                    amount:       amount,
                    memo:         this.state.payMemo,
                    // Only sent for a foreign-currency invoice. The server
                    // derives the effective rate from it and posts the
                    // realised FX difference to 7900.
                    ...(this.state.payIsForeign &&
                        parseFloat(this.state.payReceivedBase) > 0
                            ? { amount_received_base: parseFloat(this.state.payReceivedBase) }
                            : {}),
                });
            this.state.payDialogOpen = false;
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) {
            this.state.payError = e.message || 'Payment registration failed.';
        }
    }
}

// ----------------------------------------------------------------
// SaleOrderFormView — Odoo 14-style Sales Order form
// ----------------------------------------------------------------
class SaleOrderFormView extends Component {
    static components = { DatePicker, InvoiceFormView, ChatterPanel, AttachmentPanel };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput"
             t-on-click="onAnyClick">

            <!-- Invoice sub-view overlay -->
            <t t-if="state.invoiceMode">
                <InvoiceFormView recordId="state.invoiceMode.invoiceId"
                                 backLabel="state.invoiceMode.fromList ? ('← ' + (state.record.name || 'Sales Order')) : ('← ' + (state.record.name || 'Sales Order'))"
                                 onBack.bind="closeInvoiceView"
                                 navIdx="state.invoiceMode.idx"
                                 navTotal="state.invoiceList.length"
                                 onPrev.bind="prevInvoice"
                                 onNext.bind="nextInvoice"/>
            </t>
            <!-- Invoice list (when multiple invoices) -->
            <t t-elif="state.invoiceListMode">
                <div class="so-page-header">
                    <div class="so-header-left">
                        <div class="so-breadcrumbs">
                            <span class="so-bc-link" t-on-click.stop="closeInvoiceList" t-esc="state.record.name || 'Sales Order'"/>
                            <span class="so-bc-sep">›</span>
                            <span class="so-bc-cur">Invoices</span>
                        </div>
                    </div>
                </div>
                <div class="so-body">
                    <table class="so-line-table" style="width:100%">
                        <thead>
                            <tr>
                                <th>Invoice #</th>
                                <th>Status</th>
                                <th>Payment</th>
                                <th style="text-align:right">Amount</th>
                                <th></th>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.invoiceList" t-as="inv" t-key="inv.id">
                                <tr class="list-row"
                                    t-att-data-inv-id="inv.id"
                                    t-on-click="onInvRowClick">
                                    <td t-esc="inv.name || '/'"/>
                                    <td t-esc="inv.state"/>
                                    <td t-esc="inv.payment_state || '—'"/>
                                    <td style="text-align:right" t-esc="formatMoney(inv.amount_total)"/>
                                    <td style="text-align:center">
                                        <t t-if="inv.state === 'draft' || inv.state === 'cancel'">
                                            <button class="btn btn-danger" style="padding:2px 8px;font-size:12px"
                                                    t-att-data-del-inv-id="inv.id"
                                                    t-on-click.stop="onInvDeleteClick">Delete</button>
                                        </t>
                                    </td>
                                </tr>
                            </t>
                        </tbody>
                    </table>
                </div>
            </t>
            <t t-else="">

            <!-- Page header -->
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Quotations</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="state.isNew">
                            <button class="btn btn-primary" t-on-click.stop="onCreate">Create</button>
                        </t>
                        <t t-else="">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                            <button class="btn" t-on-click.stop="onPrint">Print</button>
                            <button class="btn" t-on-click.stop="onProforma" title="Open a Pro-Forma Invoice PDF the customer can pay against">Pro-Forma Invoice</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Discard</button>
                    </div>
                </div>
            </div>

            <!-- Status bar -->
            <div class="so-statusbar">
                <div class="so-sb-left">
                    <t t-if="showConfirm">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onConfirm">Confirm</button>
                    </t>
                    <t t-if="showCreateInvoice">
                        <button class="btn so-wf-btn" t-on-click.stop="onCreateInvoice">Create Invoice</button>
                        <button class="btn so-wf-btn" t-on-click.stop="onToggleDownPaymentForm">Down Payment</button>
                    </t>
                    <t t-if="showProgressInvoice">
                        <button class="btn so-wf-btn" t-on-click.stop="onToggleProgressForm">Progress Invoice</button>
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onCreateFinalInvoice">Final Invoice</button>
                        <span class="so-inv-remaining">
                            Invoiced: <t t-esc="formatMoney(state.invoicedAmount)"/>
                            <span style="margin:0 6px;color:var(--muted)">·</span>
                            Remaining: <t t-esc="formatMoney(remainingAmount)"/>
                        </span>
                    </t>
                    <t t-if="showCancelBtn">
                        <button class="btn ghost so-wf-btn" t-on-click.stop="onCancel">Cancel</button>
                    </t>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Quotation</div>
                    <div t-attf-class="so-step{{stepClass('sent')}}">Quotation Sent</div>
                    <div t-attf-class="so-step{{stepClass('sale')}}">Sales Order</div>
                </div>
            </div>

            <!-- Down payment / progress invoice inline form -->
            <t t-if="state.dpFormOpen">
                <div class="dp-form-bar">
                    <span class="dp-form-title" t-esc="state.dpMode === 'progress' ? 'Progress Invoice' : 'Down Payment'"/>
                    <input type="number" class="form-input dp-amount-input" placeholder="Amount"
                           t-att-value="state.dpAmount"
                           t-on-input="onDpAmountInput"/>
                    <button class="btn dp-pct-btn" t-on-click.stop="onDpSetPercent" data-pct="30">30%</button>
                    <button class="btn dp-pct-btn" t-on-click.stop="onDpSetPercent" data-pct="50">50%</button>
                    <button class="btn dp-pct-btn" t-on-click.stop="onDpSetPercent" data-pct="100">100%</button>
                    <input type="text" class="form-input dp-note-input" placeholder="Description (e.g. Down Payment, Progress Claim)"
                           t-att-value="state.dpNote"
                           t-on-input="onDpNoteInput"/>
                    <button class="btn btn-primary" t-on-click.stop="onSubmitDownPayment">Create</button>
                    <button class="btn" t-on-click.stop="onToggleDownPaymentForm">Cancel</button>
                    <span t-if="state.dpError" class="dp-error" t-esc="state.dpError"/>
                </div>
            </t>

            <!-- Loading / error / content -->
            <t t-if="state.loading">
                <div class="loading">Loading…</div>
            </t>
            <t t-elif="state.error">
                <div class="error" t-esc="state.error"/>
            </t>
            <t t-else="">
                <div class="so-card">
                    <!-- Card header -->
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New Quotation'"/>
                        <div class="so-stat-btns">
                            <div t-attf-class="so-stat-btn{{ props.recordId ? '' : ' so-stat-btn-disabled' }}"
                                 t-on-click.stop="onPreviewPdf"
                                 title="Open this order as a printable document">
                                <span class="so-stat-num">&#128196;</span>
                                <span class="so-stat-lbl">Preview</span>
                            </div>
                            <div class="so-stat-btn" t-on-click.stop="onViewDeliveries">
                                <span class="so-stat-num" t-esc="state.deliveryCount"/>
                                <span class="so-stat-lbl">Delivery</span>
                            </div>
                            <div class="so-stat-btn" t-on-click.stop="onViewInvoices">
                                <span class="so-stat-num" t-esc="state.invoiceCount"/>
                                <span class="so-stat-lbl">Invoices</span>
                            </div>
                        </div>
                    </div>

                    <!-- Two-column info grid -->
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Customer</label>
                                <select class="form-input" data-field="partner_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.partners" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.partner_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Invoice Address</label>
                                <select class="form-input" data-field="partner_invoice_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.partners" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.partner_invoice_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Delivery Address</label>
                                <select class="form-input" data-field="partner_shipping_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.partners" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.partner_shipping_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Payment Terms</label>
                                <select class="form-input" data-field="payment_term_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.paymentTerms" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.payment_term_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Order Date</label>
                                <DatePicker value="formatDate(state.record.date_order)"
                                            onSelect.bind="setDateOrder"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Expiration</label>
                                <DatePicker value="formatDate(state.record.validity_date)"
                                            onSelect.bind="setValidityDate"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Salesperson</label>
                                <select class="form-input" data-field="user_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.users" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.user_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Customer Ref</label>
                                <input class="form-input" data-field="client_order_ref"
                                       t-att-value="formatVal(state.record.client_order_ref)"/>
                            </div>
                        </div>
                    </div>

                    <!-- Tabs -->
                    <div class="so-tabs">
                        <span t-attf-class="so-tab{{state.activeTab === 'lines' ? ' active' : ''}}"
                              t-on-click.stop="setTabLines">Order Lines</span>
                        <span t-attf-class="so-tab{{state.activeTab === 'other' ? ' active' : ''}}"
                              t-on-click.stop="setTabOther">Other Info</span>
                    </div>

                    <!-- Order lines tab -->
                    <t t-if="state.activeTab === 'lines'">
                        <table class="so-lines-table">
                            <thead>
                                <tr>
                                    <th>Product</th>
                                    <th>Description</th>
                                    <th>Qty</th>
                                    <th>UoM</th>
                                    <th>Unit Price</th>
                                    <th>Disc.%</th>
                                    <th>Subtotal</th>
                                    <th></th>
                                </tr>
                            </thead>
                            <tbody>
                                <t t-foreach="state.lines" t-as="line" t-key="line._key">
                                    <t t-if="line.display_type === 'line_section'">
                                        <tr class="inv-row-section">
                                            <td colspan="7">
                                                <input class="inv-sect-input"
                                                       data-line-field="name"
                                                       t-att-data-key="line._key"
                                                       t-att-value="line.name || ''"
                                                       placeholder="Section name…"/>
                                            </td>
                                            <td class="so-col-del">
                                                <button class="btn btn-sm btn-danger"
                                                        data-del-line="1"
                                                        t-att-data-key="line._key">✕</button>
                                            </td>
                                        </tr>
                                    </t>
                                    <t t-elif="line.display_type === 'line_note'">
                                        <tr class="inv-row-note">
                                            <td colspan="7">
                                                <input class="inv-note-input"
                                                       data-line-field="name"
                                                       t-att-data-key="line._key"
                                                       t-att-value="line.name || ''"
                                                       placeholder="Note…"/>
                                            </td>
                                            <td class="so-col-del">
                                                <button class="btn btn-sm btn-danger"
                                                        data-del-line="1"
                                                        t-att-data-key="line._key">✕</button>
                                            </td>
                                        </tr>
                                    </t>
                                    <t t-else="">
                                    <tr>
                                        <td class="so-col-product">
                                            <select class="o2m-input"
                                                    data-line-field="product_id"
                                                    t-att-data-key="line._key">
                                                <option value="0">—</option>
                                                <t t-foreach="state.products" t-as="opt" t-key="opt.id">
                                                    <option t-att-value="opt.id"
                                                            t-att-selected="getM2oId(line.product_id) === opt.id ? true : undefined"
                                                            t-esc="opt.display"/>
                                                </t>
                                            </select>
                                        </td>
                                        <td class="so-col-desc">
                                            <input class="o2m-input"
                                                   data-line-field="name"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.name || ''"/>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="o2m-input" type="number" step="0.01"
                                                   data-line-field="product_uom_qty"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.product_uom_qty !== undefined ? line.product_uom_qty : 1"/>
                                        </td>
                                        <td>
                                            <select class="o2m-input"
                                                    data-line-field="product_uom_id"
                                                    t-att-data-key="line._key">
                                                <option value="0">—</option>
                                                <t t-foreach="state.uoms" t-as="opt" t-key="opt.id">
                                                    <option t-att-value="opt.id"
                                                            t-att-selected="getM2oId(line.product_uom_id) === opt.id ? true : undefined"
                                                            t-esc="opt.display"/>
                                                </t>
                                            </select>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="o2m-input" type="number" step="0.01"
                                                   data-line-field="price_unit"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.price_unit !== undefined ? line.price_unit : 0"/>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="o2m-input" type="number" step="0.01"
                                                   data-line-field="discount"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.discount !== undefined ? line.discount : 0"/>
                                        </td>
                                        <td class="so-col-num so-col-subtotal">
                                            <span t-esc="formatMoney(line.price_subtotal)"/>
                                        </td>
                                        <td class="so-col-del">
                                            <button class="btn btn-sm btn-danger"
                                                    data-del-line="1"
                                                    t-att-data-key="line._key">✕</button>
                                        </td>
                                    </tr>
                                    </t>
                                </t>
                            </tbody>
                        </table>
                        <div class="so-line-adds">
                            <button class="btn so-add-line" data-add-line="normal">+ Add a line</button>
                            <button class="btn so-add-line" data-add-line="section">+ Add a section</button>
                            <button class="btn so-add-line" data-add-line="note">+ Add a note</button>
                        </div>

                        <div class="so-footer">
                            <div class="so-notes-wrap">
                                <label class="so-notes-lbl">Notes / Terms</label>
                                <textarea class="so-notes-ta" data-field="note"><t t-esc="state.record.note || ''"/></textarea>
                            </div>
                            <div class="so-totals">
                                <div class="so-total-row">
                                    <span class="so-total-lbl">Untaxed Amount</span>
                                    <span class="so-total-val" t-esc="formatMoney(state.record.amount_untaxed)"/>
                                </div>
                                <div class="so-total-row">
                                    <span class="so-total-lbl">Taxes</span>
                                    <span class="so-total-val" t-esc="formatMoney(state.record.amount_tax)"/>
                                </div>
                                <div class="so-total-row so-total-grand">
                                    <span class="so-total-lbl">Total</span>
                                    <span class="so-total-val" t-esc="formatMoney(state.record.amount_total)"/>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- Other info tab -->
                    <t t-if="state.activeTab === 'other'">
                        <div class="so-other-tab">
                            <div class="so-field-row">
                                <label>Currency</label>
                                <select class="form-input" data-field="currency_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.currencies" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.currency_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label>Source</label>
                                <input class="form-input" data-field="origin"
                                       t-att-value="formatVal(state.record.origin)"/>
                            </div>
                            <div class="so-field-row">
                                <label>Invoice Status</label>
                                <input class="form-input" readonly="readonly"
                                       t-att-value="formatVal(state.record.invoice_status)"/>
                            </div>
                        </div>
                    </t>
                </div>

                <!-- Signed quotations, customer purchase orders -->
                <t t-if="!state.isNew">
                    <AttachmentPanel model="'sale.order'"
                                     recordId="props.recordId"
                                     title="'Documents'"/>
                </t>

                <!-- Chatter -->
                <t t-if="!state.isNew">
                    <ChatterPanel model="'sale.order'"
                                  recordId="props.recordId"
                                  refreshKey="state.chatRefreshKey"/>
                </t>
            </t>
            </t> <!-- end t-else: invoice mode off -->
        </div>
    `;

    setup() {
        this.state = useState({
            loading:        true,
            error:          '',
            isNew:          !this.props.recordId,
            record:         {},
            activeTab:      'lines',
            lines:          [],
            deletedLineIds: [],
            partners:       [],
            paymentTerms:   [],
            users:          [],
            currencies:     [],
            products:       [],
            uoms:           [],
            deliveryCount:  0,
            invoiceCount:   0,
            invoiceIds:     [],
            invoiceList:    [],
            invoiceListMode: false,
            invoiceMode:    null,   // null | { invoiceId: N }
            dpFormOpen:     false,
            dpMode:         'down_payment', // 'down_payment' | 'progress'
            dpAmount:       '',
            dpNote:         'Down Payment',
            dpError:        '',
            invoicedAmount: 0,
            chatRefreshKey: 0,
        });
        this._nextKey = 1;
        onMounted(() => this.load());
    }

    // ---- Getters for workflow button visibility (avoid && in XML) ----

    get showConfirm() {
        const s = this.state.record.state;
        return this.state.isNew || s === 'draft' || s === 'sent';
    }

    get showCreateInvoice() {
        const invStatus = this.state.record.invoice_status;
        return this.state.record.state === 'sale' &&
               invStatus !== 'invoiced' && invStatus !== 'to invoice';
    }

    get showProgressInvoice() {
        return this.state.record.state === 'sale' &&
               this.state.record.invoice_status === 'to invoice';
    }

    get remainingAmount() {
        const total    = parseFloat(this.state.record.amount_total) || 0;
        const invoiced = parseFloat(this.state.invoicedAmount) || 0;
        return Math.max(0, total - invoiced);
    }

    get showCancelBtn() {
        const s = this.state.record.state;
        return !this.state.isNew && s !== 'cancel';
    }

    // ---- Data loading ----

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const fields = [
                'name', 'state', 'partner_id', 'partner_invoice_id', 'partner_shipping_id',
                'payment_term_id', 'date_order', 'validity_date', 'user_id', 'client_order_ref',
                'currency_id', 'origin', 'invoice_status', 'note',
                'amount_untaxed', 'amount_tax', 'amount_total',
            ];
            const [recordData] = await Promise.all([
                this.props.recordId
                    ? RpcService.call('sale.order', 'read', [[this.props.recordId]], { fields })
                          .then(r => (Array.isArray(r) ? r[0] : r) || {})
                    : Promise.resolve({}),
                this.loadOpts('res.partner',             'partners',     ['id', 'name']),
                this.loadOpts('account.payment.term',    'paymentTerms', ['id', 'name']),
                this.loadOpts('res.users',               'users',        ['id', 'name']),
                this.loadOpts('res.currency',            'currencies',   ['id', 'name']),
                this.loadOpts('product.product',         'products',     ['id', 'name']),
                this.loadOpts('uom.uom',                 'uoms',         ['id', 'name']),
            ]);
            this.state.record = recordData;
            // Default currency for new records
            if (!this.props.recordId && !this.state.record.currency_id && this.state.currencies.length > 0) {
                this.state.record.currency_id = this.state.currencies[0].id;
            }
            if (this.props.recordId) {
                await this.loadLines();
                try {
                    const picks = await RpcService.call('stock.picking', 'search_read',
                        [[['sale_id', '=', this.props.recordId]]],
                        { fields: ['id'], limit: 500 });
                    this.state.deliveryCount = Array.isArray(picks) ? picks.length : 0;
                } catch (_) {}
                try {
                    const moves = await RpcService.call('account.move', 'search_read',
                        [[['sale_id', '=', this.props.recordId],
                          ['move_type', '=', 'out_invoice']]],
                        { fields: ['id', 'name', 'amount_total', 'state', 'payment_state'], limit: 500 });
                    const all = Array.isArray(moves) ? moves : [];
                    this.state.invoiceIds   = all.map(m => m.id);
                    this.state.invoiceList  = all;
                    this.state.invoiceCount = all.length;
                    this.state.invoicedAmount = all
                        .filter(m => m.state === 'posted')
                        .reduce((s, m) => s + (parseFloat(m.amount_total) || 0), 0);
                } catch (_) {}
            }
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async loadOpts(model, key, fields) {
        try {
            const recs = await RpcService.call(model, 'search_read', [[]], { fields, limit: 500 });
            this.state[key] = (Array.isArray(recs) ? recs : []).map(r => ({
                id:      r.id,
                display: r.name || String(r.id),
            }));
        } catch (_) {
            this.state[key] = [];
        }
    }

    async loadLines() {
        try {
            const lineFields = [
                'id', 'product_id', 'name', 'product_uom_qty', 'product_uom_id',
                'price_unit', 'discount', 'price_subtotal', 'display_type',
            ];
            const rows = await RpcService.call('sale.order.line', 'search_read',
                [[['order_id', '=', this.props.recordId]]],
                { fields: lineFields, limit: 500 });
            this.state.lines = (Array.isArray(rows) ? rows : []).map(r => ({
                _key: String(this._nextKey++),
                ...r,
            }));
        } catch (_) {
            this.state.lines = [];
        }
    }

    // ---- Helpers ----

    getM2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        if (typeof val === 'string') return parseInt(val) || 0;
        return 0;
    }

    formatVal(val) {
        if (val === null || val === undefined || val === false) return '';
        if (Array.isArray(val)) return val[1] ?? '';
        return String(val);
    }

    formatDate(val) {
        if (!val || val === false) return '';
        return String(val).substring(0, 10);
    }

    formatMoney(val) {
        const n = parseFloat(val) || 0;
        return n.toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    }

    recalcLine(line) {
        const qty   = parseFloat(line.product_uom_qty) || 0;
        const price = parseFloat(line.price_unit)      || 0;
        const disc  = parseFloat(line.discount)        || 0;
        line.price_subtotal = Math.round(qty * price * (1 - disc / 100) * 100) / 100;
    }

    stepClass(step) {
        const order = { draft: 0, sent: 1, sale: 2 };
        const cur   = order[this.state.record.state] ?? -1;
        const s     = order[step];
        if (s === cur)  return ' active';
        if (s < cur)    return ' done';
        return '';
    }

    // ---- Tab switching (named methods — safe outside t-foreach) ----

    setTabLines() { this.state.activeTab = 'lines'; }
    setTabOther()  { this.state.activeTab = 'other'; }

    // ---- Event delegation handlers ----

    onAnyChange(e) {
        if (this.state.invoiceMode || this.state.invoiceListMode) return;
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            const key = e.target.dataset.key;
            const val = e.target.tagName === 'SELECT'
                ? (parseInt(e.target.value) || 0)
                : e.target.value;
            this.updateLine(key, lineField, val);
            return;
        }
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.tagName === 'SELECT') {
            this.state.record[field] = parseInt(e.target.value) || 0;
        }
    }

    onAnyInput(e) {
        if (this.state.invoiceMode || this.state.invoiceListMode) return;
        if (e.target.tagName === 'SELECT') return;
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            this.updateLine(e.target.dataset.key, lineField, e.target.value);
            return;
        }
        const field = e.target.dataset.field;
        if (field) {
            this.state.record[field] = e.target.value;
        }
    }

    onAnyClick(e) {
        if (this.state.invoiceMode || this.state.invoiceListMode) return;
        const addType = e.target.dataset.addLine;
        if (addType) {
            e.preventDefault();
            const dt = addType === 'section' ? 'line_section'
                     : addType === 'note'    ? 'line_note' : '';
            this.state.lines.push({
                _key:            String(this._nextKey++),
                id:              null,
                product_id:      0,
                name:            '',
                product_uom_qty: 1,
                product_uom_id:  0,
                price_unit:      0,
                discount:        0,
                price_subtotal:  0,
                display_type:    dt,
            });
            return;
        }
        if (e.target.dataset.delLine) {
            e.preventDefault();
            const key  = e.target.dataset.key;
            const line = this.state.lines.find(l => l._key === key);
            if (line && line.id) this.state.deletedLineIds.push(line.id);
            this.state.lines = this.state.lines.filter(l => l._key !== key);
        }
    }

    // ---- Line update + product defaults ----

    updateLine(key, field, val) {
        const line = this.state.lines.find(l => l._key === key);
        if (!line) return;
        line[field] = val;
        if (field === 'product_id' && val > 0) {
            this.triggerLineOnchange(key, field);
        } else if (field === 'product_uom_qty' || field === 'price_unit' || field === 'discount') {
            this.recalcLine(line);
        }
    }

    // Server-side onchange for a sale order line field.
    // The server fills in name/price_unit/uom when product_id changes.
    async triggerLineOnchange(key, changedField) {
        const line = this.state.lines.find(l => l._key === key);
        if (!line) return;
        try {
            const res = await RpcService.call(
                'sale.order.line', 'onchange', [line, changedField, {}], {});
            if (res && res.value) {
                Object.assign(line, res.value);
                // After product_id fill, recalc subtotal with whatever price was returned
                if (changedField === 'product_id') {
                    if (!line.product_uom_qty) line.product_uom_qty = 1;
                    this.recalcLine(line);
                }
            }
        } catch (_) {}
    }

    // ---- Collect + sync ----

    collectRecord() {
        const r = this.state.record;
        return {
            partner_id:          this.getM2oId(r.partner_id),
            partner_invoice_id:  this.getM2oId(r.partner_invoice_id),
            partner_shipping_id: this.getM2oId(r.partner_shipping_id),
            payment_term_id:     this.getM2oId(r.payment_term_id),
            user_id:             this.getM2oId(r.user_id),
            currency_id:         this.getM2oId(r.currency_id) || false,
            date_order:          r.date_order          || false,
            validity_date:       r.validity_date       || false,
            client_order_ref:    r.client_order_ref    || false,
            origin:              r.origin              || false,
            note:                r.note                || false,
        };
    }

    async syncLines(parentId) {
        const deleted = this.state.deletedLineIds;
        if (deleted.length) {
            await RpcService.call('sale.order.line', 'unlink', [deleted], {});
            this.state.deletedLineIds = [];
        }
        for (const line of this.state.lines) {
            const isSN = line.display_type === 'line_section' || line.display_type === 'line_note';
            const vals = {
                order_id:        parentId,
                product_id:      isSN ? false : this.getM2oId(line.product_id),
                name:            line.name || '',
                product_uom_qty: isSN ? 0 : (parseFloat(line.product_uom_qty) || 1),
                product_uom_id:  isSN ? false : (this.getM2oId(line.product_uom_id) || false),
                price_unit:      isSN ? 0 : (parseFloat(line.price_unit) || 0),
                discount:        isSN ? 0 : (parseFloat(line.discount) || 0),
                display_type:    line.display_type || '',
            };
            if (!line.id) {
                await RpcService.call('sale.order.line', 'create', [vals], {});
            } else {
                await RpcService.call('sale.order.line', 'write', [[line.id], vals], {});
            }
        }
    }

    // ---- Action handlers ----

    async onSave() {
        try {
            await RpcService.call('sale.order', 'write',
                [[this.state.record.id], this.collectRecord()], {});
            await this.syncLines(this.state.record.id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCreate() {
        try {
            const newId = await RpcService.call('sale.order', 'create',
                [this.collectRecord()], {});
            await this.syncLines(newId);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onDelete() {
        try {
            await RpcService.call('sale.order', 'unlink',
                [[this.state.record.id]], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onConfirm() {
        try {
            let id = this.state.record.id;
            if (!id) {
                id = await RpcService.call('sale.order', 'create', [this.collectRecord()], {});
                await this.syncLines(id);
            } else {
                await RpcService.call('sale.order', 'write', [[id], this.collectRecord()], {});
                await this.syncLines(id);
            }
            await RpcService.call('sale.order', 'action_confirm', [[id]], {});
            this.state.chatRefreshKey++;
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCancel() {
        try {
            await RpcService.call('sale.order', 'action_cancel',
                [[this.state.record.id]], {});
            this.state.chatRefreshKey++;
            this.state.record.state = 'cancel';
        } catch (e) { this.state.error = e.message; }
    }

    async onCreateInvoice() {
        try {
            await RpcService.call('sale.order', 'action_create_invoices',
                [[this.state.record.id]], {});
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    onToggleDownPaymentForm() {
        const wasOpen = this.state.dpFormOpen && this.state.dpMode === 'down_payment';
        this.state.dpFormOpen = !wasOpen;
        this.state.dpMode     = 'down_payment';
        this.state.dpAmount   = '';
        this.state.dpNote     = 'Down Payment';
        this.state.dpError    = '';
    }

    onToggleProgressForm() {
        const wasOpen = this.state.dpFormOpen && this.state.dpMode === 'progress';
        this.state.dpFormOpen = !wasOpen;
        this.state.dpMode     = 'progress';
        this.state.dpAmount   = '';
        this.state.dpNote     = 'Progress Payment';
        this.state.dpError    = '';
    }

    onDpAmountInput(ev) { this.state.dpAmount = ev.target.value; }
    onDpNoteInput(ev)   { this.state.dpNote   = ev.target.value; }

    onDpSetPercent(ev) {
        const pct   = parseFloat(ev.currentTarget.dataset.pct);
        const base  = this.state.dpMode === 'progress'
            ? this.remainingAmount
            : (parseFloat(this.state.record.amount_total) || 0);
        this.state.dpAmount = ((pct / 100) * base).toFixed(2);
    }

    async onSubmitDownPayment() {
        const amount = parseFloat(this.state.dpAmount);
        if (!amount || amount <= 0) { this.state.dpError = 'Enter a valid amount.'; return; }
        const method = this.state.dpMode === 'progress'
            ? 'action_create_progress_payment'
            : 'action_create_down_payment';
        try {
            await RpcService.call('sale.order', method,
                [[this.state.record.id]], { amount, note: this.state.dpNote });
            this.state.dpFormOpen = false;
            await this.load();
        } catch (e) { this.state.dpError = e.message; }
    }

    async onCreateFinalInvoice() {
        this.state.dpFormOpen = false;
        try {
            await RpcService.call('sale.order', 'action_create_final_invoice',
                [[this.state.record.id]], {});
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }

    setDateOrder(v)    { this.state.record.date_order    = v; }
    setValidityDate(v) { this.state.record.validity_date = v; }

    onViewDeliveries() {
        // Full delivery view is in the Inventory app; navigate back to list for now
        this.props.onBack();
    }

    // The report route renders the sale.order document template with a
    // Print / Save as PDF button, so this is the PDF preview.
    onPreviewPdf() {
        if (!this.props.recordId) return;
        window.open(`/report/html/sale.order/${this.props.recordId}`, '_blank');
    }

    onViewInvoices() {
        if (this.state.invoiceIds.length === 0) return;
        if (this.state.invoiceIds.length === 1) {
            this.state.invoiceMode = { invoiceId: this.state.invoiceIds[0], idx: 0, fromList: false };
        } else {
            this.state.invoiceListMode = true;
        }
    }

    openInvoiceFromList(id) {
        const idx = this.state.invoiceList.findIndex(inv => inv.id === id);
        this.state.invoiceListMode = false;
        this.state.invoiceMode = { invoiceId: id, idx: idx >= 0 ? idx : 0, fromList: true };
    }

    onInvRowClick(e) {
        const row = e.currentTarget.closest('[data-inv-id]');
        const id = row ? parseInt(row.dataset.invId) : 0;
        if (!id) return;
        this.openInvoiceFromList(id);
    }

    async onInvDeleteClick(e) {
        const id = parseInt(e.currentTarget.dataset.delInvId);
        if (!id) return;
        await this.deleteInvoiceFromList(id);
    }

    closeInvoiceList() {
        this.state.invoiceListMode = false;
    }

    async deleteInvoiceFromList(id) {
        try {
            await RpcService.call('account.move', 'unlink', [[id]], {});
            await this.load();
            if (this.state.invoiceIds.length <= 1) {
                this.state.invoiceListMode = false;
            }
        } catch (e) { this.state.error = e.message; }
    }

    closeInvoiceView() {
        const fromList = this.state.invoiceMode && this.state.invoiceMode.fromList;
        this.state.invoiceMode = null;
        this.state.invoiceListMode = fromList && this.state.invoiceList.length > 1;
    }

    prevInvoice() {
        const cur = this.state.invoiceMode;
        if (!cur || cur.idx <= 0) return;
        const newIdx = cur.idx - 1;
        this.state.invoiceMode = { invoiceId: this.state.invoiceList[newIdx].id, idx: newIdx, fromList: cur.fromList };
    }

    nextInvoice() {
        const cur = this.state.invoiceMode;
        if (!cur || cur.idx >= this.state.invoiceList.length - 1) return;
        const newIdx = cur.idx + 1;
        this.state.invoiceMode = { invoiceId: this.state.invoiceList[newIdx].id, idx: newIdx, fromList: cur.fromList };
    }

    onPrint() { window.open('/report/pdf/sale.order/' + this.state.record.id, '_blank'); }
    onProforma() { window.open('/report/pdf/sale.order/' + this.state.record.id + '?proforma=1', '_blank'); }
}

// ----------------------------------------------------------------
// PurchaseOrderFormView — Odoo 14-style Purchase Order form
// ----------------------------------------------------------------
class PurchaseOrderFormView extends Component {
    static components = { DatePicker, ChatterPanel, AttachmentPanel };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput"
             t-on-click="onAnyClick">

            <!-- Page header -->
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Purchase Orders</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="state.isNew">
                            <button class="btn btn-primary" t-on-click.stop="onCreate">Create</button>
                        </t>
                        <t t-else="">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                            <button class="btn" t-on-click.stop="onPrint">Print</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Discard</button>
                    </div>
                </div>
            </div>

            <!-- Status bar -->
            <div class="so-statusbar">
                <div class="so-sb-left">
                    <t t-if="showConfirm">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onConfirm">Confirm Order</button>
                    </t>
                    <t t-if="showBillActions">
                        <button class="btn so-wf-btn" t-on-click.stop="onCreateBill">Create Bill</button>
                        <button class="btn so-wf-btn" t-on-click.stop="onToggleDownPaymentForm">Down Payment</button>
                    </t>
                    <t t-if="showCancelBtn">
                        <button class="btn ghost so-wf-btn" t-on-click.stop="onCancel">Cancel</button>
                    </t>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Request for Quotation</div>
                    <div t-attf-class="so-step{{stepClass('purchase')}}">Purchase Order</div>
                    <div t-attf-class="so-step{{stepClass('done')}}">Done</div>
                </div>
            </div>

            <!-- Down payment inline form -->
            <t t-if="state.dpFormOpen">
                <div class="dp-form-bar">
                    <span class="dp-form-title">Advance Bill</span>
                    <input type="number" class="form-input dp-amount-input" placeholder="Amount"
                           t-att-value="state.dpAmount"
                           t-on-input="onDpAmountInput"/>
                    <button class="btn dp-pct-btn" t-on-click.stop="onDpSetPercent" data-pct="30">30%</button>
                    <button class="btn dp-pct-btn" t-on-click.stop="onDpSetPercent" data-pct="50">50%</button>
                    <button class="btn dp-pct-btn" t-on-click.stop="onDpSetPercent" data-pct="100">100%</button>
                    <input type="text" class="form-input dp-note-input" placeholder="Description (e.g. Advance, Deposit)"
                           t-att-value="state.dpNote"
                           t-on-input="onDpNoteInput"/>
                    <button class="btn btn-primary" t-on-click.stop="onSubmitDownPayment">Create</button>
                    <button class="btn" t-on-click.stop="onToggleDownPaymentForm">Cancel</button>
                    <span t-if="state.dpError" class="dp-error" t-esc="state.dpError"/>
                </div>
            </t>

            <t t-if="state.loading">
                <div class="loading">Loading…</div>
            </t>
            <t t-elif="state.error">
                <div class="error" t-esc="state.error"/>
            </t>
            <t t-else="">
                <div class="so-card">
                    <!-- Card header -->
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New RFQ'"/>
                        <div class="so-stat-btns">
                            <div class="so-stat-btn" t-on-click.stop="onOpenReceipts">
                                <span class="so-stat-num" t-esc="state.receiptCount"/>
                                <span class="so-stat-lbl">Receipts</span>
                            </div>
                            <div class="so-stat-btn" t-on-click.stop="onOpenBills">
                                <span class="so-stat-num" t-esc="state.billCount"/>
                                <span class="so-stat-lbl">Bills</span>
                            </div>
                        </div>
                    </div>

                    <!-- Two-column info grid -->
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Vendor</label>
                                <select class="form-input" data-field="partner_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.partners" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.partner_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Payment Terms</label>
                                <select class="form-input" data-field="payment_term_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.paymentTerms" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.payment_term_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Purchase Rep.</label>
                                <select class="form-input" data-field="user_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.users" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.user_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Order Date</label>
                                <DatePicker value="formatDate(state.record.date_order)"
                                            onSelect.bind="setDateOrder"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Expected Arrival</label>
                                <DatePicker value="formatDate(state.record.date_planned)"
                                            onSelect.bind="setDatePlanned"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Source</label>
                                <input class="form-input" data-field="origin"
                                       t-att-value="formatVal(state.record.origin)"/>
                            </div>
                        </div>
                    </div>

                    <!-- Tabs -->
                    <div class="so-tabs">
                        <span t-attf-class="so-tab{{state.activeTab === 'lines' ? ' active' : ''}}"
                              t-on-click.stop="setTabLines">Order Lines</span>
                        <span t-attf-class="so-tab{{state.activeTab === 'other' ? ' active' : ''}}"
                              t-on-click.stop="setTabOther">Other Info</span>
                    </div>

                    <!-- Order lines tab -->
                    <t t-if="state.activeTab === 'lines'">
                        <table class="so-lines-table">
                            <thead>
                                <tr>
                                    <th>Product</th>
                                    <th>Description</th>
                                    <th>Qty</th>
                                    <th>UoM</th>
                                    <th>Unit Price</th>
                                    <th>Disc.%</th>
                                    <th>Subtotal</th>
                                    <th></th>
                                </tr>
                            </thead>
                            <tbody>
                                <t t-foreach="state.lines" t-as="line" t-key="line._key">
                                    <tr>
                                        <td class="so-col-product">
                                            <select class="o2m-input"
                                                    data-line-field="product_id"
                                                    t-att-data-key="line._key">
                                                <option value="0">—</option>
                                                <t t-foreach="state.products" t-as="opt" t-key="opt.id">
                                                    <option t-att-value="opt.id"
                                                            t-att-selected="getM2oId(line.product_id) === opt.id ? true : undefined"
                                                            t-esc="opt.display"/>
                                                </t>
                                            </select>
                                        </td>
                                        <td class="so-col-desc">
                                            <input class="o2m-input"
                                                   data-line-field="name"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.name || ''"/>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="o2m-input" type="number" step="0.01"
                                                   data-line-field="product_qty"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.product_qty !== undefined ? line.product_qty : 1"/>
                                        </td>
                                        <td>
                                            <select class="o2m-input"
                                                    data-line-field="product_uom_id"
                                                    t-att-data-key="line._key">
                                                <option value="0">—</option>
                                                <t t-foreach="state.uoms" t-as="opt" t-key="opt.id">
                                                    <option t-att-value="opt.id"
                                                            t-att-selected="getM2oId(line.product_uom_id) === opt.id ? true : undefined"
                                                            t-esc="opt.display"/>
                                                </t>
                                            </select>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="o2m-input" type="number" step="0.01"
                                                   data-line-field="price_unit"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.price_unit !== undefined ? line.price_unit : 0"/>
                                        </td>
                                        <td class="so-col-num">
                                            <input class="o2m-input" type="number" step="0.01"
                                                   data-line-field="discount"
                                                   t-att-data-key="line._key"
                                                   t-att-value="line.discount !== undefined ? line.discount : 0"/>
                                        </td>
                                        <td class="so-col-num so-col-subtotal">
                                            <span t-esc="formatMoney(line.price_subtotal)"/>
                                        </td>
                                        <td class="so-col-del">
                                            <button class="btn btn-sm btn-danger"
                                                    data-del-line="1"
                                                    t-att-data-key="line._key">✕</button>
                                        </td>
                                    </tr>
                                </t>
                            </tbody>
                        </table>
                        <button class="btn so-add-line" data-add-line="1">+ Add a line</button>

                        <div class="so-footer">
                            <div class="so-notes-wrap">
                                <label class="so-notes-lbl">Notes / Terms</label>
                                <textarea class="so-notes-ta" data-field="note"><t t-esc="state.record.note || ''"/></textarea>
                            </div>
                            <div class="so-totals">
                                <div class="so-total-row">
                                    <span class="so-total-lbl">Untaxed Amount</span>
                                    <span class="so-total-val" t-esc="formatMoney(state.record.amount_untaxed)"/>
                                </div>
                                <div class="so-total-row">
                                    <span class="so-total-lbl">Taxes</span>
                                    <span class="so-total-val" t-esc="formatMoney(state.record.amount_tax)"/>
                                </div>
                                <div class="so-total-row so-total-grand">
                                    <span class="so-total-lbl">Total</span>
                                    <span class="so-total-val" t-esc="formatMoney(state.record.amount_total)"/>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- Other info tab -->
                    <t t-if="state.activeTab === 'other'">
                        <div class="so-other-tab">
                            <div class="so-field-row">
                                <label>Currency</label>
                                <select class="form-input" data-field="currency_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.currencies" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.currency_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label>Billing Status</label>
                                <input class="form-input" readonly="readonly"
                                       t-att-value="formatVal(state.record.invoice_status)"/>
                            </div>
                        </div>
                    </t>
                </div>

                <!-- Supplier quotes, order confirmations -->
                <t t-if="!state.isNew">
                    <AttachmentPanel model="'purchase.order'"
                                     recordId="props.recordId"
                                     title="'Documents'"/>
                </t>

                <!-- Chatter -->
                <t t-if="!state.isNew">
                    <ChatterPanel model="'purchase.order'"
                                  recordId="props.recordId"
                                  refreshKey="state.chatRefreshKey"/>
                </t>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading:        true,
            error:          '',
            isNew:          !this.props.recordId,
            record:         {},
            activeTab:      'lines',
            lines:          [],
            deletedLineIds: [],
            partners:       [],
            paymentTerms:   [],
            users:          [],
            currencies:     [],
            products:       [],
            uoms:           [],
            receiptCount:     0,
            billCount:        0,
            dpFormOpen:       false,
            dpAmount:         '',
            dpNote:           'Down Payment',
            chatRefreshKey:   0,
        });
        this._nextKey = 1;
        onMounted(() => this.load());
    }

    get showConfirm() {
        const s = this.state.record.state;
        return this.state.isNew || s === 'draft';
    }

    get showBillActions() {
        return !this.state.isNew &&
               (this.state.record.state === 'purchase' || this.state.record.state === 'done') &&
               this.state.record.invoice_status !== 'billed';
    }

    get showCancelBtn() {
        return !this.state.isNew && this.state.record.state !== 'cancel';
    }

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const fields = [
                'name', 'state', 'partner_id', 'date_order', 'date_planned',
                'payment_term_id', 'currency_id', 'user_id', 'origin',
                'invoice_status', 'note', 'amount_untaxed', 'amount_tax', 'amount_total',
            ];
            const [recordData] = await Promise.all([
                this.props.recordId
                    ? RpcService.call('purchase.order', 'read', [[this.props.recordId]], { fields })
                          .then(r => (Array.isArray(r) ? r[0] : r) || {})
                    : Promise.resolve({}),
                this.loadOpts('res.partner',          'partners',     ['id', 'name']),
                this.loadOpts('account.payment.term', 'paymentTerms', ['id', 'name']),
                this.loadOpts('res.users',            'users',        ['id', 'name']),
                this.loadOpts('res.currency',         'currencies',   ['id', 'name']),
                this.loadOpts('product.product',      'products',     ['id', 'name']),
                this.loadOpts('uom.uom',              'uoms',         ['id', 'name']),
            ]);
            this.state.record = recordData;
            // Default currency for new records
            if (!this.props.recordId && !this.state.record.currency_id && this.state.currencies.length > 0) {
                this.state.record.currency_id = this.state.currencies[0].id;
            }
            if (this.props.recordId) {
                await this.loadLines();
                try {
                    const picks = await RpcService.call('stock.picking', 'search_read',
                        [[['purchase_id', '=', this.props.recordId]]],
                        { fields: ['id'], limit: 500 });
                    this.state.receiptCount = Array.isArray(picks) ? picks.length : 0;
                } catch (_) {}
                try {
                    const bills = await RpcService.call('account.move', 'search_read',
                        [[['purchase_id', '=', this.props.recordId], ['move_type', '=', 'in_invoice']]],
                        { fields: ['id'], limit: 500 });
                    this.state.billCount = Array.isArray(bills) ? bills.length : 0;
                } catch (_) {}
            }
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async loadOpts(model, key, fields) {
        try {
            const recs = await RpcService.call(model, 'search_read', [[]], { fields, limit: 500 });
            this.state[key] = (Array.isArray(recs) ? recs : []).map(r => ({
                id:      r.id,
                display: r.name || String(r.id),
            }));
        } catch (_) { this.state[key] = []; }
    }

    async loadLines() {
        try {
            const lineFields = [
                'id', 'product_id', 'name', 'product_qty', 'product_uom_id',
                'price_unit', 'discount', 'price_subtotal',
            ];
            const rows = await RpcService.call('purchase.order.line', 'search_read',
                [[['order_id', '=', this.props.recordId]]],
                { fields: lineFields, limit: 500 });
            this.state.lines = (Array.isArray(rows) ? rows : []).map(r => ({
                _key: String(this._nextKey++), ...r,
            }));
        } catch (_) { this.state.lines = []; }
    }

    getM2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        if (typeof val === 'string') return parseInt(val) || 0;
        return 0;
    }

    formatVal(val) {
        if (val === null || val === undefined || val === false) return '';
        if (Array.isArray(val)) return val[1] ?? '';
        return String(val);
    }

    formatDate(val) {
        if (!val || val === false) return '';
        return String(val).substring(0, 10);
    }

    formatMoney(val) {
        const n = parseFloat(val) || 0;
        return n.toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    }

    recalcLine(line) {
        const qty   = parseFloat(line.product_qty) || 0;
        const price = parseFloat(line.price_unit)  || 0;
        const disc  = parseFloat(line.discount)    || 0;
        line.price_subtotal = Math.round(qty * price * (1 - disc / 100) * 100) / 100;
    }

    stepClass(step) {
        const order = { draft: 0, purchase: 1, done: 2 };
        const stateMap = { draft: 0, purchase: 1, done: 2, cancel: -1 };
        const cur = stateMap[this.state.record.state] ?? -1;
        const s   = order[step];
        if (s === cur) return ' active';
        if (cur > 0 && s < cur) return ' done';
        return '';
    }

    setTabLines() { this.state.activeTab = 'lines'; }
    setTabOther()  { this.state.activeTab = 'other'; }

    onAnyChange(e) {
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            const val = e.target.tagName === 'SELECT'
                ? (parseInt(e.target.value) || 0)
                : e.target.value;
            this.updateLine(e.target.dataset.key, lineField, val);
            return;
        }
        const field = e.target.dataset.field;
        if (field && e.target.tagName === 'SELECT') {
            this.state.record[field] = parseInt(e.target.value) || 0;
        }
    }

    onAnyInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const lineField = e.target.dataset.lineField;
        if (lineField) { this.updateLine(e.target.dataset.key, lineField, e.target.value); return; }
        const field = e.target.dataset.field;
        if (field) this.state.record[field] = e.target.value;
    }

    onAnyClick(e) {
        if (e.target.dataset.addLine) {
            e.preventDefault();
            this.state.lines.push({
                _key: String(this._nextKey++), id: null,
                product_id: 0, name: '', product_qty: 1,
                product_uom_id: 0, price_unit: 0, discount: 0, price_subtotal: 0,
            });
            return;
        }
        if (e.target.dataset.delLine) {
            e.preventDefault();
            const key  = e.target.dataset.key;
            const line = this.state.lines.find(l => l._key === key);
            if (line && line.id) this.state.deletedLineIds.push(line.id);
            this.state.lines = this.state.lines.filter(l => l._key !== key);
        }
    }

    updateLine(key, field, val) {
        const line = this.state.lines.find(l => l._key === key);
        if (!line) return;
        line[field] = val;
        if (field === 'product_id' && val > 0) {
            this.triggerLineOnchange(key, field);
        } else if (field === 'product_qty' || field === 'price_unit' || field === 'discount') {
            this.recalcLine(line);
        }
    }

    async triggerLineOnchange(key, changedField) {
        const line = this.state.lines.find(l => l._key === key);
        if (!line) return;
        try {
            const res = await RpcService.call(
                'purchase.order.line', 'onchange', [line, changedField, {}], {});
            if (res && res.value) {
                Object.assign(line, res.value);
                if (changedField === 'product_id') {
                    if (!line.product_qty) line.product_qty = 1;
                    this.recalcLine(line);
                }
            }
        } catch (_) {}
    }

    collectRecord() {
        const r = this.state.record;
        return {
            partner_id:      this.getM2oId(r.partner_id),
            payment_term_id: this.getM2oId(r.payment_term_id),
            user_id:         this.getM2oId(r.user_id),
            currency_id:     this.getM2oId(r.currency_id) || false,
            date_order:      r.date_order   || false,
            date_planned:    r.date_planned || false,
            origin:          r.origin       || false,
            note:            r.note         || false,
        };
    }

    async syncLines(parentId) {
        if (this.state.deletedLineIds.length) {
            await RpcService.call('purchase.order.line', 'unlink', [this.state.deletedLineIds], {});
            this.state.deletedLineIds = [];
        }
        for (const line of this.state.lines) {
            const vals = {
                order_id:       parentId,
                product_id:     this.getM2oId(line.product_id),
                name:           line.name           || '',
                product_qty:    parseFloat(line.product_qty)    || 1,
                product_uom_id: this.getM2oId(line.product_uom_id) || false,
                price_unit:     parseFloat(line.price_unit)     || 0,
                discount:       parseFloat(line.discount)        || 0,
            };
            if (!line.id) {
                await RpcService.call('purchase.order.line', 'create', [vals], {});
            } else {
                await RpcService.call('purchase.order.line', 'write', [[line.id], vals], {});
            }
        }
    }

    async onSave() {
        try {
            await RpcService.call('purchase.order', 'write',
                [[this.state.record.id], this.collectRecord()], {});
            await this.syncLines(this.state.record.id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCreate() {
        try {
            const newId = await RpcService.call('purchase.order', 'create',
                [this.collectRecord()], {});
            await this.syncLines(newId);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onDelete() {
        try {
            await RpcService.call('purchase.order', 'unlink', [[this.state.record.id]], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onConfirm() {
        try {
            let id = this.state.record.id;
            if (!id) {
                id = await RpcService.call('purchase.order', 'create', [this.collectRecord()], {});
                await this.syncLines(id);
            } else {
                await RpcService.call('purchase.order', 'write', [[id], this.collectRecord()], {});
                await this.syncLines(id);
            }
            await RpcService.call('purchase.order', 'action_confirm', [[id]], {});
            this.state.chatRefreshKey++;
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCancel() {
        try {
            await RpcService.call('purchase.order', 'action_cancel', [[this.state.record.id]], {});
            this.state.chatRefreshKey++;
            this.state.record.state = 'cancel';
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }

    setDateOrder(v)   { this.state.record.date_order   = v; }
    setDatePlanned(v) { this.state.record.date_planned = v; }

    onPrint() { window.open('/report/pdf/purchase.order/' + this.state.record.id, '_blank'); }

    async onCreateBill() {
        try {
            await RpcService.call('purchase.order', 'action_create_bills',
                [[this.state.record.id]], {});
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    onToggleDownPaymentForm() {
        this.state.dpFormOpen = !this.state.dpFormOpen;
        this.state.dpAmount   = '';
        this.state.dpNote     = 'Down Payment';
        this.state.dpError    = '';
    }

    onDpAmountInput(ev) { this.state.dpAmount = ev.target.value; }
    onDpNoteInput(ev)   { this.state.dpNote   = ev.target.value; }

    onDpSetPercent(ev) {
        const pct   = parseFloat(ev.currentTarget.dataset.pct);
        const total = parseFloat(this.state.record.amount_total) || 0;
        this.state.dpAmount = ((pct / 100) * total).toFixed(2);
    }

    async onSubmitDownPayment() {
        const amount = parseFloat(this.state.dpAmount);
        if (!amount || amount <= 0) { this.state.dpError = 'Enter a valid amount.'; return; }
        try {
            await RpcService.call('purchase.order', 'action_create_down_payment',
                [[this.state.record.id]], { amount, note: this.state.dpNote });
            this.state.dpFormOpen = false;
            await this.load();
        } catch (e) { this.state.dpError = e.message; }
    }

    onOpenReceipts() {
        if (!this.props.recordId) return;
        this.props.onNavigate?.('stock.picking',
            [['purchase_id', '=', this.props.recordId]]);
    }

    onOpenBills() {
        if (!this.props.recordId) return;
        this.props.onNavigate?.('account.move',
            [['purchase_id', '=', this.props.recordId], ['move_type', '=', 'in_invoice']]);
    }
}

// ----------------------------------------------------------------
// TransferFormView — stock.picking detail (Odoo 14-style)
// ----------------------------------------------------------------
class TransferFormView extends Component {
    static components = { DatePicker, ChatterPanel, AttachmentPanel };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput">

            <!-- Page header -->
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Transfers</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New Transfer'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="isNew">
                            <button class="btn btn-primary" t-on-click.stop="onCreate">Create</button>
                        </t>
                        <t t-elif="canEdit">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                        </t>
                        <t t-if="!isNew">
                            <button class="btn" t-on-click.stop="onPrint">Print</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Back</button>
                    </div>
                </div>
            </div>

            <!-- Status bar with workflow buttons -->
            <div class="so-statusbar">
                <div class="so-sb-left">
                    <t t-if="isDraft">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onConfirm">Confirm</button>
                    </t>
                    <t t-if="isConfirmed">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onCheckAvailability">Check Availability</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onCancel">Cancel</button>
                    </t>
                    <t t-if="isAssigned">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onValidate">Validate</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onUnreserve">Unreserve</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onCancel">Cancel</button>
                    </t>
                    <t t-if="isCancelled">
                        <button class="btn ghost so-wf-btn" t-on-click.stop="onResetDraft">Reset to Draft</button>
                    </t>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Draft</div>
                    <div t-attf-class="so-step{{stepClass('confirmed')}}">Waiting</div>
                    <div t-attf-class="so-step{{stepClass('assigned')}}">Ready</div>
                    <div t-attf-class="so-step{{stepClass('done')}}">Done</div>
                </div>
            </div>

            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <!-- Card title + source badge -->
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New Transfer'"/>
                        <div class="so-stat-btns">
                            <t t-if="state.record.origin">
                                <div class="so-stat-btn">
                                    <span class="so-stat-num" t-esc="state.record.origin"/>
                                    <span class="so-stat-lbl">Source</span>
                                </div>
                            </t>
                        </div>
                    </div>

                    <!-- Two-column info grid -->
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Delivery Address</label>
                                <select class="form-input" data-field="partner_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.partners" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.partner_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Source Location</label>
                                <t t-if="isDraft">
                                    <select class="form-input" data-field="location_id">
                                        <t t-foreach="state.locations" t-as="loc" t-key="loc.id">
                                            <option t-att-value="loc.id"
                                                    t-att-selected="getM2oId(state.record.location_id) === loc.id ? true : undefined"
                                                    t-esc="loc.display"/>
                                        </t>
                                    </select>
                                </t>
                                <t t-else="">
                                    <span class="so-field-val" t-esc="locName(state.record.location_id)"/>
                                </t>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Destination</label>
                                <t t-if="isDraft">
                                    <select class="form-input" data-field="location_dest_id">
                                        <t t-foreach="state.locations" t-as="loc" t-key="loc.id">
                                            <option t-att-value="loc.id"
                                                    t-att-selected="getM2oId(state.record.location_dest_id) === loc.id ? true : undefined"
                                                    t-esc="loc.display"/>
                                        </t>
                                    </select>
                                </t>
                                <t t-else="">
                                    <span class="so-field-val" t-esc="locName(state.record.location_dest_id)"/>
                                </t>
                            </div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Scheduled Date</label>
                                <DatePicker value="formatDate(state.record.scheduled_date)"
                                            onSelect.bind="setScheduledDate"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Source Document</label>
                                <input class="form-input" type="text" readonly="1"
                                       t-att-value="state.record.origin || ''"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Operation Type</label>
                                <t t-if="isNew">
                                    <select class="form-input" data-field="picking_type_id">
                                        <option value="0">—</option>
                                        <t t-foreach="state.pickingTypes" t-as="pt" t-key="pt.id">
                                            <option t-att-value="pt.id"
                                                    t-att-selected="getM2oId(state.record.picking_type_id) === pt.id ? true : undefined"
                                                    t-esc="pt.display"/>
                                        </t>
                                    </select>
                                </t>
                                <t t-else="">
                                    <span class="so-field-val" t-esc="pickingTypeName"/>
                                </t>
                            </div>
                        </div>
                    </div>

                    <!-- Tabs -->
                    <div class="so-tabs">
                        <div t-attf-class="so-tab{{state.activeTab === 'operations' ? ' active' : ''}}"
                             t-on-click.stop="setTabOperations">Operations</div>
                        <div t-attf-class="so-tab{{state.activeTab === 'info' ? ' active' : ''}}"
                             t-on-click.stop="setTabInfo">Additional Info</div>
                    </div>

                    <!-- Operations tab: stock.move table -->
                    <t t-if="state.activeTab === 'operations'">
                        <table class="so-lines-table">
                            <thead>
                                <tr>
                                    <th>Product</th>
                                    <th>From</th>
                                    <th>To</th>
                                    <th class="text-right">Demand</th>
                                    <th class="text-right">Done</th>
                                    <th>UoM</th>
                                    <th t-if="anyTracked">Lot/Serial</th>
                                    <th t-if="anyPacked">Package</th>
                                </tr>
                            </thead>
                            <tbody>
                                <t t-if="state.moves.length === 0">
                                    <tr><td colspan="8" class="trn-empty-row">No operations.</td></tr>
                                </t>
                                <t t-foreach="state.moves" t-as="mv" t-key="mv._key">
                                    <tr>
                                        <td t-esc="prodName(mv.product_id)"/>
                                        <td t-esc="locName(mv.location_id)"/>
                                        <td t-esc="locName(mv.location_dest_id)"/>
                                        <td class="text-right" t-esc="fmtQty(mv.product_uom_qty)"/>
                                        <td class="text-right">
                                            <t t-if="isAssigned">
                                                <input class="inv-line-input text-right"
                                                       type="number" step="0.01" min="0"
                                                       t-att-data-move-key="mv._key"
                                                       data-move-field="quantity"
                                                       t-att-value="mv.quantity"/>
                                            </t>
                                            <t t-else="">
                                                <span t-esc="fmtQty(mv.quantity)"/>
                                            </t>
                                        </td>
                                        <td t-esc="uomName(mv.product_uom_id)"/>
                                        <td t-if="anyTracked">
                                            <t t-if="isTracked(mv)">
                                                <select class="inv-line-input"
                                                        t-att-data-move-key="mv._key" data-move-field="lot_id"
                                                        t-att-disabled="isDone ? true : undefined">
                                                    <option value="0">— select —</option>
                                                    <t t-foreach="lotsFor(mv)" t-as="lot" t-key="lot.id">
                                                        <option t-att-value="lot.id"
                                                                t-att-selected="getM2oId(mv.lot_id) === lot.id ? true : undefined"
                                                                t-esc="lot.name"/>
                                                    </t>
                                                </select>
                                                <span t-if="!isDone" class="trn-lot-add" title="Create a new lot/serial"
                                                      t-on-click.stop="() => this.onNewLot(mv._key)">＋</span>
                                            </t>
                                            <span t-else="" class="trn-muted">—</span>
                                        </td>
                                        <td t-if="anyPacked">
                                            <span t-if="mv.result_package_id" class="trn-pack-tag"
                                                  t-esc="m2oName(mv.result_package_id)"/>
                                            <span t-else="" class="trn-muted">—</span>
                                        </td>
                                    </tr>
                                </t>
                            </tbody>
                        </table>
                        <!-- Put in Pack: group the unpacked operations into a parcel -->
                        <div class="trn-put-in-pack-row">
                            <button t-if="canPack" class="btn so-wf-btn" t-on-click.stop="onPutInPack"
                                    t-att-disabled="state.packing"
                                    title="Group the remaining operations into a numbered package">
                                <t t-if="state.packing">Packing…</t><t t-else="">Put in Pack</t>
                            </button>
                            <button t-else="" class="btn so-wf-btn so-stat-btn-disabled"
                                    t-att-title="packDisabledReason">Put in Pack</button>
                            <span t-if="state.packMsg" class="trn-pack-msg" t-esc="state.packMsg"/>
                        </div>
                    </t>

                    <!-- Additional Info tab -->
                    <t t-if="state.activeTab === 'info'">
                        <div class="so-info-grid" style="margin-top:12px;">
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Responsible</label>
                                    <select class="form-input" data-field="user_id">
                                        <option value="0">—</option>
                                        <t t-foreach="state.users" t-as="usr" t-key="usr.id">
                                            <option t-att-value="usr.id"
                                                    t-att-selected="getM2oId(state.record.user_id) === usr.id ? true : undefined"
                                                    t-esc="usr.display"/>
                                        </t>
                                    </select>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Company</label>
                                    <span class="so-field-val" t-esc="state.companyName || '—'"/>
                                </div>
                            </div>
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Lot/Serial tracking</label>
                                    <span class="so-field-val" t-esc="trackingSummary"/>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Packages</label>
                                    <span class="so-field-val" t-esc="packageSummary"/>
                                </div>
                            </div>
                        </div>
                    </t>
                </div>

                <!-- Delivery notes, signed PODs, packing lists -->
                <t t-if="!isNew">
                    <AttachmentPanel model="'stock.picking'"
                                     recordId="props.recordId"
                                     title="'Documents'"/>
                </t>

                <!-- Chatter / audit log -->
                <t t-if="!isNew">
                    <ChatterPanel model="'stock.picking'"
                                  recordId="props.recordId"
                                  refreshKey="state.chatRefreshKey"/>
                </t>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading:     true,
            error:       null,
            record:      {},
            moves:       [],
            partners:    [],
            users:          [],
            locations:      [],
            pickingTypes:   [],
            companyName:    '',
            activeTab:      'operations',
            chatRefreshKey: 0,
            // Lots for the tracked products on this transfer, keyed by product id
            lotsByProduct:  {},
            packing:        false,
            packMsg:        '',
        });
        this._locMap  = {};
        this._prodMap = {};
        this._uomMap  = {};
        this._pickingTypeName = '';
        this._nextMoveKey = 1;
        onMounted(() => this.load());
    }

    async load() {
        this.state.loading = true;
        this.state.error   = null;
        try {
            if (this.props.recordId) {
                const recs = await RpcService.call('stock.picking', 'read',
                    [[this.props.recordId]],
                    { fields: ['name','state','partner_id','location_id','location_dest_id',
                               'scheduled_date','origin','picking_type_id','sale_id','purchase_id',
                               'company_id','user_id'] });
                if (!recs || recs.length === 0) throw new Error('Transfer not found');
                this.state.record = recs[0];
            } else {
                // New transfer: no record to read (reading a null id 500'd — the
                // "Internal Error" on Operations → New). Start a draft and let the
                // user pick an Operation Type, which supplies the locations.
                this.state.record = { state: 'draft' };
                const types = await RpcService.call('stock.picking.type', 'search_read',
                    [[]], { fields: ['id','name','default_location_src_id','default_location_dest_id'], limit: 100 });
                this.state.pickingTypes = (Array.isArray(types) ? types : []).map(t => ({
                    id: t.id, display: t.name,
                    src:  this.getM2oId(t.default_location_src_id),
                    dest: this.getM2oId(t.default_location_dest_id),
                }));
            }

            // Partners, users, and locations in parallel. Each is wrapped so a
            // single failing lookup (e.g. an 'active' column that isn't
            // registered as filterable on that model) yields an empty dropdown
            // instead of breaking the whole form with an "internal error".
            const safe = pr => pr.catch(() => []);
            const [parts, users, locs] = await Promise.all([
                safe(RpcService.call('res.partner', 'search_read',
                    [[]], { fields: ['id','name'], limit: 200 })),
                safe(RpcService.call('res.users', 'search_read',
                    [[]], { fields: ['id','name'], limit: 200 })),
                safe(RpcService.call('stock.location', 'search_read',
                    [[]], { fields: ['id','name','complete_name'], limit: 500 })),
            ]);
            this.state.partners  = (Array.isArray(parts) ? parts : []).map(p => ({ id: p.id, display: p.name }));
            this.state.users     = (Array.isArray(users) ? users : []).map(u => ({ id: u.id, display: u.name }));
            this.state.locations = (Array.isArray(locs)  ? locs  : []).map(l => ({ id: l.id, display: l.complete_name || l.name }));

            // Company name
            const companyId = this.getM2oId(this.state.record.company_id);
            if (companyId) {
                try {
                    const co = await RpcService.call('res.company', 'read', [[companyId]], { fields: ['id','name'] });
                    this.state.companyName = (co && co.length > 0) ? co[0].name : '';
                } catch (_) {}
            }

            // Picking type name
            const ptId = this.getM2oId(this.state.record.picking_type_id);
            if (ptId) {
                try {
                    const pt = await RpcService.call('stock.picking.type', 'read', [[ptId]], { fields: ['id','name'] });
                    this._pickingTypeName = pt && pt.length > 0 ? pt[0].name : '';
                } catch (_) {}
            }

            if (this.props.recordId) {
                await this.loadMoves();
                // Load location names for display in non-draft mode
                await this.loadLocNames([
                    this.getM2oId(this.state.record.location_id),
                    this.getM2oId(this.state.record.location_dest_id),
                ]);
            }
        } catch (e) {
            this.state.error = e.message || 'Failed to load transfer';
        } finally {
            this.state.loading = false;
        }
    }

    async loadMoves() {
        try {
            const moves = await RpcService.call('stock.move', 'search_read',
                [[['picking_id','=',this.props.recordId]]],
                { fields: ['id','product_id','location_id','location_dest_id','product_uom_id',
                           'name','product_uom_qty','quantity','state',
                           'lot_id','tracking','result_package_id'], limit: 200 });
            const arr = Array.isArray(moves) ? moves : [];

            const prodIds = [...new Set(arr.map(m => this.getM2oId(m.product_id)).filter(Boolean))];
            const uomIds  = [...new Set(arr.map(m => this.getM2oId(m.product_uom_id)).filter(Boolean))];
            const locIds  = [...new Set(arr.flatMap(m => [
                this.getM2oId(m.location_id), this.getM2oId(m.location_dest_id)
            ]).filter(Boolean))];

            if (prodIds.length > 0) {
                const prods = await RpcService.call('product.product', 'read',
                    [prodIds], { fields: ['id','name'] });
                for (const p of (Array.isArray(prods) ? prods : [])) this._prodMap[p.id] = p.name;
            }
            if (uomIds.length > 0) {
                const uoms = await RpcService.call('uom.uom', 'read',
                    [uomIds], { fields: ['id','name'] });
                for (const u of (Array.isArray(uoms) ? uoms : [])) this._uomMap[u.id] = u.name;
            }
            if (locIds.length > 0) {
                await this.loadLocNames(locIds);
            }

            this.state.moves = arr.map(m => ({
                _key: String(this._nextMoveKey++), ...m,
            }));
            await this.loadLots();
        } catch (_) {
            this.state.moves = [];
        }
    }

    // Lots are only fetched for the products that are actually tracked — an
    // untracked transfer makes no extra call.
    async loadLots() {
        const tracked = [...new Set(this.state.moves
            .filter(m => m.tracking === 'lot' || m.tracking === 'serial')
            .map(m => this.getM2oId(m.product_id))
            .filter(Boolean))];
        if (!tracked.length) { this.state.lotsByProduct = {}; return; }
        try {
            const byProd = {};
            for (const pid of tracked) {
                const lots = await RpcService.call('stock.production.lot', 'search_read',
                    [[['product_id','=',pid]]], { fields: ['id','name'], limit: 300 });
                byProd[pid] = (Array.isArray(lots) ? lots : []).map(l => ({ id: l.id, name: l.name }));
            }
            this.state.lotsByProduct = byProd;
        } catch (_) { this.state.lotsByProduct = {}; }
    }

    async loadLocNames(ids) {
        const validIds = ids.filter(Boolean);
        if (validIds.length === 0) return;
        try {
            const locs = await RpcService.call('stock.location', 'read',
                [validIds], { fields: ['id', 'name', 'complete_name'] });
            for (const l of (Array.isArray(locs) ? locs : []))
                this._locMap[l.id] = l.complete_name || l.name;
        } catch (_) {}
    }

    // ---- Getters ----
    get isNew()       { return !this.props.recordId; }
    get isDraft()     { return this.state.record.state === 'draft'; }
    get isConfirmed() { return this.state.record.state === 'confirmed'; }
    get isAssigned()  { return this.state.record.state === 'assigned'; }
    get isDone()      { return this.state.record.state === 'done'; }
    get isCancelled() { return this.state.record.state === 'cancel'; }
    get canEdit()     { return !this.isDone && !this.state.loading; }

    get pickingTypeName() { return this._pickingTypeName || '—'; }

    // ---- Lots & packages -------------------------------------------
    // The backend already enforces lots on tracked products at validation
    // (a tracked move without a lot is refused); these getters are what let
    // the operator set one before they hit Validate rather than after.
    isTracked(mv)  { return mv.tracking === 'lot' || mv.tracking === 'serial'; }
    get anyTracked() { return this.state.moves.some(m => this.isTracked(m)); }
    get anyPacked()  { return this.state.moves.some(m => !!m.result_package_id); }
    lotsFor(mv)    { return this.state.lotsByProduct[this.getM2oId(mv.product_id)] || []; }
    m2oName(v)     { return Array.isArray(v) && v.length > 1 ? v[1] : ''; }

    get trackingSummary() {
        const tracked = this.state.moves.filter(m => this.isTracked(m));
        if (!tracked.length) return 'No tracked products on this transfer';
        const missing = tracked.filter(m => !this.getM2oId(m.lot_id)).length;
        return missing
            ? `${tracked.length} tracked line(s) — ${missing} still needs a lot/serial`
            : `${tracked.length} tracked line(s), all assigned`;
    }
    get packageSummary() {
        const names = [...new Set(this.state.moves
            .filter(m => m.result_package_id)
            .map(m => this.m2oName(m.result_package_id)))];
        if (!names.length) return 'Not packed';
        const loose = this.state.moves.filter(m => !m.result_package_id).length;
        return names.join(', ') + (loose ? ` (${loose} line(s) still loose)` : '');
    }

    get canPack() {
        return !this.isNew && !this.isDone && !this.isCancelled
               && this.state.moves.some(m => !m.result_package_id);
    }
    get packDisabledReason() {
        if (this.isNew)       return 'Save the transfer first';
        if (this.isDone)      return 'This transfer is already done';
        if (this.isCancelled) return 'This transfer is cancelled';
        if (!this.state.moves.length) return 'Add an operation first';
        return 'Everything on this transfer is already packed';
    }

    stepClass(step) {
        const order = { draft: 0, confirmed: 1, assigned: 2, done: 3 };
        const cur = this.state.record.state === 'cancel'
            ? -1 : (order[this.state.record.state] ?? 0);
        const s = order[step] ?? 0;
        if (s === cur) return ' active';
        if (cur >= 0 && s < cur) return ' done';
        return '';
    }

    getM2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        if (typeof val === 'string') return parseInt(val) || 0;
        return 0;
    }
    locName(val)  { const id = this.getM2oId(val); return id ? (this._locMap[id]  || String(id)) : '—'; }
    prodName(val) { const id = this.getM2oId(val); return id ? (this._prodMap[id] || String(id)) : '—'; }
    uomName(val)  { const id = this.getM2oId(val); return id ? (this._uomMap[id]  || '')         : ''; }
    fmtQty(val)   { const n = parseFloat(val) || 0; return n % 1 === 0 ? String(n) : n.toFixed(2); }
    formatDate(val) { if (!val || val === false) return ''; return String(val).substring(0, 10); }

    // ---- Event handlers ----
    onAnyChange(e) {
        // A per-line <select> (the lot picker) is a change event, not an input
        // one, so it has to be routed here as well as in onAnyInput.
        const moveKey   = e.target.dataset.moveKey;
        const moveField = e.target.dataset.moveField;
        if (moveKey && moveField && e.target.tagName === 'SELECT') {
            const mv = this.state.moves.find(m => m._key === moveKey);
            if (mv) {
                mv[moveField] = parseInt(e.target.value) || 0;
                if (moveField === 'lot_id') this.saveLot(mv);
            }
            return;
        }
        const field = e.target.dataset.field;
        if (field && e.target.tagName === 'SELECT') {
            this.state.record[field] = parseInt(e.target.value) || 0;
        }
    }

    // Written straight through: the lot is what Validate will check, and a
    // half-saved transfer that looks assigned but is not is worse than a
    // round trip per pick.
    async saveLot(mv) {
        if (!mv.id) return;
        try {
            await RpcService.call('stock.move', 'write',
                [[mv.id], { lot_id: this.getM2oId(mv.lot_id) || false }], {});
        } catch (e) {
            alert('Could not set the lot/serial: ' + (e.message || e));
            await this.loadMoves();
        }
    }

    async onNewLot(moveKey) {
        const mv = this.state.moves.find(m => m._key === moveKey);
        if (!mv) return;
        const pid = this.getM2oId(mv.product_id);
        const name = prompt(`New lot/serial number for ${this.prodName(mv.product_id)}:`);
        if (!name) return;
        try {
            const id = await RpcService.call('stock.production.lot', 'create',
                [{ name, product_id: pid }], {});
            mv.lot_id = [id, name];
            await this.saveLot(mv);
            await this.loadLots();
        } catch (e) {
            alert('Could not create the lot: ' + (e.message || e));
        }
    }

    async onPutInPack() {
        this.state.packing = true;
        this.state.packMsg = '';
        try {
            const res = await RpcService.call('stock.quant.package', 'put_in_pack',
                [{ picking_id: this.props.recordId }], {});
            this.state.packMsg = res && res.name
                ? `Packed ${res.moves} operation(s) into ${res.name}` : 'Packed';
            await this.loadMoves();
        } catch (e) {
            this.state.packMsg = '';
            alert(e.message || e);
        } finally {
            this.state.packing = false;
        }
    }

    onAnyInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const moveKey   = e.target.dataset.moveKey;
        const moveField = e.target.dataset.moveField;
        if (moveKey && moveField) {
            const mv = this.state.moves.find(m => m._key === moveKey);
            if (mv) mv[moveField] = parseFloat(e.target.value) || 0;
            return;
        }
        const field = e.target.dataset.field;
        if (field) this.state.record[field] = e.target.value;
    }

    setTabOperations() { this.state.activeTab = 'operations'; }
    setTabInfo()       { this.state.activeTab = 'info'; }
    setScheduledDate(v){ this.state.record.scheduled_date = v; }

    // ---- Actions ----
    async onSave() {
        try {
            const vals = {
                partner_id:     this.getM2oId(this.state.record.partner_id) || false,
                scheduled_date: this.state.record.scheduled_date || false,
                user_id:        this.getM2oId(this.state.record.user_id)    || false,
            };
            if (this.isDraft) {
                vals.location_id      = this.getM2oId(this.state.record.location_id)      || false;
                vals.location_dest_id = this.getM2oId(this.state.record.location_dest_id) || false;
            }
            await RpcService.call('stock.picking', 'write', [[this.props.recordId], vals]);
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }

    // Create a new draft transfer. The Operation Type supplies the source and
    // destination locations (both required), so it is the one mandatory choice.
    async onCreate() {
        const ptId = this.getM2oId(this.state.record.picking_type_id);
        const pt   = this.state.pickingTypes.find(t => t.id === ptId);
        if (!pt) { alert('Please choose an Operation Type first.'); return; }
        try {
            const vals = {
                picking_type_id: ptId,
                location_id:      this.getM2oId(this.state.record.location_id)      || pt.src  || false,
                location_dest_id: this.getM2oId(this.state.record.location_dest_id) || pt.dest || false,
                partner_id:       this.getM2oId(this.state.record.partner_id) || false,
                scheduled_date:   this.state.record.scheduled_date || false,
                user_id:          this.getM2oId(this.state.record.user_id)    || false,
                state:            'draft',
            };
            await RpcService.call('stock.picking', 'create', [vals]);
            this.props.onBack();   // back to the list, where the new draft appears
        } catch (e) { alert('Create failed: ' + (e.message || e)); }
    }

    async onConfirm() {
        await this.onSave();
        try {
            await RpcService.call('stock.picking', 'action_confirm', [[this.props.recordId]]);
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) { alert('Confirm failed: ' + (e.message || e)); }
    }

    async onCheckAvailability() {
        try {
            await RpcService.call('stock.picking', 'action_assign', [[this.props.recordId]]);
            await this.load();
        } catch (e) { alert('Check availability failed: ' + (e.message || e)); }
    }

    async onValidate() {
        try {
            for (const mv of this.state.moves) {
                if (mv.id) {
                    await RpcService.call('stock.move', 'write',
                        [[mv.id], { quantity: parseFloat(mv.quantity) || 0 }]);
                }
            }
            await RpcService.call('stock.picking', 'button_validate', [[this.props.recordId]]);
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) { alert('Validate failed: ' + (e.message || e)); }
    }

    async onUnreserve() {
        try {
            await RpcService.call('stock.picking', 'button_unreserve', [[this.props.recordId]]);
            await this.load();
        } catch (e) { alert('Unreserve failed: ' + (e.message || e)); }
    }

    async onCancel() {
        if (!confirm('Cancel this transfer?')) return;
        try {
            await RpcService.call('stock.picking', 'action_cancel', [[this.props.recordId]]);
            this.state.chatRefreshKey++;
            await this.load();
        } catch (e) { alert('Cancel failed: ' + (e.message || e)); }
    }

    async onResetDraft() {
        try {
            await RpcService.call('stock.picking', 'button_reset_to_draft', [[this.props.recordId]]);
            await this.load();
        } catch (e) { alert('Reset to draft failed: ' + (e.message || e)); }
    }

    onBack() { this.props.onBack(); }

    onPrint() { window.open('/report/pdf/stock.picking/' + this.state.record.id, '_blank'); }
}

// ----------------------------------------------------------------
// ProductFormView — product.product detail (Odoo 17-style)
// ----------------------------------------------------------------
class ProductFormView extends Component {
    static components = { ChatterPanel, AttachmentPanel };
    static template = xml`
        <div class="so-shell"
             t-on-change="onFormChange"
             t-on-input="onFormInput">

            <!-- Page header: breadcrumb + Save/Discard + secondary btns -->
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Products</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New'"/>
                    </div>
                    <div class="so-action-btns">
                        <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                        <button class="btn"             t-on-click.stop="onDiscard">Discard</button>
                    </div>
                </div>
                <!-- Secondary action buttons (inventory operations) -->
                <div class="prd-secondary-btns">
                    <t t-if="stockTracked">
                        <button class="btn" t-on-click.stop="onUpdateQuantity"
                                title="Count the stock on hand at a location">
                            Update Quantity
                        </button>
                        <button class="btn" t-on-click.stop="onReplenish" t-att-disabled="state.replenishing"
                                title="Run this product's reordering rules now">
                            <t t-if="state.replenishing">Replenishing…</t><t t-else="">Replenish</t>
                        </button>
                    </t>
                    <t t-else="">
                        <button class="btn so-stat-btn-disabled"
                                title="Only storable products carry stock">Update Quantity</button>
                        <button class="btn so-stat-btn-disabled"
                                title="Only storable products carry stock">Replenish</button>
                    </t>
                </div>
            </div>

            <!-- Inventory adjustment: count the on-hand per location -->
            <t t-if="state.adjust.open">
                <div class="gf-modal" t-on-click="onAdjustBackdrop">
                    <div class="prd-adjust-card" t-on-click.stop="">
                        <h3 class="gf-modal-title">Update Quantity &#8212; <t t-esc="state.record.name"/></h3>
                        <p class="prd-adjust-hint">
                            Set the counted quantity. The difference is booked as a move to or from
                            Inventory Adjustments, so the correction stays in the stock ledger.
                        </p>
                        <table class="prd-adjust-table">
                            <thead>
                                <tr><th>Location</th><th class="num">On Hand</th><th class="num">Counted</th></tr>
                            </thead>
                            <tbody>
                                <tr t-foreach="state.adjust.rows" t-as="ar" t-key="ar.location_id">
                                    <td t-esc="ar.location_name"/>
                                    <td class="num" t-esc="fmtQty(ar.on_hand)"/>
                                    <td class="num">
                                        <input type="number" step="0.01" class="prd-adjust-input"
                                               t-att-value="ar.counted"
                                               t-att-data-loc="ar.location_id"
                                               t-on-input="onAdjustInput"/>
                                    </td>
                                </tr>
                                <tr t-if="!state.adjust.rows.length">
                                    <td colspan="3" class="prd-adjust-empty">
                                        No internal locations available.
                                    </td>
                                </tr>
                            </tbody>
                        </table>
                        <div t-if="state.adjust.error" class="gf-modal-err" t-esc="state.adjust.error"/>
                        <div class="gf-modal-actions">
                            <button class="btn" t-on-click="() => this.state.adjust.open = false">Cancel</button>
                            <button class="btn btn-primary" t-on-click="onApplyAdjust"
                                    t-att-disabled="state.adjust.saving">
                                <t t-if="state.adjust.saving">Applying…</t><t t-else="">Apply</t>
                            </button>
                        </div>
                    </div>
                </div>
            </t>

            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">

                <div class="so-card">

                    <!-- ── Stat widget row ─────────────────────────── -->
                    <div class="prd-stat-row">
                        <div class="prd-stat-widget prd-stat-disabled"
                             title="Go to Website — requires website module (not planned)">
                            <span class="prd-stat-icon">🌐</span>
                            <span class="prd-stat-lbl">Go to Website</span>
                        </div>
                        <div t-attf-class="prd-stat-widget{{ stockTracked ? '' : ' prd-stat-disabled' }}"
                             t-on-click.stop="onViewOnHand"
                             title="Units on hand across internal locations">
                            <span class="prd-stat-icon">📦</span>
                            <span class="prd-stat-num" t-esc="fmtQty(state.stock.qty_available)"/>
                            <span class="prd-stat-lbl">On Hand</span>
                        </div>
                        <div t-attf-class="prd-stat-widget{{ stockTracked ? '' : ' prd-stat-disabled' }}"
                             t-on-click.stop="onViewForecast"
                             t-att-title="forecastTitle">
                            <span class="prd-stat-icon">📊</span>
                            <span class="prd-stat-num" t-esc="fmtQty(state.stock.virtual_available)"/>
                            <span class="prd-stat-lbl">Forecasted</span>
                        </div>
                        <div class="prd-stat-widget" t-on-click.stop="onViewMoves"
                             t-att-title="'Stock moves for this product'">
                            <span class="prd-stat-icon">↔</span>
                            <span class="prd-stat-num" t-esc="state.moveCount"/>
                            <span class="prd-stat-lbl">Product Moves</span>
                        </div>
                        <div class="prd-stat-widget" t-on-click.stop="onViewOrderpoints"
                             title="Reordering rules for this product">
                            <span class="prd-stat-icon">🔄</span>
                            <span class="prd-stat-num" t-esc="state.stock.orderpoint_count"/>
                            <span class="prd-stat-lbl">Reordering Rules</span>
                        </div>
                        <div class="prd-stat-widget" t-on-click.stop="onViewBom"
                             title="Bills of Materials for this product">
                            <span class="prd-stat-icon">⚗</span>
                            <span class="prd-stat-num" t-esc="state.bomCount"/>
                            <span class="prd-stat-lbl">Bill of Materials</span>
                        </div>
                        <div class="prd-stat-widget" t-on-click.stop="onViewPutaway"
                             title="Putaway rules for this product">
                            <span class="prd-stat-icon">📋</span>
                            <span class="prd-stat-num" t-esc="state.stock.putaway_count"/>
                            <span class="prd-stat-lbl">Putaway Rules</span>
                        </div>
                    </div>

                    <!-- ── Identity: name + checkboxes + image ───────── -->
                    <div class="prd-identity">
                        <div class="prd-identity-main">
                            <input class="prd-name-input form-input" type="text"
                                   data-field="name" placeholder="Product Name"
                                   t-att-value="state.record.name || ''"/>
                            <div class="prd-checkboxes">
                                <label class="prd-check-lbl">
                                    <input type="checkbox" data-field="sale_ok"
                                           t-att-checked="state.record.sale_ok ? true : undefined"/>
                                    Can be Sold
                                </label>
                                <label class="prd-check-lbl">
                                    <input type="checkbox" data-field="purchase_ok"
                                           t-att-checked="state.record.purchase_ok ? true : undefined"/>
                                    Can be Purchased
                                </label>
                                <label class="prd-check-lbl">
                                    <input type="checkbox" data-field="expense_ok"
                                           t-att-checked="state.record.expense_ok ? true : undefined"/>
                                    Can be Expensed
                                </label>
                            </div>
                        </div>
                        <!-- Product image — label triggers file input natively -->
                        <label class="prd-image-box" for="prd-file-input">
                            <t t-if="state.record.image_1920">
                                <img class="prd-img-preview"
                                     t-att-src="'data:image/*;base64,' + state.record.image_1920"
                                     alt="Product image"/>
                            </t>
                            <t t-else="">
                                <span class="prd-img-icon">📷</span>
                                <span class="prd-img-lbl">Add Photo</span>
                            </t>
                        </label>
                        <input id="prd-file-input" type="file" accept="image/*"
                               style="display:none" t-on-change.stop="onImageChange"/>
                    </div>

                    <!-- ── Tabs ──────────────────────────────────────── -->
                    <div class="so-tabs">
                        <div t-attf-class="so-tab{{state.activeTab==='general'?' active':''}}"
                             t-on-click.stop="()=>this.setTab('general')">General Information</div>
                        <div t-attf-class="so-tab{{state.activeTab==='sales'?' active':''}}"
                             t-on-click.stop="()=>this.setTab('sales')">Sales</div>
                        <div t-attf-class="so-tab{{state.activeTab==='purchase'?' active':''}}"
                             t-on-click.stop="()=>this.setTab('purchase')">Purchase</div>
                        <div t-attf-class="so-tab{{state.activeTab==='inventory'?' active':''}}"
                             t-on-click.stop="()=>this.setTab('inventory')">Inventory</div>
                        <div t-attf-class="so-tab{{state.activeTab==='accounting'?' active':''}}"
                             t-on-click.stop="()=>this.setTab('accounting')">Accounting</div>
                    </div>

                    <!-- ── General Information tab ───────────────────── -->
                    <t t-if="state.activeTab === 'general'">
                        <div class="so-info-grid" style="margin-top:12px;">
                            <!-- Left column -->
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Product Type</label>
                                    <select class="form-input" data-field="type">
                                        <option value="consu"
                                                t-att-selected="isType('consu')">Consumable</option>
                                        <option value="service"
                                                t-att-selected="isType('service')">Service</option>
                                        <option value="storable"
                                                t-att-selected="isType('storable')">Storable Product</option>
                                    </select>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Product Category</label>
                                    <!-- The generic form has had this ＋ since docs/078; the
                                         bespoke Product form did not, so adding a category
                                         meant leaving the product half-filled. -->
                                    <div class="prd-combo">
                                        <select class="form-input" data-field="categ_id">
                                            <option value="0">—</option>
                                            <t t-foreach="categoriesByPath" t-as="cat" t-key="cat.id">
                                                <option t-att-value="cat.id"
                                                        t-att-selected="m2oId(state.record.categ_id)===cat.id?true:undefined"
                                                        t-esc="cat.fullPath"/>
                                            </t>
                                        </select>
                                        <button class="prd-combo-add" title="New category"
                                                t-on-click.stop.prevent="() => this.openAddNew('product.category', 'Category', 'categ_id')">＋</button>
                                    </div>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Internal Reference</label>
                                    <input class="form-input" type="text" data-field="default_code"
                                           t-att-value="state.record.default_code || ''"/>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Barcode</label>
                                    <input class="form-input" type="text" data-field="barcode"
                                           t-att-value="strVal(state.record.barcode)"/>
                                </div>
                            </div>
                            <!-- Right column -->
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Sales Price</label>
                                    <div class="prd-price-row">
                                        <input class="form-input prd-price-input" type="number"
                                               step="0.01" min="0" data-field="list_price"
                                               t-att-value="state.record.list_price ?? 0"/>
                                        <span class="prd-currency-lbl" t-esc="state.currencySymbol"/>
                                    </div>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Customer Taxes</label>
                                    <span class="so-field-val prd-stub-note"
                                          title="Tax field on product requires account_tax_ids — coming soon">
                                        — (not linked)
                                    </span>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Cost</label>
                                    <input class="form-input prd-price-input" type="number"
                                           step="0.01" min="0" data-field="standard_price"
                                           t-att-value="state.record.standard_price ?? 0"/>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Unit of Measure</label>
                                    <div class="prd-combo">
                                        <select class="form-input" data-field="uom_id">
                                            <t t-foreach="state.uoms" t-as="uom" t-key="uom.id">
                                                <option t-att-value="uom.id"
                                                        t-att-selected="m2oId(state.record.uom_id)===uom.id?true:undefined"
                                                        t-esc="uom.name"/>
                                            </t>
                                        </select>
                                        <button class="prd-combo-add" title="New unit of measure"
                                                t-on-click.stop.prevent="() => this.openAddNew('uom.uom', 'Unit of Measure', 'uom_id')">＋</button>
                                    </div>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Purchase UoM</label>
                                    <select class="form-input" data-field="uom_po_id">
                                        <t t-foreach="state.uoms" t-as="uom" t-key="uom.id">
                                            <option t-att-value="uom.id"
                                                    t-att-selected="m2oId(state.record.uom_po_id)===uom.id?true:undefined"
                                                    t-esc="uom.name"/>
                                        </t>
                                    </select>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- ── Sales tab ─────────────────────────────────── -->
                    <t t-if="state.activeTab === 'sales'">
                        <div class="so-info-grid" style="margin-top:12px;">
                            <!-- Left column: sales policy & warnings -->
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Invoicing Policy</label>
                                    <select class="form-input" data-field="invoice_policy">
                                        <option value="order"
                                                t-att-selected="(state.record.invoice_policy||'order')==='order'?true:undefined">
                                            Ordered Quantities
                                        </option>
                                        <option value="delivery"
                                                t-att-selected="state.record.invoice_policy==='delivery'?true:undefined">
                                            Delivered Quantities
                                        </option>
                                    </select>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Sales Warning</label>
                                    <select class="form-input" data-field="sale_line_warn">
                                        <option value="no-message"
                                                t-att-selected="(state.record.sale_line_warn||'no-message')==='no-message'?true:undefined">
                                            No Warning
                                        </option>
                                        <option value="warning"
                                                t-att-selected="state.record.sale_line_warn==='warning'?true:undefined">
                                            Warning
                                        </option>
                                        <option value="block"
                                                t-att-selected="state.record.sale_line_warn==='block'?true:undefined">
                                            Blocking Message
                                        </option>
                                    </select>
                                </div>
                                <t t-if="state.record.sale_line_warn &amp;&amp; state.record.sale_line_warn !== 'no-message'">
                                    <div class="so-field-row" style="flex-direction:column;align-items:flex-start;gap:4px;">
                                        <label class="so-field-lbl">Warning Message</label>
                                        <textarea class="form-input prd-notes-area" data-field="sale_line_warn_msg"
                                                  style="min-height:60px"
                                                  placeholder="Message shown when this product is added to a sale order…"
                                                  t-att-value="strVal(state.record.sale_line_warn_msg)"/>
                                    </div>
                                </t>
                            </div>
                            <!-- Right column: sales description -->
                            <div class="so-info-col">
                                <div class="so-field-row" style="flex-direction:column;align-items:flex-start;gap:4px;">
                                    <label class="so-field-lbl">Sales Description</label>
                                    <textarea class="form-input prd-notes-area" data-field="description_sale"
                                              placeholder="Description shown to customers on quotations and sales orders…"
                                              t-att-value="strVal(state.record.description_sale)"/>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- ── Purchase tab ──────────────────────────────── -->
                    <t t-elif="state.activeTab === 'purchase'">
                        <div class="so-info-grid" style="margin-top:12px;">
                            <!-- Left column: purchase policy & warnings -->
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Control Policy</label>
                                    <select class="form-input" data-field="purchase_method">
                                        <option value="purchase"
                                                t-att-selected="(state.record.purchase_method||'purchase')==='purchase'?true:undefined">
                                            Based on Ordered Quantities
                                        </option>
                                        <option value="receive"
                                                t-att-selected="state.record.purchase_method==='receive'?true:undefined">
                                            Based on Received Quantities
                                        </option>
                                    </select>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Purchase Lead Time (days)</label>
                                    <input class="form-input" type="number" step="0.5" min="0"
                                           data-field="purchase_lead_time"
                                           t-att-value="state.record.purchase_lead_time ?? 0"/>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Purchase Warning</label>
                                    <select class="form-input" data-field="purchase_line_warn">
                                        <option value="no-message"
                                                t-att-selected="(state.record.purchase_line_warn||'no-message')==='no-message'?true:undefined">
                                            No Warning
                                        </option>
                                        <option value="warning"
                                                t-att-selected="state.record.purchase_line_warn==='warning'?true:undefined">
                                            Warning
                                        </option>
                                        <option value="block"
                                                t-att-selected="state.record.purchase_line_warn==='block'?true:undefined">
                                            Blocking Message
                                        </option>
                                    </select>
                                </div>
                                <t t-if="state.record.purchase_line_warn &amp;&amp; state.record.purchase_line_warn !== 'no-message'">
                                    <div class="so-field-row" style="flex-direction:column;align-items:flex-start;gap:4px;">
                                        <label class="so-field-lbl">Warning Message</label>
                                        <textarea class="form-input prd-notes-area" data-field="purchase_line_warn_msg"
                                                  style="min-height:60px"
                                                  placeholder="Message shown when this product is added to a purchase order…"
                                                  t-att-value="strVal(state.record.purchase_line_warn_msg)"/>
                                    </div>
                                </t>
                            </div>
                            <!-- Right column: purchase description -->
                            <div class="so-info-col">
                                <div class="so-field-row" style="flex-direction:column;align-items:flex-start;gap:4px;">
                                    <label class="so-field-lbl">Purchase Description</label>
                                    <textarea class="form-input prd-notes-area" data-field="description_purchase"
                                              placeholder="Description shown to vendors on purchase orders…"
                                              t-att-value="strVal(state.record.description_purchase)"/>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- ── Inventory tab ─────────────────────────────── -->
                    <t t-elif="state.activeTab === 'inventory'">
                        <div class="so-info-grid" style="margin-top:12px;">
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Weight (kg)</label>
                                    <input class="form-input" type="number" step="0.001" min="0"
                                           data-field="weight"
                                           t-att-value="state.record.weight ?? 0"/>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Volume (m³)</label>
                                    <input class="form-input" type="number" step="0.001" min="0"
                                           data-field="volume"
                                           t-att-value="state.record.volume ?? 0"/>
                                </div>
                            </div>
                            <div class="so-info-col">
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Product Type</label>
                                    <span class="so-field-val" t-esc="typeLabel"/>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Tracking</label>
                                    <span class="so-field-val prd-stub-note"
                                          title="Lot/serial tracking — coming soon">No Tracking</span>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- ── Accounting tab ────────────────────────────── -->
                    <t t-elif="state.activeTab === 'accounting'">
                        <div class="so-info-grid" style="margin-top:12px;">
                            <!-- Left column: receivable / income -->
                            <div class="so-info-col">
                                <div class="prd-tab-section-title">Receivables</div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Income Account</label>
                                    <select class="form-input" data-field="income_account_id">
                                        <option value="0">—</option>
                                        <t t-foreach="state.accounts" t-as="acc" t-key="acc.id">
                                            <option t-att-value="acc.id"
                                                    t-att-selected="m2oId(state.record.income_account_id)===acc.id?true:undefined"
                                                    t-esc="acc.code + ' ' + acc.name"/>
                                        </t>
                                    </select>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Customer Taxes</label>
                                    <span class="prd-stub-note" title="Tax management via account.tax — configured in Accounting module">
                                        Managed in Accounting
                                    </span>
                                </div>
                            </div>
                            <!-- Right column: payable / expense -->
                            <div class="so-info-col">
                                <div class="prd-tab-section-title">Payables</div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Expense Account</label>
                                    <select class="form-input" data-field="expense_account_id">
                                        <option value="0">—</option>
                                        <t t-foreach="state.accounts" t-as="acc" t-key="acc.id">
                                            <option t-att-value="acc.id"
                                                    t-att-selected="m2oId(state.record.expense_account_id)===acc.id?true:undefined"
                                                    t-esc="acc.code + ' ' + acc.name"/>
                                        </t>
                                    </select>
                                </div>
                                <div class="so-field-row">
                                    <label class="so-field-lbl">Vendor Taxes</label>
                                    <span class="prd-stub-note" title="Tax management via account.tax — configured in Accounting module">
                                        Managed in Accounting
                                    </span>
                                </div>
                            </div>
                        </div>
                    </t>

                    <!-- ── Internal Notes ────────────────────────────── -->
                    <div class="prd-notes-section">
                        <div class="prd-notes-header">
                            <span class="prd-notes-lbl">Internal Notes</span>
                            <span class="prd-notes-sub">This note is only for internal purposes.</span>
                        </div>
                        <textarea class="form-input prd-notes-area"
                                  data-field="description"
                                  t-att-value="strVal(state.record.description)"
                                  placeholder="Add internal notes…"/>
                    </div>

                </div><!-- /.so-card -->

                <!-- Inline create for the Category / UoM pickers (docs/092) -->
                <t t-if="state.addNew">
                    <div class="gf-modal" t-on-click="onAddNewBackdrop">
                        <div class="gf-modal-card" t-on-click.stop="">
                            <h3 class="gf-modal-title">New <t t-esc="state.addNew.label"/></h3>
                            <label class="gf-modal-lbl">Name</label>
                            <input class="gf-modal-input" t-att-value="state.addNew.name"
                                   t-on-input="onAddNewNameInput" t-on-keydown="onAddNewKey"/>
                            <div t-if="state.addNew.error" class="gf-modal-err" t-esc="state.addNew.error"/>
                            <div class="gf-modal-actions">
                                <button class="btn" t-on-click="closeAddNew">Cancel</button>
                                <button class="btn btn-primary" t-on-click="submitAddNew"
                                        t-att-disabled="state.addNew.saving">Create</button>
                            </div>
                        </div>
                    </div>
                </t>

                <!-- ── Datasheets & documents ──────────────────── -->
                <t t-if="!isNew">
                    <AttachmentPanel model="'product.product'"
                                     recordId="props.recordId"
                                     title="'Datasheets &amp; Documents'"/>
                </t>

                <!-- ── Chatter ───────────────────────────────────── -->
                <t t-if="!isNew">
                    <ChatterPanel model="'product.product'"
                                  recordId="props.recordId"
                                  refreshKey="state.chatRefreshKey"/>
                </t>

            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading:        true,
            error:          null,
            record:         {},
            categories:     [],
            uoms:           [],
            accounts:       [],
            currencySymbol: 'RM',
            moveCount:      0,
            bomCount:       0,
            activeTab:      'general',
            chatRefreshKey: 0,
            // Live inventory figures (stock.quant product_summary)
            stock: { qty_available: 0, reserved: 0, incoming_qty: 0, outgoing_qty: 0,
                     virtual_available: 0, orderpoint_count: 0, putaway_count: 0 },
            replenishing: false,
            adjust: { open: false, rows: [], saving: false, error: '' },
            // Inline create for the Category / UoM comboboxes
            addNew: null,   // { model, label, field, name, error, saving }
        });
        this._orig = {};
        onMounted(() => this.load());
    }

    // ---- Inline create (＋ beside a picker) --------------------------
    // The generic FormView has had this since docs/078. Without it here, adding
    // a missing category meant abandoning a half-filled product form.
    openAddNew(model, label, field) {
        this.state.addNew = { model, label, field, name: '', error: '', saving: false };
    }
    closeAddNew() { this.state.addNew = null; }
    onAddNewBackdrop(ev) { if (ev.target === ev.currentTarget) this.state.addNew = null; }
    onAddNewNameInput(ev) { if (this.state.addNew) this.state.addNew.name = ev.target.value; }
    onAddNewKey(ev) {
        if (ev.key === 'Enter')  { ev.preventDefault(); this.submitAddNew(); }
        if (ev.key === 'Escape') { this.closeAddNew(); }
    }

    async submitAddNew() {
        const ctx = this.state.addNew;
        if (!ctx || ctx.saving) return;
        const name = (ctx.name || '').trim();
        if (!name) { ctx.error = 'A name is required.'; return; }
        ctx.saving = true;
        ctx.error  = '';
        try {
            const id = await RpcService.call(ctx.model, 'create', [{ name }], {});
            // Refresh the list the picker reads from, then select the new row —
            // the point of the ＋ is not having to go and find it.
            if (ctx.model === 'product.category') {
                const cats = await RpcService.call('product.category', 'search_read',
                    [[]], { fields: ['id','name','parent_id'], limit: 500 });
                this.state.categories = Array.isArray(cats) ? cats : [];
            } else {
                const uoms = await RpcService.call('uom.uom', 'search_read',
                    [[]], { fields: ['id','name'], limit: 100 });
                this.state.uoms = Array.isArray(uoms) ? uoms : [];
            }
            this.state.record[ctx.field] = id;
            this.state.addNew = null;
        } catch (e) {
            // Surface the server's message — "Category name is required." is
            // more use than a silent failure.
            ctx.error  = e.message || String(e);
            ctx.saving = false;
        }
    }

    // Storable products carry stock. A consumable that already has quants or a
    // reordering rule still needs its counters usable, so it counts too.
    get stockTracked() {
        if ((this.state.record.type || 'consu') === 'storable') return true;
        const s = this.state.stock;
        return !!(s.qty_available || s.incoming_qty || s.outgoing_qty || s.orderpoint_count);
    }

    get forecastTitle() {
        const s = this.state.stock;
        return `Forecasted = on hand ${this.fmtQty(s.qty_available)}`
             + ` + incoming ${this.fmtQty(s.incoming_qty)}`
             + ` − outgoing ${this.fmtQty(s.outgoing_qty)}`;
    }

    fmtQty(v) { return (Number(v) || 0).toFixed(2); }

    get isNew() { return !this.props.recordId; }

    get categoriesByPath() {
        const cats = this.state.categories || [];
        const paths = buildCategoryPaths(cats);
        return cats
            .map(c => ({ ...c, fullPath: paths.get(c.id) || c.name }))
            .sort((a, b) => a.fullPath.localeCompare(b.fullPath));
    }

    async load() {
        this.state.loading = true;
        this.state.error   = null;
        try {
            const [cats, uoms, accs] = await Promise.all([
                RpcService.call('product.category', 'search_read',
                    [[]], { fields: ['id','name','parent_id'], limit: 500 }),
                RpcService.call('uom.uom', 'search_read',
                    [[]], { fields: ['id','name'], limit: 100 }),
                RpcService.call('account.account', 'search_read',
                    [[]], { fields: ['id','code','name'], limit: 500 }),
            ]);
            this.state.categories = Array.isArray(cats) ? cats : [];
            this.state.uoms       = Array.isArray(uoms) ? uoms : [];
            this.state.accounts   = Array.isArray(accs) ? accs : [];

            if (this.props.recordId) {
                const recs = await RpcService.call('product.product', 'read',
                    [[this.props.recordId]],
                    { fields: ['id','name','default_code','barcode','description','type',
                               'categ_id','uom_id','uom_po_id','list_price',
                               'standard_price','sale_ok','purchase_ok','expense_ok',
                               'volume','weight','image_1920','active',
                               'description_sale','description_purchase',
                               'income_account_id','expense_account_id',
                               'invoice_policy','sale_line_warn','sale_line_warn_msg',
                               'purchase_method','purchase_lead_time',
                               'purchase_line_warn','purchase_line_warn_msg'] });
                if (!recs || recs.length === 0) throw new Error('Product not found');
                this.state.record = { ...recs[0] };
                this._orig        = { ...recs[0] };

                const [mc, bc] = await Promise.all([
                    RpcService.call('stock.move', 'search_count',
                        [[['product_id','=',this.props.recordId]]]),
                    RpcService.call('mrp.bom', 'search_count',
                        [[['product_id','=',this.props.recordId]]]),
                ]);
                this.state.moveCount = typeof mc === 'number' ? mc : 0;
                this.state.bomCount  = typeof bc === 'number' ? bc : 0;
                await this.loadStock();
            } else {
                this.state.record = {
                    name: '', default_code: '', barcode: false,
                    description: false, type: 'consu',
                    categ_id: false, uom_id: 1, uom_po_id: 1,
                    list_price: 0, standard_price: 0,
                    volume: 0, weight: 0,
                    sale_ok: true, purchase_ok: true, expense_ok: false,
                    image_1920: false, active: true,
                    description_sale: false, description_purchase: false,
                    income_account_id: false, expense_account_id: false,
                    invoice_policy: 'order', sale_line_warn: 'no-message', sale_line_warn_msg: false,
                    purchase_method: 'purchase', purchase_lead_time: 0,
                    purchase_line_warn: 'no-message', purchase_line_warn_msg: false,
                };
                this._orig = { ...this.state.record };
            }
        } catch (e) {
            this.state.error = e.message || 'Failed to load product';
        } finally {
            this.state.loading = false;
        }
    }

    // ---- Helpers ----
    m2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        if (typeof val === 'string') return parseInt(val) || 0;
        return 0;
    }
    strVal(val) { return (!val || val === false) ? '' : String(val); }
    isType(t)   { return (this.state.record.type || 'consu') === t ? true : undefined; }

    setTab(tab) { this.state.activeTab = tab; }

    get typeLabel() {
        const t = this.state.record.type || 'consu';
        return t === 'consu' ? 'Consumable' : t === 'service' ? 'Service' : 'Storable Product';
    }

    // ---- Event handlers ----
    onFormChange(e) {
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.type === 'checkbox') {
            this.state.record[field] = e.target.checked;
        } else if (e.target.tagName === 'SELECT') {
            const raw = e.target.value;
            this.state.record[field] = isNaN(raw) || raw === '' ? raw : (parseInt(raw) || 0);
        }
    }

    onFormInput(e) {
        const field = e.target.dataset.field;
        if (!field || e.target.tagName === 'SELECT') return;
        if (e.target.type === 'number') {
            this.state.record[field] = parseFloat(e.target.value) || 0;
        } else {
            this.state.record[field] = e.target.value;
        }
    }

    // ---- Actions ----
    async onSave() {
        const r = this.state.record;
        if (!r.name || !r.name.trim()) { alert('Product Name is required.'); return; }
        const vals = {
            name:           r.name.trim(),
            default_code:   r.default_code  || false,
            barcode:        r.barcode        || false,
            description:    r.description   || false,
            type:           r.type          || 'consu',
            categ_id:       this.m2oId(r.categ_id)  || false,
            uom_id:         this.m2oId(r.uom_id)    || 1,
            uom_po_id:      this.m2oId(r.uom_po_id) || 1,
            list_price:     r.list_price     ?? 0,
            standard_price: r.standard_price ?? 0,
            volume:         r.volume         ?? 0,
            weight:         r.weight         ?? 0,
            sale_ok:              !!r.sale_ok,
            purchase_ok:          !!r.purchase_ok,
            expense_ok:           !!r.expense_ok,
            image_1920:           r.image_1920           || false,
            description_sale:     r.description_sale     || false,
            description_purchase: r.description_purchase || false,
            income_account_id:    this.m2oId(r.income_account_id)  || false,
            expense_account_id:   this.m2oId(r.expense_account_id) || false,
            invoice_policy:       r.invoice_policy     || 'order',
            sale_line_warn:       r.sale_line_warn     || 'no-message',
            sale_line_warn_msg:   r.sale_line_warn_msg || false,
            purchase_method:      r.purchase_method    || 'purchase',
            purchase_lead_time:   r.purchase_lead_time ?? 0,
            purchase_line_warn:   r.purchase_line_warn || 'no-message',
            purchase_line_warn_msg: r.purchase_line_warn_msg || false,
        };
        try {
            if (this.props.recordId) {
                await RpcService.call('product.product', 'write',
                    [[this.props.recordId], vals]);
                await this.load();
            } else {
                await RpcService.call('product.product', 'create', [vals]);
                this.props.onBack();
            }
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }

    onDiscard() {
        if (this.isNew) { this.props.onBack(); return; }
        this.state.record = { ...this._orig };
    }

    onImageChange(e) {
        const file = e.target.files?.[0];
        if (!file) return;
        const reader = new FileReader();
        reader.onload = (ev) => {
            const b64 = ev.target.result.replace(/^data:[^;]+;base64,/, '');
            this.state.record.image_1920 = b64;
        };
        reader.readAsDataURL(file);
    }

    // ---- Inventory: live figures, adjustment, replenishment ----------
    // The stat row used to be inert placeholders. stock.quant, the reordering
    // rules and the putaway rules all exist, so these read and write the real
    // ledger (docs/090).

    async loadStock() {
        if (!this.props.recordId) return;
        try {
            const s = await RpcService.call('stock.quant', 'product_summary',
                                            [this.props.recordId], {});
            if (s && typeof s === 'object') this.state.stock = { ...this.state.stock, ...s };
        } catch (e) { console.error(e); }
    }

    async onUpdateQuantity() {
        this.state.adjust = { open: true, rows: [], saving: false, error: '' };
        try {
            // Every internal location, with the product's current quant on it —
            // stock can be counted into a location that has no quant row yet.
            const [locs, quants] = await Promise.all([
                RpcService.call('stock.location', 'search_read',
                    [[['usage','=','internal']]], { fields: ['id','complete_name','name'], limit: 200 }),
                RpcService.call('stock.quant', 'search_read',
                    [[['product_id','=',this.props.recordId]]], { fields: ['id','location_id','quantity'], limit: 200 }),
            ]);
            const onHand = new Map();
            for (const q of (Array.isArray(quants) ? quants : [])) {
                const lid = Array.isArray(q.location_id) ? q.location_id[0] : q.location_id;
                onHand.set(lid, (onHand.get(lid) || 0) + (Number(q.quantity) || 0));
            }
            this.state.adjust.rows = (Array.isArray(locs) ? locs : []).map(l => ({
                location_id:   l.id,
                location_name: l.complete_name || l.name,
                on_hand:       onHand.get(l.id) || 0,
                counted:       onHand.get(l.id) || 0,
            }));
        } catch (e) {
            this.state.adjust.error = e.message || 'Failed to load locations';
        }
    }

    onAdjustInput(ev) {
        const lid = parseInt(ev.target.dataset.loc);
        const row = this.state.adjust.rows.find(r => r.location_id === lid);
        if (row) row.counted = parseFloat(ev.target.value);
    }

    onAdjustBackdrop(ev) { if (ev.target === ev.currentTarget) this.state.adjust.open = false; }

    async onApplyAdjust() {
        this.state.adjust.saving = true;
        this.state.adjust.error  = '';
        try {
            // Only the rows whose count actually moved — set_on_hand books a
            // stock move per call, and an unchanged line would book a zero move.
            const changed = this.state.adjust.rows.filter(
                r => Number.isFinite(r.counted) && Math.abs(r.counted - r.on_hand) > 1e-6);
            for (const r of changed) {
                await RpcService.call('stock.quant', 'set_on_hand',
                    [{ product_id: this.props.recordId, location_id: r.location_id, quantity: r.counted }], {});
            }
            this.state.adjust.open = false;
            await this.loadStock();
            const mc = await RpcService.call('stock.move', 'search_count',
                                             [[['product_id','=',this.props.recordId]]]);
            this.state.moveCount = typeof mc === 'number' ? mc : this.state.moveCount;
        } catch (e) {
            this.state.adjust.error = e.message || 'Failed to apply the adjustment';
        } finally {
            this.state.adjust.saving = false;
        }
    }

    async onReplenish() {
        if (this.state.stock.orderpoint_count === 0) {
            alert('This product has no reordering rule yet. Add one first — the Reordering Rules button opens the list.');
            return;
        }
        this.state.replenishing = true;
        try {
            const res = await RpcService.call('stock.warehouse.orderpoint', 'run_scheduler',
                                              [{ product_id: this.props.recordId }], {});
            const made    = (res && res.replenishments_created) || 0;
            const noVend  = (res && res.skipped_no_vendor) || 0;
            if (made)        alert(`Replenishment drafted: ${made} order(s) created.`);
            else if (noVend) alert('Stock is below the minimum, but this product has no vendor — '
                                 + 'set one on the reordering rule or in the vendor pricelist.');
            else             alert('Nothing to replenish — forecasted stock is at or above the minimum.');
            await this.loadStock();
        } catch (e) {
            alert('Replenish failed: ' + (e.message || e));
        } finally {
            this.state.replenishing = false;
        }
    }

    onViewOnHand() {
        if (this.props.onNavigate) this.props.onNavigate('stock.quant', [['product_id','=',this.props.recordId]]);
    }
    onViewForecast() {
        // The forecast is on-hand plus pending moves — the moves are what a
        // user drills into to see why the number is what it is.
        if (this.props.onNavigate) this.props.onNavigate('stock.move', [['product_id','=',this.props.recordId]]);
    }
    onViewOrderpoints() {
        if (this.props.onNavigate) this.props.onNavigate('stock.warehouse.orderpoint', [['product_id','=',this.props.recordId]]);
    }
    onViewPutaway() {
        if (this.props.onNavigate) this.props.onNavigate('stock.putaway.rule', [['product_id','=',this.props.recordId]]);
    }

    onViewMoves() {
        if (this.props.onNavigate) {
            this.props.onNavigate('stock.move', [['product_id','=',this.props.recordId]]);
        }
    }

    onViewBom() {
        if (this.props.onNavigate) {
            this.props.onNavigate('mrp.bom', [['product_id','=',this.props.recordId]]);
        }
    }

    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// LocationFormView — stock.location detail
// ----------------------------------------------------------------
class LocationFormView extends Component {
    static template = xml`
        <div class="so-shell" t-on-change="onFormChange" t-on-input="onFormInput">
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Locations</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.complete_name || state.record.name || 'New'"/>
                    </div>
                    <div class="so-action-btns">
                        <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                        <button class="btn"             t-on-click.stop="onDiscard">Discard</button>
                    </div>
                </div>
            </div>
            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-info-grid" style="margin-top:12px;">
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Location Name</label>
                                <input class="form-input" type="text" data-field="name"
                                       t-att-value="state.record.name || ''"
                                       placeholder="e.g. Stock, Shelf A, Room 1"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Parent Location</label>
                                <select class="form-input" data-field="location_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.locations" t-as="loc" t-key="loc.id">
                                        <option t-att-value="loc.id"
                                                t-att-selected="m2oId(state.record.location_id)===loc.id?true:undefined"
                                                t-esc="loc.complete_name || loc.name"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Usage</label>
                                <select class="form-input" data-field="usage">
                                    <option value="internal"   t-att-selected="(state.record.usage||'internal')==='internal'?true:undefined">Internal Location</option>
                                    <option value="view"       t-att-selected="state.record.usage==='view'?true:undefined">View</option>
                                    <option value="supplier"   t-att-selected="state.record.usage==='supplier'?true:undefined">Vendor Location</option>
                                    <option value="customer"   t-att-selected="state.record.usage==='customer'?true:undefined">Customer Location</option>
                                    <option value="inventory"  t-att-selected="state.record.usage==='inventory'?true:undefined">Inventory Adjustments</option>
                                    <option value="transit"    t-att-selected="state.record.usage==='transit'?true:undefined">Transit</option>
                                </select>
                            </div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Full Path</label>
                                <span class="so-field-val" t-esc="state.record.complete_name || '—'"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Active</label>
                                <input type="checkbox" data-field="active"
                                       t-att-checked="state.record.active !== false ? true : undefined"/>
                            </div>
                        </div>
                    </div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ loading: true, error: null, record: {}, locations: [] });
        this._orig = {};
        onMounted(() => this.load());
    }

    async load() {
        this.state.loading = true;
        this.state.error   = null;
        try {
            const locs = await RpcService.call('stock.location', 'search_read', [[]], {
                fields: ['id','name','complete_name'], limit: 200 });
            this.state.locations = Array.isArray(locs) ? locs : [];

            if (this.props.recordId) {
                const recs = await RpcService.call('stock.location', 'read',
                    [[this.props.recordId]],
                    { fields: ['id','name','complete_name','location_id','usage','active','company_id'] });
                if (!recs || recs.length === 0) throw new Error('Location not found');
                this.state.record = { ...recs[0] };
                this._orig        = { ...recs[0] };
            } else {
                this.state.record = { name:'', complete_name:'', location_id: false, usage:'internal', active: true };
                this._orig = { ...this.state.record };
            }
        } catch (e) {
            this.state.error = e.message || 'Failed to load location';
        } finally {
            this.state.loading = false;
        }
    }

    m2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val)) return val[0] || 0;
        return parseInt(val) || 0;
    }

    onFormChange(e) {
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.type === 'checkbox') {
            this.state.record[field] = e.target.checked;
        } else if (e.target.tagName === 'SELECT') {
            const raw = e.target.value;
            this.state.record[field] = isNaN(raw) || raw === '' ? raw : (parseInt(raw) || 0);
        }
    }
    onFormInput(e) {
        const field = e.target.dataset.field;
        if (!field || e.target.tagName === 'SELECT') return;
        this.state.record[field] = e.target.value;
    }

    async onSave() {
        const r = this.state.record;
        if (!r.name || !r.name.trim()) { alert('Location name is required.'); return; }
        const vals = {
            name:        r.name.trim(),
            location_id: this.m2oId(r.location_id) || false,
            usage:       r.usage || 'internal',
            active:      r.active !== false,
        };
        try {
            if (this.props.recordId) {
                await RpcService.call('stock.location', 'write', [[this.props.recordId], vals]);
                await this.load();
            } else {
                await RpcService.call('stock.location', 'create', [vals]);
                this.props.onBack();
            }
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }
    onDiscard() {
        if (!this.props.recordId) { this.props.onBack(); return; }
        this.state.record = { ...this._orig };
    }
    onBack() { this.props.onBack(); }
}


// ----------------------------------------------------------------
// WarehouseFormView — stock.warehouse detail
// ----------------------------------------------------------------
class WarehouseFormView extends Component {
    static template = xml`
        <div class="so-shell" t-on-change="onFormChange" t-on-input="onFormInput">
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Warehouses</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New Warehouse'"/>
                    </div>
                    <div class="so-action-btns">
                        <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                        <button class="btn"             t-on-click.stop="onDiscard">Discard</button>
                    </div>
                </div>
            </div>
            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-info-grid" style="margin-top:12px;">
                        <!-- Left: identity -->
                        <div class="so-info-col">
                            <div class="prd-tab-section-title">Identity</div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Warehouse Name</label>
                                <input class="form-input" type="text" data-field="name"
                                       t-att-value="state.record.name || ''"
                                       placeholder="e.g. Main Warehouse"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Short Name</label>
                                <input class="form-input" type="text" data-field="code"
                                       t-att-value="state.record.code || ''"
                                       placeholder="e.g. WH" maxlength="5"/>
                            </div>
                        </div>
                        <!-- Right: locations + operation types (read-only info) -->
                        <div class="so-info-col">
                            <div class="prd-tab-section-title">Locations</div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Main Stock Location</label>
                                <select class="form-input" data-field="lot_stock_id">
                                    <option value="0">—</option>
                                    <t t-foreach="internalLocations" t-as="loc" t-key="loc.id">
                                        <option t-att-value="loc.id"
                                                t-att-selected="m2oId(state.record.lot_stock_id)===loc.id?true:undefined"
                                                t-esc="loc.complete_name || loc.name"/>
                                    </t>
                                </select>
                            </div>
                            <div class="prd-tab-section-title" style="margin-top:12px;">Operation Types</div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Receipts</label>
                                <select class="form-input" data-field="in_type_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.pickingTypes" t-as="pt" t-key="pt.id">
                                        <option t-att-value="pt.id"
                                                t-att-selected="m2oId(state.record.in_type_id)===pt.id?true:undefined"
                                                t-esc="pt.name"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Deliveries</label>
                                <select class="form-input" data-field="out_type_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.pickingTypes" t-as="pt" t-key="pt.id">
                                        <option t-att-value="pt.id"
                                                t-att-selected="m2oId(state.record.out_type_id)===pt.id?true:undefined"
                                                t-esc="pt.name"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Internal Transfers</label>
                                <select class="form-input" data-field="int_type_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.pickingTypes" t-as="pt" t-key="pt.id">
                                        <option t-att-value="pt.id"
                                                t-att-selected="m2oId(state.record.int_type_id)===pt.id?true:undefined"
                                                t-esc="pt.name"/>
                                    </t>
                                </select>
                            </div>
                        </div>
                    </div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ loading: true, error: null, record: {}, locations: [], pickingTypes: [] });
        this._orig = {};
        onMounted(() => this.load());
    }

    get internalLocations() {
        return this.state.locations.filter(l => l.usage === 'internal' || l.usage === 'view');
    }

    async load() {
        this.state.loading = true;
        this.state.error   = null;
        try {
            const [locs, pts] = await Promise.all([
                RpcService.call('stock.location', 'search_read', [[]], {
                    fields: ['id','name','complete_name','usage'], limit: 200 }),
                RpcService.call('stock.picking.type', 'search_read', [[]], {
                    fields: ['id','name','code'], limit: 50 }),
            ]);
            this.state.locations    = Array.isArray(locs) ? locs : [];
            this.state.pickingTypes = Array.isArray(pts)  ? pts  : [];

            if (this.props.recordId) {
                const recs = await RpcService.call('stock.warehouse', 'read',
                    [[this.props.recordId]],
                    { fields: ['id','name','code','company_id','lot_stock_id','view_location_id',
                               'in_type_id','out_type_id','int_type_id','active'] });
                if (!recs || recs.length === 0) throw new Error('Warehouse not found');
                this.state.record = { ...recs[0] };
                this._orig        = { ...recs[0] };
            } else {
                this.state.record = {
                    name: '', code: '', active: true,
                    lot_stock_id: false, view_location_id: false,
                    in_type_id: false, out_type_id: false, int_type_id: false,
                };
                this._orig = { ...this.state.record };
            }
        } catch (e) {
            this.state.error = e.message || 'Failed to load warehouse';
        } finally {
            this.state.loading = false;
        }
    }

    m2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val)) return val[0] || 0;
        return parseInt(val) || 0;
    }

    onFormChange(e) {
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.type === 'checkbox') {
            this.state.record[field] = e.target.checked;
        } else if (e.target.tagName === 'SELECT') {
            const raw = e.target.value;
            this.state.record[field] = isNaN(raw) || raw === '' ? raw : (parseInt(raw) || 0);
        }
    }
    onFormInput(e) {
        const field = e.target.dataset.field;
        if (!field || e.target.tagName === 'SELECT') return;
        this.state.record[field] = e.target.value;
    }

    async onSave() {
        const r = this.state.record;
        if (!r.name || !r.name.trim()) { alert('Warehouse name is required.'); return; }
        if (!r.code || !r.code.trim()) { alert('Short name is required.'); return; }
        const vals = {
            name:             r.name.trim(),
            code:             r.code.trim().toUpperCase(),
            lot_stock_id:     this.m2oId(r.lot_stock_id)  || false,
            in_type_id:       this.m2oId(r.in_type_id)    || false,
            out_type_id:      this.m2oId(r.out_type_id)   || false,
            int_type_id:      this.m2oId(r.int_type_id)   || false,
        };
        try {
            if (this.props.recordId) {
                await RpcService.call('stock.warehouse', 'write', [[this.props.recordId], vals]);
                await this.load();
            } else {
                await RpcService.call('stock.warehouse', 'create', [vals]);
                this.props.onBack();
            }
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }
    onDiscard() {
        if (!this.props.recordId) { this.props.onBack(); return; }
        this.state.record = { ...this._orig };
    }
    onBack() { this.props.onBack(); }
}


// ----------------------------------------------------------------
// ContactFormView — res.partner detail
// ----------------------------------------------------------------
class ContactFormView extends Component {
    static components = { ChatterPanel };
    static template = xml`
        <div class="so-shell"
             t-on-change="onFormChange"
             t-on-input="onFormInput">

            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Contacts</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="pageTitle"/>
                    </div>
                    <div class="so-action-btns">
                        <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                        <button class="btn" t-on-click.stop="onDiscard">Discard</button>
                    </div>
                </div>
            </div>

            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">

                    <!-- Name row -->
                    <div class="contact-name-row">
                        <input class="prd-name-input form-input" type="text"
                               data-field="name" placeholder="Contact Name"
                               t-att-value="state.record.name || ''"/>
                    </div>

                    <!-- All type tags as independent badges (any or none can be active) -->
                    <div class="contact-badges">
                        <span t-attf-class="contact-badge{{state.record.is_company ? ' active' : ''}}"
                              t-on-click.stop="toggleCompany">Company</span>
                        <span t-attf-class="contact-badge{{state.record.is_individual ? ' active' : ''}}"
                              t-on-click.stop="toggleIndividual">Individual</span>
                        <span t-attf-class="contact-badge{{isCustomer ? ' active' : ''}}"
                              t-on-click.stop="toggleCustomer">Customer</span>
                        <span t-attf-class="contact-badge{{isVendor ? ' active' : ''}}"
                              t-on-click.stop="toggleVendor">Vendor</span>
                        <span t-attf-class="contact-badge{{state.record.is_contractor ? ' active' : ''}}"
                              t-on-click.stop="toggleContractor">Contractor</span>
                    </div>

                    <!-- Two-column info grid -->
                    <div class="so-info-grid" style="margin-top:16px;">
                        <!-- Left: contact fields -->
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Company Name</label>
                                <t t-if="state.record.is_company">
                                    <!-- Company contacts: name IS company name, read-only here -->
                                    <input class="form-input" type="text" readonly="true"
                                           t-att-value="strVal(state.record.company_name)"
                                           placeholder="Set via Name field above"/>
                                </t>
                                <t t-else="">
                                    <t t-if="state.newCompanyMode">
                                        <!-- Free-text entry for a new company -->
                                        <input class="form-input" type="text" data-field="company_name"
                                               t-att-value="strVal(state.record.company_name)"
                                               placeholder="Enter new company name…"/>
                                        <span class="form-link" style="font-size:12px;cursor:pointer;margin-left:4px;"
                                              t-on-click.stop="()=>{ this.state.newCompanyMode=false; this.state.record.company_name=''; }">
                                            ✕ Cancel
                                        </span>
                                    </t>
                                    <t t-else="">
                                        <select class="form-input" t-on-change="onCompanyNameSelect">
                                            <option value="">—</option>
                                            <t t-foreach="state.companies" t-as="co" t-key="co.id">
                                                <option t-att-value="co.id"
                                                        t-att-selected="m2oId(state.record.company_id)===co.id?true:undefined"
                                                        t-esc="co.name"/>
                                            </t>
                                            <option value="__new__">— New Company —</option>
                                        </select>
                                    </t>
                                </t>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Job Position</label>
                                <input class="form-input" type="text" data-field="job_position"
                                       t-att-value="strVal(state.record.job_position)"
                                       placeholder="e.g. Manager, Contractor, Staff"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Email</label>
                                <input class="form-input" type="email" data-field="email"
                                       t-att-value="strVal(state.record.email)"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Phone</label>
                                <input class="form-input" type="tel" data-field="phone"
                                       t-att-value="strVal(state.record.phone)"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Mobile</label>
                                <input class="form-input" type="tel" data-field="mobile"
                                       t-att-value="strVal(state.record.mobile)"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Website</label>
                                <input class="form-input" type="text" data-field="website"
                                       t-att-value="strVal(state.record.website)"
                                       placeholder="https://"/>
                            </div>
                        </div>
                        <!-- Right: address -->
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Street</label>
                                <input class="form-input" type="text" data-field="street"
                                       t-att-value="strVal(state.record.street)"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">City</label>
                                <input class="form-input" type="text" data-field="city"
                                       t-att-value="strVal(state.record.city)"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">ZIP</label>
                                <input class="form-input" type="text" data-field="zip"
                                       t-att-value="strVal(state.record.zip)"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Country</label>
                                <select class="form-input" data-field="country_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.countries" t-as="co" t-key="co.id">
                                        <option t-att-value="co.id"
                                                t-att-selected="m2oId(state.record.country_id)===co.id?true:undefined"
                                                t-esc="co.name"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">State / Province</label>
                                <select class="form-input" data-field="state_id">
                                    <option value="0">—</option>
                                    <t t-foreach="filteredStates" t-as="st" t-key="st.id">
                                        <option t-att-value="st.id"
                                                t-att-selected="m2oId(state.record.state_id)===st.id?true:undefined"
                                                t-esc="st.name"/>
                                    </t>
                                </select>
                            </div>
                        </div>
                    </div>

                    <!-- Notes -->
                    <div class="prd-notes-section">
                        <div class="prd-notes-header">
                            <span class="prd-notes-lbl">Notes</span>
                            <span class="prd-notes-sub">Internal notes about this contact.</span>
                        </div>
                        <textarea class="form-input prd-notes-area"
                                  data-field="comment"
                                  t-att-value="strVal(state.record.comment)"
                                  placeholder="Internal notes…"/>
                    </div>

                </div><!-- /.so-card -->

                <!-- Chatter -->
                <t t-if="!isNew">
                    <ChatterPanel model="'res.partner'"
                                  recordId="props.recordId"
                                  refreshKey="state.chatRefreshKey"/>
                </t>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading:        true,
            error:          null,
            record:         {},
            countries:      [],
            states:         [],
            companies:      [],   // company contacts for Company Name select
            newCompanyMode: false,
            chatRefreshKey: 0,
        });
        this._orig = {};
        onMounted(() => this.load());
    }

    get isNew()      { return !this.props.recordId; }
    get pageTitle()  { return this.state.record.name || (this.isNew ? 'New Contact' : '…'); }
    get isCustomer() { return (this.state.record.customer_rank || 0) > 0; }
    get isVendor()   { return (this.state.record.vendor_rank   || 0) > 0; }

    get filteredStates() {
        const cid = this.m2oId(this.state.record.country_id);
        if (!cid) return this.state.states;
        return this.state.states.filter(s => {
            const sid = Array.isArray(s.country_id) ? s.country_id[0] : s.country_id;
            return sid === cid;
        });
    }

    async load() {
        this.state.loading = true;
        this.state.error   = null;
        try {
            const [countries, states, companies] = await Promise.all([
                RpcService.call('res.country', 'search_read', [[]], { fields: ['id','name'], limit: 300 }),
                RpcService.call('res.country.state', 'search_read', [[]], { fields: ['id','name','country_id'], limit: 500 }),
                RpcService.call('res.partner', 'search_read', [[['is_company','=',true]]], { fields: ['id','name'], limit: 500 }),
            ]);
            this.state.countries = Array.isArray(countries) ? countries : [];
            this.state.states    = Array.isArray(states)    ? states    : [];
            this.state.companies = Array.isArray(companies) ? companies : [];

            if (this.props.recordId) {
                const recs = await RpcService.call('res.partner', 'read',
                    [[this.props.recordId]],
                    { fields: ['id','name','company_name','company_id','email','phone','mobile','website',
                               'street','city','zip','lang','comment','job_position',
                               'is_company','is_individual','is_contractor',
                               'country_id','state_id',
                               'customer_rank','vendor_rank','active'] });
                if (!recs || recs.length === 0) throw new Error('Contact not found');
                this.state.record = { ...recs[0] };
                this._orig        = { ...recs[0] };
            } else {
                this.state.record = {
                    name: '', company_name: '', company_id: false,
                    email: '', phone: '', mobile: '', website: '',
                    street: '', city: '', zip: '', lang: '',
                    is_company: false, is_individual: false, is_contractor: false,
                    country_id: false, state_id: false,
                    customer_rank: 0, vendor_rank: 0,
                    comment: false, job_position: false, active: true,
                };
                this._orig = { ...this.state.record };
            }
        } catch (e) {
            this.state.error = e.message || 'Failed to load contact';
        } finally {
            this.state.loading = false;
        }
    }

    m2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        if (typeof val === 'string') return parseInt(val) || 0;
        return 0;
    }
    strVal(val) { return (!val || val === false) ? '' : String(val); }

    toggleCompany() {
        this.state.record.is_company = !this.state.record.is_company;
        if (this.state.record.is_company) {
            this.state.record.company_name = this.state.record.name || '';
            this.state.newCompanyMode = false;
        }
    }
    toggleIndividual() { this.state.record.is_individual = !this.state.record.is_individual; }

    onCompanyNameSelect(e) {
        const val = e.target.value;
        if (val === '__new__') {
            this.state.newCompanyMode = true;
            this.state.record.company_name = '';
            this.state.record.company_id   = false;
        } else if (val === '') {
            this.state.record.company_name = '';
            this.state.record.company_id   = false;
        } else {
            const coId = parseInt(val);
            const co   = this.state.companies.find(c => c.id === coId);
            this.state.record.company_id   = coId;
            this.state.record.company_name = co ? co.name : '';
        }
    }
    toggleCustomer()   { this.state.record.customer_rank = this.isCustomer ? 0 : 1; }
    toggleVendor()     { this.state.record.vendor_rank   = this.isVendor   ? 0 : 1; }
    toggleContractor() { this.state.record.is_contractor = !this.state.record.is_contractor; }

    onFormChange(e) {
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.type === 'checkbox') {
            this.state.record[field] = e.target.checked;
        } else if (e.target.tagName === 'SELECT') {
            const raw = e.target.value;
            this.state.record[field] = isNaN(raw) || raw === '' ? raw : (parseInt(raw) || 0);
        }
    }

    onFormInput(e) {
        const field = e.target.dataset.field;
        if (!field || e.target.tagName === 'SELECT') return;
        this.state.record[field] = e.target.value;
        // Keep company_name in sync with name for company-type contacts
        if (field === 'name' && this.state.record.is_company)
            this.state.record.company_name = e.target.value;
    }

    async onSave() {
        const r = this.state.record;
        if (!r.name || !r.name.trim()) { alert('Name is required.'); return; }

        // For company contacts, company_name always mirrors name
        if (r.is_company) r.company_name = r.name.trim();

        // Auto-create a company contact if company_name is typed but not in DB
        const companyNameVal = (r.company_name || '').trim();
        if (companyNameVal && !r.is_company) {
            // Resolve company_id: if name was typed free-form, find or create the company partner
            const existing = this.state.companies.find(
                c => c.name.toLowerCase() === companyNameVal.toLowerCase());
            if (existing) {
                r.company_id = existing.id;
            } else if (!this.m2oId(r.company_id)) {
                // Auto-create the company partner and capture its ID
                try {
                    const newCoId = await RpcService.call('res.partner', 'create', [{
                        name: companyNameVal,
                        is_company: true,
                        company_name: companyNameVal,
                    }]);
                    r.company_id = typeof newCoId === 'number' ? newCoId : 0;
                    const cos = await RpcService.call('res.partner', 'search_read',
                        [[['is_company','=',true]]], { fields: ['id','name'], limit: 500 });
                    this.state.companies = Array.isArray(cos) ? cos : [];
                } catch (e) {
                    console.warn('[contact] Auto-create company failed:', e.message || e);
                }
            }
        }

        const vals = {
            name:          r.name.trim(),
            company_name:  r.company_name   || false,
            company_id:    this.m2oId(r.company_id) || false,
            email:         r.email         || false,
            phone:         r.phone         || false,
            mobile:        r.mobile        || false,
            website:       r.website       || false,
            street:        r.street        || false,
            city:          r.city          || false,
            zip:           r.zip           || false,
            is_company:    !!r.is_company,
            is_individual: !!r.is_individual,
            is_contractor: !!r.is_contractor,
            country_id:    this.m2oId(r.country_id)  || false,
            state_id:      this.m2oId(r.state_id)    || false,
            customer_rank: r.customer_rank  || 0,
            vendor_rank:   r.vendor_rank    || 0,
            comment:       r.comment        || false,
            job_position:  r.job_position   || false,
        };
        try {
            if (this.props.recordId) {
                await RpcService.call('res.partner', 'write', [[this.props.recordId], vals]);
                await this.load();
            } else {
                await RpcService.call('res.partner', 'create', [vals]);
                this.props.onBack();
            }
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }

    onDiscard() {
        if (this.isNew) { this.props.onBack(); return; }
        this.state.record = { ...this._orig };
    }

    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// BomFormView — mrp.bom detail (Bill of Materials)
// ----------------------------------------------------------------
class BomFormView extends Component {
    static components = { DatePicker };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput">

            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Bills of Materials</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="pageTitle"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="state.isNew">
                            <button class="btn btn-primary" t-on-click.stop="onCreate">Create</button>
                        </t>
                        <t t-else="">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Discard</button>
                    </div>
                </div>
            </div>

            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="pageTitle"/>
                    </div>

                    <!-- Header fields -->
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">Product</label>
                                <select class="form-input" data-field="product_id">
                                    <option value="0">— select product —</option>
                                    <t t-foreach="state.products" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.product_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Reference</label>
                                <input class="form-input" type="text" data-field="code"
                                       t-att-value="state.record.code || ''"/>
                            </div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row">
                                <label class="so-field-lbl">BOM Type</label>
                                <select class="form-input" data-field="bom_type">
                                    <option value="normal"  t-att-selected="state.record.bom_type === 'normal'  ? true : undefined">Manufacture this Product</option>
                                    <option value="phantom" t-att-selected="state.record.bom_type === 'phantom' ? true : undefined">Kit</option>
                                </select>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Quantity</label>
                                <input class="form-input" type="number" step="0.001" min="0.001"
                                       data-field="product_qty"
                                       t-att-value="state.record.product_qty ?? 1"/>
                            </div>
                            <div class="so-field-row">
                                <label class="so-field-lbl">Unit of Measure</label>
                                <select class="form-input" data-field="product_uom_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.uoms" t-as="opt" t-key="opt.id">
                                        <option t-att-value="opt.id"
                                                t-att-selected="getM2oId(state.record.product_uom_id) === opt.id ? true : undefined"
                                                t-esc="opt.display"/>
                                    </t>
                                </select>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Components tab -->
                <div class="so-card" style="margin-top:12px">
                    <div class="so-card-head">
                        <h2 style="font-size:1rem;font-weight:600;margin:0">Components</h2>
                    </div>
                    <table class="so-line-table" style="width:100%">
                        <thead>
                            <tr>
                                <th>Component</th>
                                <th style="width:120px;text-align:right">Quantity</th>
                                <th style="width:130px">Unit of Measure</th>
                                <th style="width:40px"></th>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.lines" t-as="line" t-key="line._key">
                                <tr>
                                    <td>
                                        <select class="form-input" style="width:100%"
                                                t-att-data-key="line._key" data-line-field="product_id">
                                            <option value="0">— select product —</option>
                                            <t t-foreach="state.products" t-as="opt" t-key="opt.id">
                                                <option t-att-value="opt.id"
                                                        t-att-selected="getM2oId(line.product_id) === opt.id ? true : undefined"
                                                        t-esc="opt.display"/>
                                            </t>
                                        </select>
                                    </td>
                                    <td style="text-align:right">
                                        <input class="form-input" type="number" step="0.001" min="0.001"
                                               style="width:100%;text-align:right"
                                               t-att-data-key="line._key" data-line-field="product_qty"
                                               t-att-value="line.product_qty ?? 1"/>
                                    </td>
                                    <td>
                                        <select class="form-input" style="width:100%"
                                                t-att-data-key="line._key" data-line-field="product_uom_id">
                                            <option value="0">—</option>
                                            <t t-foreach="state.uoms" t-as="opt" t-key="opt.id">
                                                <option t-att-value="opt.id"
                                                        t-att-selected="getM2oId(line.product_uom_id) === opt.id ? true : undefined"
                                                        t-esc="opt.display"/>
                                            </t>
                                        </select>
                                    </td>
                                    <td style="text-align:center">
                                        <button class="btn btn-danger"
                                                style="padding:2px 8px;font-size:11px"
                                                t-att-data-key="line._key"
                                                t-on-click.stop="onRemoveLine">✕</button>
                                    </td>
                                </tr>
                            </t>
                        </tbody>
                    </table>
                    <div style="padding:8px 0">
                        <button class="btn" t-on-click.stop="onAddLine">Add a line</button>
                    </div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading:  true,
            error:    '',
            isNew:    !this.props.recordId,
            record:   { bom_type: 'normal', product_qty: 1 },
            lines:    [],
            deletedLineIds: [],
            products: [],
            uoms:     [],
        });
        this._nextKey = 1;
        onMounted(() => this.load());
    }

    get pageTitle() {
        if (this.state.isNew) return 'New Bill of Materials';
        const p = this.state.products.find(o => o.id === this.getM2oId(this.state.record.product_id));
        const name = p ? p.display : ('BOM #' + this.props.recordId);
        const ref  = this.state.record.code ? ' [' + this.state.record.code + ']' : '';
        return name + ref;
    }

    getM2oId(val) {
        if (!val && val !== 0) return 0;
        if (typeof val === 'number') return val;
        if (Array.isArray(val) && val.length > 0) return typeof val[0] === 'number' ? val[0] : 0;
        return 0;
    }

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const [recordData] = await Promise.all([
                this.props.recordId
                    ? RpcService.call('mrp.bom', 'read', [[this.props.recordId]],
                            { fields: ['product_id','code','bom_type','product_qty','product_uom_id','active'] })
                          .then(r => (Array.isArray(r) ? r[0] : r) || {})
                    : Promise.resolve({ bom_type: 'normal', product_qty: 1 }),
                this.loadOpts('product.product', 'products', ['id','name']),
                this.loadOpts('uom.uom',         'uoms',     ['id','name']),
            ]);
            this.state.record = recordData;
            this.state.deletedLineIds = [];
            if (this.props.recordId) await this.loadLines();
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async loadOpts(model, key, fields) {
        try {
            const recs = await RpcService.call(model, 'search_read', [[]], { fields, limit: 500 });
            this.state[key] = (Array.isArray(recs) ? recs : []).map(r => ({
                id: r.id, display: r.name || String(r.id),
            }));
        } catch (_) { this.state[key] = []; }
    }

    async loadLines() {
        try {
            const rows = await RpcService.call('mrp.bom.line', 'search_read',
                [[['bom_id', '=', this.props.recordId]]],
                { fields: ['id','bom_id','product_id','product_qty','product_uom_id','sequence'], limit: 500 });
            this.state.lines = (Array.isArray(rows) ? rows : []).map(r => ({
                _key: String(this._nextKey++), ...r,
            }));
        } catch (_) { this.state.lines = []; }
    }

    onAnyChange(e) {
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            const key = e.target.dataset.key;
            const val = e.target.tagName === 'SELECT' ? (parseInt(e.target.value) || 0) : e.target.value;
            const line = this.state.lines.find(l => l._key === key);
            if (line) line[lineField] = val;
            return;
        }
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.tagName === 'SELECT') {
            this.state.record[field] = parseInt(e.target.value) || 0;
        }
    }

    onAnyInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const lineField = e.target.dataset.lineField;
        if (lineField) {
            const key = e.target.dataset.key;
            const line = this.state.lines.find(l => l._key === key);
            if (line) line[lineField] = parseFloat(e.target.value) || e.target.value;
            return;
        }
        const field = e.target.dataset.field;
        if (field) this.state.record[field] = e.target.value;
    }

    onAddLine() {
        this.state.lines.push({
            _key:           String(this._nextKey++),
            id:             null,
            product_id:     0,
            product_qty:    1,
            product_uom_id: 0,
            sequence:       (this.state.lines.length + 1) * 10,
        });
    }

    onRemoveLine(e) {
        const key = e.currentTarget.dataset.key;
        const idx = this.state.lines.findIndex(l => l._key === key);
        if (idx < 0) return;
        const line = this.state.lines[idx];
        if (line.id) this.state.deletedLineIds.push(line.id);
        this.state.lines.splice(idx, 1);
    }

    collectRecord() {
        const r = this.state.record;
        return {
            product_id:     this.getM2oId(r.product_id)     || false,
            code:           r.code           || false,
            bom_type:       r.bom_type       || 'normal',
            product_qty:    parseFloat(r.product_qty) || 1,
            product_uom_id: this.getM2oId(r.product_uom_id) || false,
        };
    }

    async syncLines(bomId) {
        if (this.state.deletedLineIds.length) {
            await RpcService.call('mrp.bom.line', 'unlink', [this.state.deletedLineIds], {});
            this.state.deletedLineIds = [];
        }
        for (const line of this.state.lines) {
            const vals = {
                bom_id:         bomId,
                product_id:     this.getM2oId(line.product_id)     || false,
                product_qty:    parseFloat(line.product_qty) || 1,
                product_uom_id: this.getM2oId(line.product_uom_id) || false,
                sequence:       line.sequence || 10,
            };
            if (line.id) {
                await RpcService.call('mrp.bom.line', 'write', [[line.id], vals], {});
            } else {
                await RpcService.call('mrp.bom.line', 'create', [vals], {});
            }
        }
    }

    async onCreate() {
        try {
            const newId = await RpcService.call('mrp.bom', 'create', [this.collectRecord()], {});
            await this.syncLines(newId);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onSave() {
        try {
            await RpcService.call('mrp.bom', 'write', [[this.state.record.id], this.collectRecord()], {});
            await this.syncLines(this.state.record.id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onDelete() {
        try {
            await RpcService.call('mrp.bom', 'unlink', [[this.state.record.id]], {});
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// ERPSettingsView — company / banking / documents / email config
// ----------------------------------------------------------------
class ERPSettingsView extends Component {
    static template = xml`
        <div class="erp-settings-shell">
            <div class="erp-settings-header">
                <h2>ERP Settings</h2>
            </div>
            <div class="erp-tab-bar">
                <button t-attf-class="erp-tab{{ state.activeTab==='general'?' active':'' }}"
                        t-on-click="()=>this.setTab('general')">General</button>
                <button t-attf-class="erp-tab{{ state.activeTab==='banking'?' active':'' }}"
                        t-on-click="()=>this.setTab('banking')">Banking</button>
                <button t-attf-class="erp-tab{{ state.activeTab==='documents'?' active':'' }}"
                        t-on-click="()=>this.setTab('documents')">Documents</button>
                <button t-attf-class="erp-tab{{ state.activeTab==='email'?' active':'' }}"
                        t-on-click="()=>this.setTab('email')">Email</button>
                <button t-attf-class="erp-tab{{ state.activeTab==='countries'?' active':'' }}"
                        t-on-click="()=>this.loadCountriesTab()">Countries</button>
                <button t-attf-class="erp-tab{{ state.activeTab==='precision'?' active':'' }}"
                        t-on-click="()=>this.loadPrecisionTab()">Precision &amp; Currency</button>
            </div>
            <div class="erp-settings-body">
                <t t-if="state.loading">
                    <div class="loading">Loading settings…</div>
                </t>
                <t t-elif="state.error">
                    <div class="error" t-esc="state.error"/>
                </t>
                <t t-else="">
                    <!-- General Tab -->
                    <t t-if="state.activeTab==='general'">
                        <div class="erp-section">
                            <div class="erp-section-title">Company Information</div>
                            <div class="erp-field-grid">
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Company Name</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['company.name']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Registration No.</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.reg_number']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Address Line 1</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.addr1']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Address Line 2</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.addr2']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Address Line 3</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.addr3']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">City &amp; Country</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.city_country']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Phone</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['company.phone']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Email</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['company.email']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Website</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['company.website']"/>
                                </div>
                            </div>
                        </div>
                        <div class="erp-section">
                            <div class="erp-section-title">Invoice Defaults</div>
                            <div class="erp-field-grid">
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Currency Code</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.currency_code']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Payment Terms Days</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.payment_term_days']"/>
                                </div>
                            </div>
                        </div>
                        <div class="erp-save-row">
                            <button class="btn btn-primary" t-att-disabled="state.saving" t-on-click="onSave">
                                <t t-if="state.saving">Saving…</t><t t-else="">Save Changes</t>
                            </button>
                            <t t-if="state.saved"><span class="erp-saved-ok">Saved!</span></t>
                            <t t-if="state.saveError"><span class="erp-save-err" t-esc="state.saveError"/></t>
                        </div>
                    </t>

                    <!-- Banking Tab -->
                    <t t-if="state.activeTab==='banking'">
                        <div class="erp-section">
                            <div class="erp-section-title">Bank Account Details</div>
                            <div class="erp-field-grid">
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Account Holder Name</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.bank.account_name']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Account Number</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.bank.account_no']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Bank Name</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.bank.bank_name']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Bank Address</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.bank.bank_address']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">SWIFT Code</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['report.bank.swift_code']"/>
                                </div>
                            </div>
                        </div>
                        <div class="erp-save-row">
                            <button class="btn btn-primary" t-att-disabled="state.saving" t-on-click="onSave">
                                <t t-if="state.saving">Saving…</t><t t-else="">Save Changes</t>
                            </button>
                            <t t-if="state.saved"><span class="erp-saved-ok">Saved!</span></t>
                            <t t-if="state.saveError"><span class="erp-save-err" t-esc="state.saveError"/></t>
                        </div>
                    </t>

                    <!-- Documents Tab -->
                    <t t-if="state.activeTab==='documents'">
                        <div class="erp-section">
                            <div class="erp-section-title">Document Layout</div>
                            <div class="erp-field-grid">
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Paper Format</label>
                                    <select class="erp-field-input" t-model="state.cfg['report.paper_format']">
                                        <option value="A4">A4</option>
                                        <option value="Letter">Letter</option>
                                        <option value="A3">A3</option>
                                        <option value="Legal">Legal</option>
                                    </select>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Orientation</label>
                                    <select class="erp-field-input" t-model="state.cfg['report.orientation']">
                                        <option value="portrait">Portrait</option>
                                        <option value="landscape">Landscape</option>
                                    </select>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Font Family</label>
                                    <select class="erp-field-input" t-model="state.cfg['report.design.font_family']">
                                        <option value="Arial, sans-serif">Arial</option>
                                        <option value="'Times New Roman', serif">Times New Roman</option>
                                        <option value="Helvetica, sans-serif">Helvetica</option>
                                        <option value="Georgia, serif">Georgia</option>
                                    </select>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Accent Color</label>
                                    <input class="erp-field-input" type="color" t-att-value="state.cfg['report.design.accent_color']"
                                           t-on-input="ev=>this.state.cfg['report.design.accent_color']=ev.target.value"/>
                                </div>
                            </div>
                        </div>
                        <div class="erp-save-row">
                            <button class="btn btn-primary" t-att-disabled="state.saving" t-on-click="onSave">
                                <t t-if="state.saving">Saving…</t><t t-else="">Save Changes</t>
                            </button>
                            <t t-if="state.saved"><span class="erp-saved-ok">Saved!</span></t>
                            <t t-if="state.saveError"><span class="erp-save-err" t-esc="state.saveError"/></t>
                        </div>
                    </t>

                    <!-- Countries Tab -->
                    <t t-if="state.activeTab==='precision'">
                        <div class="erp-section">
                            <div class="erp-section-title">Decimal Precision</div>
                            <div class="erp-hint">
                                Controls how many decimals are <b>displayed</b> and where amounts are
                                rounded. Values are always stored at 6 decimal places, so changing
                                these never alters stored data — only how it is shown.
                                The invoice <b>total</b> always rounds to the currency's own decimals,
                                whatever is set here.
                            </div>
                            <t t-if="state.precLoading"><div class="loading">Loading…</div></t>
                            <t t-else="">
                                <div class="erp-field-grid">
                                    <t t-foreach="state.precisions" t-as="p" t-key="p.id">
                                        <div class="erp-field-row">
                                            <label class="erp-field-label" t-esc="p.name"/>
                                            <input class="erp-field-input" type="number"
                                                   min="0" max="6" step="1"
                                                   t-att-value="p.digits"
                                                   t-on-change="(ev)=>this.onPrecisionChange(p.id, ev.target.value)"/>
                                        </div>
                                    </t>
                                </div>
                            </t>
                        </div>

                        <div class="erp-section">
                            <div class="erp-section-title">Currencies</div>
                            <div class="erp-hint">
                                Rate = how many <b>base currency</b> units equal 1 unit of this
                                currency. With MYR as base, USD 4.70 means 1 USD = RM 4.70.
                                Documents record the rate they were booked at, so changing a rate
                                here never restates existing invoices.
                            </div>
                            <t t-if="state.currLoading"><div class="loading">Loading…</div></t>
                            <t t-else="">
                                <div class="erp-field-grid">
                                    <t t-foreach="state.currencies" t-as="c" t-key="c.id">
                                        <div class="erp-field-row">
                                            <label class="erp-field-label">
                                                <span t-esc="c.name"/>
                                                <span class="erp-country-code" t-esc="' (' + (c.symbol||'') + ')'"/>
                                                <t t-if="c.is_base"><span class="erp-saved-ok"> base</span></t>
                                            </label>
                                            <input class="erp-field-input" type="number" step="0.000001" min="0"
                                                   t-att-value="c.rate"
                                                   t-att-disabled="c.is_base ? true : undefined"
                                                   t-on-change="(ev)=>this.onRateChange(c.id, ev.target.value)"/>
                                        </div>
                                    </t>
                                </div>
                            </t>
                        </div>

                        <div class="erp-save-row">
                            <t t-if="state.precSaved"><span class="erp-saved-ok">Saved!</span></t>
                            <t t-if="state.precError"><span class="error" t-esc="state.precError"/></t>
                        </div>
                    </t>

                    <t t-if="state.activeTab==='countries'">
                        <div class="erp-section">
                            <div class="erp-section-title">Supported Countries</div>
                            <div class="erp-field-row" style="margin-bottom:8px;">
                                <input class="erp-field-input" type="text"
                                       placeholder="Search countries…"
                                       t-model="state.countrySearch"/>
                            </div>
                            <t t-if="state.countriesLoading">
                                <div class="loading">Loading countries…</div>
                            </t>
                            <t t-else="">
                                <div class="erp-countries-list">
                                    <t t-foreach="filteredCountries" t-as="co" t-key="co.id">
                                        <label class="erp-country-item">
                                            <input type="checkbox"
                                                   t-att-checked="co.active ? true : undefined"
                                                   t-on-change="(ev) => this.onCountryToggle(co.id, ev.target.checked)"/>
                                            <span t-esc="co.name"/>
                                            <span class="erp-country-code" t-esc="' (' + co.code + ')'"/>
                                        </label>
                                    </t>
                                </div>
                            </t>
                        </div>
                        <div class="erp-save-row">
                            <t t-if="state.countrySaved"><span class="erp-saved-ok">Saved!</span></t>
                            <t t-if="state.countrySaveError"><span class="erp-save-err" t-esc="state.countrySaveError"/></t>
                        </div>
                    </t>

                    <!-- Email Tab -->
                    <t t-if="state.activeTab==='email'">
                        <div class="erp-section">
                            <div class="erp-section-title">SMTP Configuration</div>
                            <div class="erp-field-grid">
                                <div class="erp-field-row">
                                    <label class="erp-field-label">SMTP Host</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['mail.smtp_host']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">SMTP Port</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['mail.smtp_port']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">SSL</label>
                                    <input type="checkbox" t-att-checked="state.cfg['mail.smtp_ssl']==='1'||state.cfg['mail.smtp_ssl']==='true'"
                                           t-on-change="ev=>this.state.cfg['mail.smtp_ssl']=ev.target.checked?'1':'0'"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">From Address</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['mail.from_address']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Username</label>
                                    <input class="erp-field-input" type="text" t-model="state.cfg['mail.smtp_user']"/>
                                </div>
                                <div class="erp-field-row">
                                    <label class="erp-field-label">Password</label>
                                    <input class="erp-field-input" type="password" t-model="state.cfg['mail.smtp_password']"/>
                                </div>
                            </div>
                        </div>
                        <div class="erp-save-row">
                            <button class="btn btn-primary" t-att-disabled="state.saving" t-on-click="onSave">
                                <t t-if="state.saving">Saving…</t><t t-else="">Save Changes</t>
                            </button>
                            <t t-if="state.saved"><span class="erp-saved-ok">Saved!</span></t>
                            <t t-if="state.saveError"><span class="erp-save-err" t-esc="state.saveError"/></t>
                        </div>
                    </t>
                </t>
            </div>
        </div>
    `;

    static ALL_KEYS = [
        'company.name','company.phone','company.email','company.website',
        'report.reg_number','report.addr1','report.addr2','report.addr3',
        'report.city_country','report.currency_code','report.payment_term_days',
        'report.bank.account_name','report.bank.account_no','report.bank.bank_name',
        'report.bank.bank_address','report.bank.swift_code',
        'report.paper_format','report.orientation',
        'report.design.font_family','report.design.accent_color',
        'mail.smtp_host','mail.smtp_port','mail.smtp_ssl',
        'mail.from_address','mail.smtp_user','mail.smtp_password',
    ];

    setup() {
        this.state = useState({
            activeTab:         'general',
            loading:           true,
            error:             '',
            saving:            false,
            saved:             false,
            saveError:         '',
            cfg:               {},
            cfgIds:            {},
            // Countries tab
            countries:         [],
            countriesLoading:  false,
            countrySearch:     '',
            countrySaved:      false,
            countrySaveError:  '',
            // P2 — Precision & Currency tab
            precisions:        [],
            precLoading:       false,
            currencies:        [],
            currLoading:       false,
            precSaved:         false,
            precError:         '',
        });
        onMounted(() => this.loadCfg());
    }

    get filteredCountries() {
        const q = (this.state.countrySearch || '').toLowerCase();
        if (!q) return this.state.countries;
        return this.state.countries.filter(c =>
            c.name.toLowerCase().includes(q) || c.code.toLowerCase().includes(q));
    }

    setTab(tab) { this.state.activeTab = tab; }

    // ---- Precision & Currency tab (P2, docs/048 §2) ----------------
    async loadPrecisionTab() {
        this.state.activeTab = 'precision';
        this.state.precError = '';
        if (this.state.precisions.length === 0) {
            this.state.precLoading = true;
            try {
                const rows = await RpcService.call('decimal.precision', 'search_read', [[]], {
                    fields: ['id', 'name', 'digits'], order: 'name ASC'
                });
                this.state.precisions = Array.isArray(rows) ? rows : [];
            } catch (e) {
                this.state.precError = e.message || 'Failed to load precision settings';
            } finally {
                this.state.precLoading = false;
            }
        }
        if (this.state.currencies.length === 0) {
            this.state.currLoading = true;
            try {
                const [rows, companies] = await Promise.all([
                    RpcService.call('res.currency', 'search_read', [[['active', '=', true]]], {
                        fields: ['id', 'name', 'symbol', 'rate', 'decimal_places'], order: 'name ASC'
                    }),
                    RpcService.call('res.company', 'search_read', [[]], {
                        fields: ['id', 'currency_id'], limit: 1
                    }),
                ]);
                // Mark the base currency so its rate is shown read-only — it is
                // 1.0 by definition and editing it would be meaningless.
                const baseRaw = companies?.[0]?.currency_id;
                const baseId  = Array.isArray(baseRaw) ? baseRaw[0] : baseRaw;
                this.state.currencies = (Array.isArray(rows) ? rows : [])
                    .map(c => ({ ...c, is_base: c.id === baseId }));
            } catch (e) {
                this.state.precError = e.message || 'Failed to load currencies';
            } finally {
                this.state.currLoading = false;
            }
        }
    }

    async onPrecisionChange(id, raw) {
        const digits = parseInt(raw, 10);
        const row = this.state.precisions.find(p => p.id === id);
        this.state.precSaved = false;
        this.state.precError = '';
        if (!Number.isInteger(digits) || digits < 0 || digits > 6) {
            this.state.precError = 'Decimals must be a whole number between 0 and 6.';
            return;
        }
        const previous = row ? row.digits : digits;
        if (row) row.digits = digits;               // optimistic
        try {
            await RpcService.call('decimal.precision', 'write', [[id], { digits }], {});
            this.state.precSaved = true;
            // The server drops its fields_get cache on write, so re-fetching
            // metadata anywhere in the app now returns the new `digits`.
            setTimeout(() => { this.state.precSaved = false; }, 4000);
        } catch (e) {
            if (row) row.digits = previous;         // roll back the optimistic edit
            this.state.precError = e.message || 'Failed to save';
        }
    }

    async onRateChange(id, raw) {
        const rate = parseFloat(raw);
        const row = this.state.currencies.find(c => c.id === id);
        this.state.precSaved = false;
        this.state.precError = '';
        if (!Number.isFinite(rate) || rate <= 0) {
            this.state.precError = 'Rate must be a positive number.';
            return;
        }
        const previous = row ? row.rate : rate;
        if (row) row.rate = rate;
        try {
            await RpcService.call('res.currency', 'write', [[id], { rate }], {});
            this.state.precSaved = true;
            setTimeout(() => { this.state.precSaved = false; }, 4000);
        } catch (e) {
            if (row) row.rate = previous;
            this.state.precError = e.message || 'Failed to save';
        }
    }

    async loadCountriesTab() {
        this.state.activeTab = 'countries';
        if (this.state.countries.length > 0) return; // already loaded
        this.state.countriesLoading = true;
        try {
            const list = await RpcService.call('res.country', 'search_read', [[]], {
                fields: ['id', 'name', 'code', 'active'], limit: 300, order: 'name ASC'
            });
            this.state.countries = Array.isArray(list) ? list : [];
        } catch (e) {
            this.state.countrySaveError = e.message || 'Failed to load countries';
        } finally {
            this.state.countriesLoading = false;
        }
    }

    async onCountryToggle(id, active) {
        // Optimistic UI update
        const co = this.state.countries.find(c => c.id === id);
        if (co) co.active = active;
        this.state.countrySaved      = false;
        this.state.countrySaveError  = '';
        try {
            await RpcService.call('res.country', 'write', [[id], { active }], {});
            this.state.countrySaved = true;
            setTimeout(() => { if (this.state) this.state.countrySaved = false; }, 1500);
        } catch (e) {
            if (co) co.active = !active; // revert
            this.state.countrySaveError = e.message || 'Save failed';
        }
    }

    async loadCfg() {
        this.state.loading = true;
        this.state.error = '';
        try {
            const keys = ERPSettingsView.ALL_KEYS;
            // Init defaults
            const cfg = {};
            const cfgIds = {};
            for (const k of keys) cfg[k] = '';
            const params = await RpcService.call(
                'ir.config.parameter', 'search_read',
                [[['key', 'in', keys]]],
                { fields: ['id', 'key', 'value'], limit: 200 });
            if (Array.isArray(params)) {
                for (const p of params) {
                    cfg[p.key]    = p.value || '';
                    cfgIds[p.key] = p.id;
                }
            }
            this.state.cfg    = cfg;
            this.state.cfgIds = cfgIds;
        } catch (e) {
            this.state.error = e.message || 'Failed to load settings';
        } finally {
            this.state.loading = false;
        }
    }

    async onSave() {
        this.state.saving    = true;
        this.state.saved     = false;
        this.state.saveError = '';
        try {
            for (const key of ERPSettingsView.ALL_KEYS) {
                const value = this.state.cfg[key] || '';
                const id    = this.state.cfgIds[key];
                if (id) {
                    await RpcService.call('ir.config.parameter', 'write', [[id], { value }], {});
                } else {
                    const newId = await RpcService.call('ir.config.parameter', 'create', [{ key, value }], {});
                    this.state.cfgIds[key] = newId;
                }
            }
            this.state.saved = true;
            setTimeout(() => { if (this.state) this.state.saved = false; }, 2500);
        } catch (e) {
            this.state.saveError = e.message || 'Save failed';
        } finally {
            this.state.saving = false;
        }
    }
}

// ----------------------------------------------------------------
// DocumentLayoutEditor constants
// ----------------------------------------------------------------
const DLE_BLOCK_DEFS = [
    // Content blocks
    { type:'doc_header',    label:'Document Header',    icon:'\u25F0', group:'content' },
    { type:'logo',          label:'Company Logo',       icon:'\uD83D\uDDBC', group:'content' },
    { type:'from_address',  label:'Company Address',    icon:'\u2302', group:'content' },
    { type:'to_address',    label:'Recipient Address',  icon:'\u2709', group:'content' },
    { type:'doc_details',   label:'Document Details',   icon:'\u229E', group:'content' },
    { type:'items_table',   label:'Items Table',        icon:'\u25A4', group:'content' },
    { type:'totals',        label:'Totals Summary',     icon:'\u2211', group:'content' },
    { type:'payment_terms', label:'Payment Terms',      icon:'\u25F7', group:'content' },
    { type:'bank_details',  label:'Bank Details',       icon:'\u2295', group:'content' },
    { type:'notes',         label:'Notes',              icon:'\u270E', group:'content' },
    { type:'text_block',    label:'Text Block',         icon:'\u00B6', group:'content' },
    { type:'footer_bar',    label:'Footer Bar',         icon:'\u25AC', group:'content' },
    { type:'footer_sep',    label:'Footer Separator',   icon:'\u2501', group:'content' },
    { type:'page_fill',     label:'Page Fill Spacer',   icon:'\u21F3', group:'layout' },
    // Layout blocks
    { type:'separator',     label:'Separator Line',     icon:'\u2015', group:'layout' },
    { type:'spacer',        label:'Spacer / Gap',       icon:'\u2B1C', group:'layout' },
    { type:'row_start',     label:'Row Start',          icon:'\u2997', group:'layout' },
    { type:'col_break',     label:'Column Break',       icon:'\u2998', group:'layout' },
    { type:'row_end',       label:'Row End',            icon:'\u29D8', group:'layout' },
];

const DLE_DEFAULT_BLOCKS = {
    'account.move':   ['logo','doc_header','from_address','to_address','doc_details','items_table','totals','payment_terms','bank_details','footer_bar'],
    'sale.order':     ['logo','doc_header','from_address','to_address','doc_details','items_table','totals','payment_terms','notes','footer_bar'],
    'purchase.order': ['logo','doc_header','from_address','to_address','doc_details','items_table','totals','notes','footer_bar'],
    'stock.picking':  ['logo','doc_header','from_address','to_address','doc_details','items_table','footer_bar'],
};

// Default column placement for a fresh layout — same for every document type.
// Row 1: logo on the left, company name + address on the right.
// Row 2: recipient on the left, document details on the right.
const DLE_DEFAULT_PROPS = {
    logo:         { col:'left',  col_width:28, width:45, height:22 },
    doc_header:   { col:'right' },
    from_address: { col:'right' },
    to_address:   { col:'left',  col_width:55 },
    doc_details:  { col:'right' },
};

// Blocks that start switched off in a fresh layout (the logo needs a URL first,
// and an empty logo box in every PDF would be worse than no logo at all).
const DLE_DEFAULT_HIDDEN = new Set(['logo']);

function dleDefaultBlocks(model) {
    return (DLE_DEFAULT_BLOCKS[model] || []).map(t => ({
        type:    t,
        visible: !DLE_DEFAULT_HIDDEN.has(t),
        props:   { ...(DLE_DEFAULT_PROPS[t] || {}) },
    }));
}

const DLE_DOC_TYPES = [
    { model:'account.move',   label:'Invoice' },
    { model:'sale.order',     label:'Sales Order' },
    { model:'purchase.order', label:'Purchase Order' },
    { model:'stock.picking',  label:'Delivery' },
];

// ---- Shared CSS embedded in generated templates ----
const DLE_CSS = `<style>
@page { size: A4; margin: 0; }
* { box-sizing: border-box; }
body { font-family: Arial, Helvetica, sans-serif; font-size: 10pt; color: #333333; line-height: 1.5; margin: 0; padding: 15mm 18mm 20mm 18mm; display: flex; flex-direction: column; min-height: 257mm; }
.page-fill-spacer { flex: 1; }
.clearfix { overflow: hidden; }
.hdr-left { float: left; width: 40%; }
.hdr-right { float: right; width: 58%; }
.company-name { font-weight: bold; font-size: 11pt; }
.company-detail { font-size: 10pt; }
.info-row { overflow: hidden; margin-top: 10mm; margin-bottom: 6mm; }
/* buyer-col / meta-col no longer float: body is a flex column, so floats between
   sibling blocks never pair up. Side-by-side is done with .dle-row / .dle-col. */
.buyer-col { width: 100%; margin-top: 8mm; margin-bottom: 4mm; }
.meta-col { width: 100%; margin-top: 8mm; margin-bottom: 4mm; }
/* Columns use display:table, NOT flexbox. wkhtmltopdf's QtWebKit ignores
   display:flex outright (the columns silently collapse into a stack), and it
   has no support for the flexbox gap property either. CSS tables render
   identically in Chrome and QtWebKit, so the preview matches the PDF. */
.dle-row { display: table; width: 100%; table-layout: fixed; margin-bottom: 4mm; }
.dle-col { display: table-cell; vertical-align: top; }
.dle-col > .buyer-col, .dle-col > .meta-col, .dle-col > .hdr-left, .dle-col > .hdr-right { float: none; width: 100%; }
.buyer-name { font-weight: bold; font-size: 11pt; }
.doc-title { font-weight: bold; font-size: 14pt; margin-bottom: 2mm; }
.meta-line { font-size: 10pt; }
.meta-lbl { font-weight: bold; }
.currency-note { margin-bottom: 3mm; font-size: 10pt; }
.lines-table { width: 100%; border-collapse: collapse; margin-bottom: 4mm; border: 0.5pt solid #cccccc; }
.lines-table thead th { background-color: #4a4a4a; color: #ffffff; font-weight: bold; text-transform: uppercase; padding: 6px 8px; font-size: 10pt; text-align: left; }
.lines-table thead th.r { text-align: right; }
.lines-table thead th.c { text-align: center; }
.lines-table tbody td { padding: 6px 8px; border-bottom: 0.5pt solid #cccccc; font-size: 10pt; vertical-align: top; }
.lines-table tbody td.r { text-align: right; }
.lines-table tbody td.c { text-align: center; }
.col-desc { width: 55%; }
.col-qty { width: 12%; }
.col-uom { width: 10%; }
.col-price { width: 16%; }
.col-amount { width: 17%; }
.row-line_section td { font-weight: bold; background-color: #f5f5f5; }
.row-line_note td { font-style: italic; color: #666666; }
.totals-wrap { overflow: hidden; margin-bottom: 8mm; }
.totals-table { float: right; width: 45%; border-collapse: collapse; }
.totals-table td { padding: 5px 8px; font-size: 10pt; }
.totals-table .t-lbl { text-align: right; font-weight: bold; padding-right: 12px; }
.totals-table .t-val { text-align: right; white-space: nowrap; }
.totals-table .row-total td { background-color: #4a4a4a; color: #ffffff; font-weight: bold; }
.payment-terms { margin-bottom: 5mm; font-size: 11pt; }
.bank-details { font-size: 9.5pt; line-height: 1.7; }
.notes-block { margin-bottom: 5mm; font-size: 10pt; }
.text-block { margin: 4mm 0; font-size: 10pt; }
.doc-separator { border: none; border-top: 1.5pt solid #888888; margin: 5mm 0; }
.page-footer { position: fixed; bottom: 0; left: 0; right: 0; background-color: #4a4a4a; color: #ffffff; text-align: center; padding: 6px 0; font-size: 9pt; }
.print-btn { display: inline-block; margin-top: 10mm; padding: 8px 20px; background: #4a4a4a; color: #fff; border: none; font-size: 10pt; cursor: pointer; }
@media print { .print-btn { display: none; } }
</style>`;

// ---- Property schema helpers ----
const _D = (label) => ({ type: 'divider', label });
const _FONT_OPTS = [
    { v: '',                         l: 'Default' },
    { v: 'Arial, sans-serif',        l: 'Arial' },
    { v: 'Georgia, serif',           l: 'Georgia' },
    { v: "'Times New Roman', serif", l: 'Times New Roman' },
    { v: 'Verdana, sans-serif',      l: 'Verdana' },
    { v: 'Tahoma, sans-serif',       l: 'Tahoma' },
    { v: "'Courier New', monospace", l: 'Courier New' },
];
const _ALIGN_OPTS  = [{ v:'',l:'Default'},{v:'left',l:'Left'},{v:'center',l:'Center'},{v:'right',l:'Right'},{v:'justify',l:'Justify'}];
const _BSTYLE_OPTS = [{ v:'solid',l:'Solid'},{v:'dashed',l:'Dashed'},{v:'dotted',l:'Dotted'}];

function _typo()   { return [_D('Typography'),
    { key:'font_size',   label:'Font Size',   type:'number', unit:'pt', min:6,  max:48 },
    { key:'font_family', label:'Font',        type:'select', options:_FONT_OPTS },
    { key:'color',       label:'Text Color',  type:'color' },
    { key:'text_align',  label:'Align',       type:'select', options:_ALIGN_OPTS },
    { key:'bold',        label:'Bold',        type:'boolean' },
    { key:'italic',      label:'Italic',      type:'boolean' },
]; }
function _spac()   { return [_D('Spacing'),
    { key:'padding',       label:'Padding',      type:'number', unit:'mm', min:0, max:20 },
    { key:'margin_bottom', label:'Margin Below', type:'number', unit:'mm', min:0, max:30 },
]; }
function _bgbd()   { return [_D('Background & Border'),
    { key:'bg_color',     label:'Background',   type:'color' },
    { key:'border_width', label:'Border Width', type:'number', unit:'pt', min:0, max:8 },
    { key:'border_color', label:'Border Color', type:'color' },
    { key:'border_style', label:'Border Style', type:'select', options:_BSTYLE_OPTS },
]; }
const _COL_OPTS = [
    { v:'full',  l:'Full width'   },
    { v:'left',  l:'Left column'  },
    { v:'right', l:'Right column' },
];
const _VALIGN_OPTS = [{ v:'',l:'Top'},{v:'center',l:'Middle'},{v:'end',l:'Bottom'}];

// Column Layout — lets any block share a horizontal band with its neighbours.
// A band opens at the first Left/Right block and closes when the next Left block
// begins a new one, so several blocks can stack inside one column.
function _cols() { return [_D('Column Layout'),
    { key:'col',        label:'Placement',      type:'select', options:_COL_OPTS },
    { key:'col_width',  label:'Column Size',    type:'number', unit:'%',  min:10, max:90 },
    { key:'col_gap',    label:'Column Gap',     type:'number', unit:'mm', min:0,  max:30 },
    { key:'col_valign', label:'Vertical Align', type:'select', options:_VALIGN_OPTS },
    { key:'_colinfo',   type:'label',
      label:'Blocks paired side by side must sit next to each other in the block list. Column Size sets the LEFT column — the right column takes the rest. Gap and Vertical Align are read from the first block of the pair. A column with nothing opposite it falls back to full width.' },
]; }

const DLE_PROP_DEFS = {
    doc_header: [
        ..._typo(), ..._spac(), ..._bgbd(),
        _D('Accent Line'),
        { key:'show_line',  label:'Show Line',  type:'boolean' },
        { key:'line_color', label:'Line Color', type:'color'   },
        ..._cols(),
    ],
    logo: [
        _D('Image'),
        { key:'src',    label:'Logo URL', type:'text', placeholder:'https://… or data:image/png;base64,…' },
        { key:'width',  label:'Max Width',  type:'number', unit:'mm', min:10, max:150 },
        { key:'height', label:'Max Height', type:'number', unit:'mm', min:5,  max:80  },
        { key:'logo_align', label:'Align', type:'select', options:[
            { v:'',       l:'Left'   },
            { v:'center', l:'Center' },
            { v:'right',  l:'Right'  },
        ]},
        ..._spac(), ..._cols(),
    ],
    from_address:  [..._typo(), ..._spac(), ..._bgbd(), ..._cols()],
    to_address:    [..._typo(), ..._spac(), ..._bgbd(), ..._cols()],
    doc_details:   [..._typo(), ..._spac(), ..._bgbd(), ..._cols()],
    items_table: [
        ..._typo(), ..._spac(),
        _D('Table Header'),
        { key:'header_bg',    label:'Header Background',  type:'color' },
        { key:'header_color', label:'Header Text Color',  type:'color' },
        _D('Table Body'),
        { key:'alt_row_bg',   label:'Alternate Row Bg',   type:'color' },
        { key:'cell_border',  label:'Cell Border Color',  type:'color' },
        ..._bgbd(), ..._cols(),
    ],
    totals: [
        ..._typo(), ..._spac(), ..._bgbd(),
        _D('Total Row'),
        { key:'total_bg',    label:'Total Background', type:'color' },
        { key:'total_color', label:'Total Text Color', type:'color' },
        ..._cols(),
    ],
    payment_terms: [..._typo(), ..._spac(), ..._bgbd(), ..._cols()],
    bank_details:  [..._typo(), ..._spac(), ..._bgbd(), ..._cols()],
    notes:         [..._typo(), ..._spac(), ..._bgbd(), ..._cols()],
    text_block: [
        _D('Content'),
        { key:'content', label:'Text Content', type:'textarea' },
        ..._typo(), ..._spac(), ..._bgbd(), ..._cols(),
    ],
    footer_bar: [
        _D('Content'),
        { key: 'text_source', label: 'Text Source', type: 'select', options: [
            { v: 'custom',  l: 'Custom Text' },
            { v: 'website', l: 'Website (from Report Settings)' },
            { v: 'none',    l: 'None (page numbers only)' },
        ]},
        { key: 'content', label: 'Custom Text', type: 'text', placeholder: 'e.g. www.example.com' },
        _D('Page Numbers'),
        { key: 'show_page_num', label: 'Show Page Numbers', type: 'boolean' },
        { key: 'page_num_fmt',  label: 'Format', type: 'select', options: [
            { v: 'Page {p} of {t}', l: 'Page X of Y' },
            { v: '{p} / {t}',       l: 'X / Y' },
            { v: 'Page {p}',        l: 'Page X' },
            { v: '{p}',             l: 'X only' },
        ]},
        _D('Separator Line'),
        { key: 'top_sep',       label: 'Show Line',   type: 'boolean' },
        { key: 'top_sep_color', label: 'Line Color',  type: 'color'   },
        { key: 'line_width',    label: 'Line Weight', type: 'number', unit: 'pt', min: 0.25, max: 4 },
        ..._typo(), ..._spac(), ..._bgbd()
    ],
    separator: [
        _D('Line Style'),
        { key:'color',         label:'Color',        type:'color' },
        { key:'thickness',     label:'Thickness',    type:'number', unit:'pt', min:0.5, max:10 },
        { key:'margin_top',    label:'Margin Top',   type:'number', unit:'mm', min:0,   max:20 },
        { key:'margin_bottom', label:'Margin Below', type:'number', unit:'mm', min:0,   max:20 },
    ],
    footer_sep: [
        _D('Line Style'),
        { key:'color',         label:'Color',          type:'color' },
        { key:'thickness',     label:'Thickness',      type:'number', unit:'pt',  min:0.5, max:10 },
        { key:'above_footer',  label:'Above Footer',   type:'number', unit:'mm',  min:4,   max:30 },
    ],
    page_fill: [
        _D('Page Fill Spacer'),
        { key:'_info', label:'Fills remaining page space above the footer. Place it just before the Footer Bar block.', type:'label' },
    ],
    spacer:    [_D('Size'), { key:'height', label:'Height', type:'number', unit:'mm', min:1, max:80 }],
    row_start: [_D('Row'), { key:'flex', label:'1st Column Width', type:'number', unit:'flex', min:1, max:10 },
                            { key:'gap',  label:'Column Gap',       type:'number', unit:'mm',   min:0, max:30 }],
    col_break: [_D('Column'), { key:'flex', label:'Column Width', type:'number', unit:'flex', min:1, max:10 }],
    row_end:   [],
};

// Build inline CSS style string from a props object
function dleStyleStr(p) {
    if (!p) return '';
    const s = [];
    if (p.font_size)                               s.push(`font-size:${p.font_size}pt`);
    if (p.font_family)                             s.push(`font-family:${p.font_family}`);
    if (p.color)                                   s.push(`color:${p.color}`);
    if (p.bg_color)                                s.push(`background:${p.bg_color}`);
    if (p.text_align)                              s.push(`text-align:${p.text_align}`);
    if (p.padding       != null && p.padding      !== '') s.push(`padding:${p.padding}mm`);
    if (p.margin_bottom != null && p.margin_bottom!== '') s.push(`margin-bottom:${p.margin_bottom}mm`);
    if (p.border_width  && p.border_width > 0)
        s.push(`border:${p.border_width}pt ${p.border_style || 'solid'} ${p.border_color || '#cccccc'}`);
    if (p.bold)   s.push('font-weight:bold');
    if (p.italic) s.push('font-style:italic');
    return s.join(';');
}

// ---- Per-block HTML snippet generator ----
// Every block must produce visible HTML so show/hide actually works.
// to_address and doc_details use floats; dleBuildHtml injects a clearfix after them.
function dleBlockHtml(type, model, props) {
    props = props || {};
    const st = dleStyleStr(props);
    const sa = st ? ` style="${st}"` : '';   // style attribute fragment

    switch (type) {
        case 'doc_header': {
            const lineHtml = props.show_line
                ? `<div style="border-top:2pt solid ${props.line_color || '#4a4a4a'};margin:2mm 0;"></div>` : '';
            return `<div${sa}>
  <div class="company-name">{{company_name}} ({{company_reg}})</div>
</div>${lineHtml}`;
        }

        case 'logo':
            return dleLogoHtml(props);

        case 'from_address':
            return `<div style="margin-bottom:4mm;${st}">
  <div class="company-detail">{{company_addr1}}</div>
  <div class="company-detail">{{company_addr2}}</div>
  <div class="company-detail">{{company_addr3}}</div>
  <div class="company-detail">{{company_city_country}}</div>
  <div class="company-detail">Tel: {{company_phone}}</div>
</div>`;

        case 'to_address':
            return `<div class="buyer-col"${sa}>
  <div class="buyer-name">{{partner_name}}</div>
  <div>{{partner_street}}</div>
  <div>{{partner_city}}</div>
  <div>{{partner_phone}}</div>
  <div>Attn: {{attn_name}}</div>
</div>`;

        case 'doc_details': {
            const lines = {
                'account.move':   `  <div class="meta-line"><span class="meta-lbl">Invoice No. :</span> {{doc_number}}</div>\n  <div class="meta-line"><span class="meta-lbl">Invoice Date :</span> {{doc_date}}</div>\n  <div class="meta-line"><span class="meta-lbl">Due Date :</span> {{doc_date_due}}</div>`,
                'sale.order':     `  <div class="meta-line"><span class="meta-lbl">Order No. :</span> {{doc_number}}</div>\n  <div class="meta-line"><span class="meta-lbl">Order Date :</span> {{doc_date}}</div>\n  <div class="meta-line"><span class="meta-lbl">Valid Until :</span> {{validity_date}}</div>`,
                'purchase.order': `  <div class="meta-line"><span class="meta-lbl">PO No. :</span> {{doc_number}}</div>\n  <div class="meta-line"><span class="meta-lbl">Order Date :</span> {{doc_date}}</div>\n  <div class="meta-line"><span class="meta-lbl">Expected :</span> {{date_planned}}</div>`,
                'stock.picking':  `  <div class="meta-line"><span class="meta-lbl">DO No. :</span> {{doc_number}}</div>\n  <div class="meta-line"><span class="meta-lbl">Date :</span> {{doc_date}}</div>\n  <div class="meta-line"><span class="meta-lbl">Origin :</span> {{origin}}</div>`,
            }[model] || '';
            return `<div class="meta-col"${sa}>
  <div class="doc-title">{{document_title}}</div>
${lines}
</div>`;
        }

        case 'items_table': {
            const hBg    = props.header_bg    || '';
            const hClr   = props.header_color || '';
            const altBg  = props.alt_row_bg   || '';
            const cellBd = props.cell_border  || '';
            const thSt   = (hBg || hClr) ? ` style="${hBg ? `background:${hBg};` : ''}${hClr ? `color:${hClr};` : ''}"` : '';
            let overrides = '';
            if (altBg)  overrides += `.lines-table tbody tr:nth-child(even) td{background:${altBg};}`;
            if (cellBd) overrides += `.lines-table,.lines-table tbody td{border-color:${cellBd};}`;
            const styleBlk = overrides ? `<style>${overrides}</style>` : '';
            const wrap  = `${styleBlk}<div${sa}><div class="currency-note">All Amount Stated in - {{currency_code}}</div>`;
            if (model === 'stock.picking') {
                return wrap + `<table class="lines-table">
  <thead><tr>
    <th class="col-desc"${thSt}>DESCRIPTION</th>
    <th class="col-qty c"${thSt}>DEMAND</th>
    <th class="col-qty c"${thSt}>DONE</th>
    <th class="col-uom"${thSt}>UOM</th>
  </tr></thead>
  <tbody>
    {{#each lines}}
    <tr><td>{{product_name}}</td><td class="c">{{demand}}</td><td class="c">{{done}}</td><td>{{uom}}</td></tr>
    {{/each}}
  </tbody>
</table></div>`;
            }
            return wrap + `<table class="lines-table">
  <thead><tr>
    <th class="col-desc"${thSt}>DESCRIPTION</th>
    <th class="col-qty c"${thSt}>QUANTITY</th>
    <th class="col-uom"${thSt}>UOM</th>
    <th class="col-price r"${thSt}>UNIT PRICE</th>
    <th class="col-amount r"${thSt}>AMOUNT</th>
  </tr></thead>
  <tbody>
    {{#each lines}}
    <tr class="row-{{line_type}}">
      <td>{{product_name}}</td><td class="c">{{qty}}</td><td>{{uom}}</td>
      <td class="r">{{price_unit}}</td><td class="r">{{subtotal}}</td>
    </tr>
    {{/each}}
  </tbody>
</table></div>`;
        }

        case 'totals': {
            const tBg  = props.total_bg    || '#4a4a4a';
            const tClr = props.total_color || '#ffffff';
            return `<div class="totals-wrap"${sa}>
  <table class="totals-table">
    <tr><td class="t-lbl">Subtotal</td><td class="t-val">{{currency_code}} {{amount_untaxed}}</td></tr>
    <tr><td class="t-lbl">Tax</td><td class="t-val">{{currency_code}} {{amount_tax}}</td></tr>
    <tr style="background:${tBg};color:${tClr};">
      <td class="t-lbl" style="color:${tClr};">Total</td>
      <td class="t-val" style="color:${tClr};">{{currency_code}} {{amount_total}}</td>
    </tr>
  </table>
</div>`;
        }

        case 'payment_terms':
            return `<div class="payment-terms"${sa}><strong>Payment terms:</strong> {{payment_term_days}} Days</div>`;

        case 'bank_details':
            return `<div class="bank-details"${sa}>
  <div>TT Transfer Payable to</div>
  <div>Account Name : {{bank_account_name}}</div>
  <div>Account No. : {{bank_account_no}}</div>
  <div>Bank Name : {{bank_name}}</div>
  <div>Bank Address : {{bank_address}}</div>
  <div>Bank SWIFT Code : {{bank_swift}}</div>
</div>`;

        case 'notes':
            return `<div class="notes-block"${sa}><strong>Notes:</strong> {{notes}}</div>`;

        case 'separator': {
            const color = props.color     || '#888888';
            const thick = props.thickness || 1.5;
            const mt    = (props.margin_top    != null && props.margin_top    !== '') ? props.margin_top    : 5;
            const mb    = (props.margin_bottom != null && props.margin_bottom !== '') ? props.margin_bottom : 5;
            return `<hr style="border:none;border-top:${thick}pt solid ${color};margin:${mt}mm 0 ${mb}mm;">`;
        }

        case 'footer_sep': {
            // A separator line fixed just above the footer bar.
            // Uses position:fixed so it stays at the bottom of every page above the footer.
            const color = props.color     || '#888888';
            const thick = props.thickness || 0.5;
            const above = (props.above_footer != null && props.above_footer !== '') ? props.above_footer : 8;
            return `<hr style="position:fixed;left:0;right:0;bottom:${above}mm;border:none;border-top:${thick}pt solid ${color};margin:0;width:100%;z-index:99;">`;
        }

        case 'spacer':
            return `<div class="doc-spacer"></div>`; // height handled in dleBuildHtml

        case 'text_block': {
            const content = props.content || '{{text_content}}';
            return `<div class="text-block"${sa}>${content}</div>`;
        }

        case 'page_fill':
            // Adaptive spacer: grows to consume remaining vertical space on the page,
            // pushing the footer_bar to the bottom. Uses flex-grow inside a flex body.
            return `<div class="page-fill-spacer"></div>`;

        case 'row_start':
        case 'col_break':
        case 'row_end':
            return '';

        case 'footer_bar': {
            const textSource   = props.text_source || 'custom';
            const showPageNum  = props.show_page_num !== false; // default true
            const pageNumFmt   = props.page_num_fmt  || 'Page {p} of {t}';
            const lineWidth    = props.line_width    || 1.5;

            // Determine visible text (this is the ONLY content inside .page-footer —
            // the PDF backend extracts it cleanly without any preview spans).
            let footerText = '';
            if (textSource === 'custom')       footerText = props.content || '{{company_website}}';
            else if (textSource === 'website') footerText = '{{company_website}}';
            // 'none' → no text

            // Build separator line style on the div
            let fStyle = st;
            if (props.top_sep) {
                const sepCol = props.top_sep_color || '#888888';
                fStyle = (fStyle ? fStyle + ';' : '') + `border-top:${lineWidth}pt solid ${sepCol}`;
            }
            const fSa = fStyle ? ` style="${fStyle}"` : '';

            // Page number preview: rendered OUTSIDE the .page-footer div (class dle-pg-prev)
            // so the backend HTML extraction only sees clean footer text.
            // The PDF route hides .dle-pg-prev via injected CSS.
            const pgColor  = props.color || '#ffffff';
            const pgPrev   = showPageNum
                ? `<span class="dle-pg-prev" style="position:fixed;bottom:6px;right:12px;` +
                  `font-size:9pt;color:${pgColor};opacity:0.85;pointer-events:none;">` +
                  pageNumFmt.replace('{p}', 'n').replace('{t}', 'N') + `</span>`
                : '';

            // Embed footer config as data attrs for reference (PDF backend uses ir_report_template)
            const dataAttrs = [
                `data-text-source="${textSource}"`,
                `data-show-page-num="${showPageNum ? '1' : '0'}"`,
                `data-page-num-fmt="${pageNumFmt}"`,
                props.top_sep ? `data-top-sep="1"` : '',
                props.top_sep ? `data-top-sep-color="${props.top_sep_color || '#888888'}"` : '',
                props.top_sep ? `data-line-width="${lineWidth}"` : '',
            ].filter(Boolean).join(' ');

            return `<div class="page-footer"${fSa} ${dataAttrs}>${footerText}</div>${pgPrev}
<button class="print-btn" onclick="window.print()">Print / Save as PDF</button>`;
        }

        default:
            return '';
    }
}

// Blocks that may take part in the automatic two-column bands
const DLE_COL_BLOCKS = new Set([
    'doc_header', 'logo', 'from_address', 'to_address', 'doc_details',
    'items_table', 'totals', 'payment_terms', 'bank_details', 'notes', 'text_block',
]);

// 'left' | 'right' for a block that opted into a column band, otherwise null
function dleColSide(blk) {
    if (!blk || blk.visible === false)          return null;
    if (!DLE_COL_BLOCKS.has(blk.type))          return null;
    const c = (blk.props || {}).col;
    return (c === 'left' || c === 'right') ? c : null;
}

// Maps the Vertical Align prop onto table-cell vertical-align ('' => top, via CSS)
const _VALIGN_CSS = { center: 'middle', end: 'bottom' };

// ---- Logo block ----
function dleLogoHtml(props) {
    props = props || {};
    const h     = props.height || 20;
    const w     = props.width  || 50;
    const src   = props.src    || '';
    const align = props.logo_align ? `text-align:${props.logo_align};` : '';
    const pad   = (props.padding != null && props.padding !== '') ? `${props.padding}mm` : '2mm 0';
    const mb    = (props.margin_bottom != null && props.margin_bottom !== '') ? `margin-bottom:${props.margin_bottom}mm;` : '';
    const inner = src
        ? `<img src="${src}" style="max-height:${h}mm; max-width:${w}mm;" alt="Logo"/>`
        // Placeholder uses inline-block + line-height (not flex) so it honours text-align
        : `<div style="display:inline-block; width:${w}mm; height:${h}mm; line-height:${h}mm; text-align:center; background:#f5f5f5; border:1pt dashed #bbb; color:#aaa; font-size:9pt; font-style:italic;">Company Logo</div>`;
    return `<div style="padding:${pad};${mb}${align}">${inner}</div>`;
}

// ---- Render one content block (no column wrapping) ----
function dleRenderBlock(blk, model) {
    const props = blk.props || {};
    if (blk.type === 'spacer') return `<div style="height:${props.height || 8}mm;"></div>`;
    return dleBlockHtml(blk.type, model, props);
}

// Emit one CSS-table row. `cols` = [{ html, width, padLeft, padRight }].
function dleRowHtml(cols, valign) {
    const va = valign ? `vertical-align:${valign};` : '';
    const cells = cols.map(c => {
        const st = [va, c.width != null ? `width:${c.width}%;` : '',
                    c.padLeft  ? `padding-left:${c.padLeft}mm;`   : '',
                    c.padRight ? `padding-right:${c.padRight}mm;` : ''].join('');
        return `<div class="dle-col"${st ? ` style="${st}"` : ''}>\n${c.html}\n</div>`;
    });
    return `<div class="dle-row">\n${cells.join('\n')}\n</div>`;
}

// ---- Assemble full HTML document from block list ----
function dleBuildHtml(blocks, model) {
    const label = { 'account.move':'Invoice', 'sale.order':'Sales Order', 'purchase.order':'Purchase Order', 'stock.picking':'Delivery Order' }[model] || 'Document';
    const list  = Array.isArray(blocks) ? blocks : [];
    const parts = [];
    // Explicit Row Start … Row End is buffered rather than streamed: column
    // widths are a share of the row's total flex weight, so no cell can be
    // emitted until every weight in the row is known.
    let row = null;   // { gap, cols: [{ flex, parts: [] }] }

    const flushRow = () => {
        if (!row) return;
        const cols  = row.cols.filter(c => c.parts.length);
        if (cols.length) {
            const total = cols.reduce((s, c) => s + c.flex, 0) || cols.length;
            parts.push(dleRowHtml(cols.map((c, n) => ({
                html:    c.parts.join('\n'),
                width:   +(c.flex / total * 100).toFixed(4),
                padLeft: n > 0 ? row.gap : 0,
            }))));
        }
        row = null;
    };

    for (let i = 0; i < list.length; i++) {
        const blk = list[i];
        if (blk.visible === false) continue;
        const props = blk.props || {};

        // ---- Explicit row blocks (N columns, manual) ----
        if (blk.type === 'row_start') {
            flushRow();                                   // auto-close a dangling row
            row = { gap: (props.gap != null && props.gap !== '') ? props.gap : 6,
                    cols: [{ flex: props.flex || 1, parts: [] }] };
            continue;
        }
        if (blk.type === 'col_break') {
            if (row) row.cols.push({ flex: props.flex || 1, parts: [] });
            continue;
        }
        if (blk.type === 'row_end') { flushRow(); continue; }

        // ---- Automatic two-column band ----
        // Consumes the run of adjacent blocks that opted into a column. The band
        // ends when a Left block appears after a Right one — that starts a new band.
        if (!row && dleColSide(blk)) {
            const run = [];
            let seenRight = false;
            let j = i;
            for (; j < list.length; j++) {
                const b = list[j];
                if (b.visible === false) continue;        // hidden blocks never split a band
                const side = dleColSide(b);
                if (!side) break;
                if (side === 'left' && seenRight) break;
                if (side === 'right') seenRight = true;
                run.push(b);
            }
            const lHtml = run.filter(b => dleColSide(b) === 'left')
                             .map(b => dleRenderBlock(b, model)).filter(Boolean).join('\n');
            const rHtml = run.filter(b => dleColSide(b) === 'right')
                             .map(b => dleRenderBlock(b, model)).filter(Boolean).join('\n');

            if (lHtml && rHtml) {
                // Band settings come from the first block that defines them
                const pick = (k) => {
                    for (const b of run) {
                        const v = (b.props || {})[k];
                        if (v !== undefined && v !== null && v !== '') return v;
                    }
                    return undefined;
                };
                let lw = parseFloat(pick('col_width'));
                if (!(lw >= 10 && lw <= 90)) lw = 50;
                const gap    = parseFloat(pick('col_gap'));
                const gapMm  = (gap >= 0 && gap <= 30) ? gap : 6;
                // Widths total 100% and the gap lives inside the left cell's
                // padding — `box-sizing:border-box` is global, so nothing overflows.
                parts.push(dleRowHtml([
                    { html: lHtml, width: lw,      padRight: gapMm },
                    { html: rHtml, width: 100 - lw },
                ], _VALIGN_CSS[pick('col_valign')] || ''));
            } else {
                // Nothing opposite — a lone column would just waste the page width
                if (lHtml) parts.push(lHtml);
                if (rHtml) parts.push(rHtml);
            }
            i = j - 1;
            continue;
        }

        // ---- Standard content block ----
        const html = dleRenderBlock(blk, model);
        if (!html) continue;
        if (row) row.cols[row.cols.length - 1].parts.push(html);
        else     parts.push(html);
    }

    flushRow();

    return `<!DOCTYPE html><html><head><meta charset="UTF-8"><title>${label} - {{doc_number}}</title>\n${DLE_CSS}\n</head><body>\n${parts.join('\n\n')}\n</body></html>`;
}

// ---- Client-side preview renderer (substitutes {{vars}} with dummy data) ----
const DLE_DUMMY = {
    common: {
        company_name:'Demo Company Sdn. Bhd.', company_reg:'123456-A',
        company_addr1:'Level 10, Menara Demo', company_addr2:'Jalan Ampang',
        company_addr3:'', company_city_country:'50450 Kuala Lumpur, Malaysia',
        company_phone:'+603-2181 8000', company_email:'info@democompany.com',
        company_website:'www.democompany.com',
        currency_code:'MYR', payment_term_days:'30',
        bank_account_name:'Demo Company Sdn. Bhd.', bank_account_no:'1234567890',
        bank_name:'Maybank Berhad', bank_address:'Jalan Tun Perak, KL', bank_swift:'MBBEMYKL',
        partner_name:'ABC Technology Sdn. Bhd.', partner_street:'Level 3, Menara KL',
        partner_city:'50088 Kuala Lumpur, Malaysia', partner_phone:'+603-2181 9000',
        attn_name:'Mr. John Doe',
        notes:'Thank you for your business.', text_content:'Additional information.',
    },
    'account.move':   { document_title:'Sales Invoice', doc_number:'INV/2025/0001', doc_date:'01/03/2025', doc_date_due:'31/03/2025', amount_untaxed:'10,000.00', amount_tax:'600.00', amount_total:'10,600.00', lines:[
        {product_name:'Electrical Equipment', line_type:'line_section'},
        {product_name:'Industrial Motor 5kW',qty:'2.00',uom:'Unit',price_unit:'2,500.00',subtotal:'5,000.00',line_type:'product'},
        {product_name:'Control Panel Assembly',qty:'1.00',uom:'Unit',price_unit:'3,000.00',subtotal:'3,000.00',line_type:'product'},
        {product_name:'Please ensure all equipment is inspected before installation.',line_type:'line_note'},
        {product_name:'Installation & Services', line_type:'line_section'},
        {product_name:'Installation Service',qty:'1.00',uom:'Job',price_unit:'2,000.00',subtotal:'2,000.00',line_type:'product'},
        {product_name:'Delivery charges included. Contact us for warranty claims.',line_type:'line_note'},
    ] },
    'sale.order':     { document_title:'Sales Order', doc_number:'SO/2025/0001', doc_date:'01/03/2025', validity_date:'31/03/2025', amount_untaxed:'10,000.00', amount_tax:'600.00', amount_total:'10,600.00', lines:[
        {product_name:'Equipment', line_type:'line_section'},
        {product_name:'Industrial Motor 5kW',qty:'2.00',uom:'Unit',price_unit:'2,500.00',subtotal:'5,000.00',line_type:'product'},
        {product_name:'Control Panel Assembly',qty:'1.00',uom:'Unit',price_unit:'3,000.00',subtotal:'3,000.00',line_type:'product'},
        {product_name:'Lead time: 2–3 weeks upon order confirmation.',line_type:'line_note'},
        {product_name:'Installation Service',qty:'1.00',uom:'Job',price_unit:'2,000.00',subtotal:'2,000.00',line_type:'product'},
    ] },
    'purchase.order': { document_title:'Purchase Order', doc_number:'PO/2025/0001', doc_date:'01/03/2025', date_planned:'15/03/2025', amount_untaxed:'5,500.00', amount_tax:'330.00', amount_total:'5,830.00', lines:[
        {product_name:'Motors & Drives', line_type:'line_section'},
        {product_name:'Industrial Motor 5kW',qty:'2.00',uom:'Unit',price_unit:'2,000.00',subtotal:'4,000.00',line_type:'product'},
        {product_name:'Accessories', line_type:'line_section'},
        {product_name:'Cable Set',qty:'10.00',uom:'Pcs',price_unit:'150.00',subtotal:'1,500.00',line_type:'product'},
        {product_name:'Please deliver to warehouse loading bay. Call ahead.',line_type:'line_note'},
    ] },
    'stock.picking':  { document_title:'Delivery Order', doc_number:'WH/OUT/2025/0001', doc_date:'01/03/2025', origin:'SO/2025/0001', source_location:'WH/Stock', dest_location:'Customers', lines:[{product_name:'Industrial Motor 5kW',demand:'2.00',done:'2.00',uom:'Unit'},{product_name:'Control Panel Assembly',demand:'1.00',done:'1.00',uom:'Unit'}] },
};

function dleFormatPrec(val, prec) {
    const n = parseFloat(String(val).replace(/,/g, ''));
    if (isNaN(n)) return val;
    return n.toLocaleString('en-US', { minimumFractionDigits: prec, maximumFractionDigits: prec });
}

function dleRenderPreview(templateHtml, model, settings) {
    const qtyPrec = settings?.decimal_qty      ?? 2;
    const prcPrec = settings?.decimal_price    ?? 2;
    const subPrec = settings?.decimal_subtotal ?? 2;
    const d = { ...DLE_DUMMY.common, ...(DLE_DUMMY[model] || {}) };
    const cols = model === 'stock.picking' ? 4 : 5;
    // Process #each blocks FIRST — the simple replace below would blank inner {{vars}}
    let html = templateHtml.replace(/\{\{#each lines\}\}([\s\S]*?)\{\{\/each\}\}/g, (_, tpl) =>
        (d.lines || []).map(ln => {
            if (ln.line_type === 'line_section')
                return `<tr class="row-line_section"><td colspan="${cols}">${ln.product_name || ''}</td></tr>`;
            if (ln.line_type === 'line_note')
                return `<tr class="row-line_note"><td colspan="${cols}">${ln.product_name || ''}</td></tr>`;
            const fmtLn = { ...ln,
                qty:        dleFormatPrec(ln.qty,        qtyPrec),
                price_unit: dleFormatPrec(ln.price_unit, prcPrec),
                subtotal:   dleFormatPrec(ln.subtotal,   subPrec),
            };
            return tpl.replace(/\{\{(\w+)\}\}/g, (__, k) => fmtLn[k] !== undefined ? fmtLn[k] : '');
        }).join('')
    );
    // Then replace top-level document variables
    html = html.replace(/\{\{(\w+)\}\}/g, (_, k) => d[k] !== undefined ? d[k] : '');
    return html;
}

// ----------------------------------------------------------------
// DocumentLayoutEditor — replaces ReportSettingsView
// ----------------------------------------------------------------
class DocumentLayoutEditor extends Component {
    static template = xml`
        <div class="dle-shell" t-ref="shell">
            <!-- Top bar -->
            <div class="dle-topbar">
                <div class="dle-doc-tabs">
                    <t t-foreach="state.docTypes" t-as="dt" t-key="dt.model">
                        <button t-attf-class="dle-doc-tab{{ state.docModel===dt.model?' active':'' }}"
                                t-on-click="()=>this.onDocTypeChange(dt.model)"
                                t-esc="dt.label"/>
                    </t>
                </div>
                <div class="dle-topbar-actions">
                    <span t-if="state.isCustomized" class="dle-badge-custom"
                          title="This template differs from the one shipped in the source tree">Customized</span>
                    <span t-else="" class="dle-badge-stock"
                          title="This template matches the one shipped in the source tree">Default</span>
                    <button class="btn" t-on-click="onCompareShipped">&#9776; Compare</button>
                    <button class="btn" t-on-click="onAskReset" t-att-disabled="state.resetting">Reset to default</button>
                    <button class="btn" t-on-click="onRefreshPreview">&#8635; Refresh</button>
                    <button class="btn btn-primary" t-on-click="onSave" t-att-disabled="state.saving">
                        <t t-if="state.saving">Saving&#8230;</t><t t-else="">Save</t>
                    </button>
                    <span t-if="state.saved" class="dle-saved-msg">&#10003; Saved</span>
                </div>
            </div>

            <!-- Reset confirmation -->
            <t t-if="state.confirmReset">
                <div class="gf-modal" t-on-click="onResetBackdrop">
                    <div class="gf-modal-card" t-on-click.stop="">
                        <h3 class="gf-modal-title">Reset to the shipped template?</h3>
                        <p class="dle-reset-text">
                            <t t-esc="currentDocLabel"/> goes back to the template that ships with
                            this version of the software. The saved block layout for this document
                            is discarded too, so it cannot re-apply itself on the next save.
                        </p>
                        <p class="dle-reset-text dle-reset-warn" t-if="state.isCustomized">
                            Your customisations to this template will be lost.
                        </p>
                        <div class="gf-modal-actions">
                            <button class="btn" t-on-click="() => this.state.confirmReset = false">Cancel</button>
                            <button class="btn btn-primary" t-on-click="onResetDefault"
                                    t-att-disabled="state.resetting">
                                <t t-if="state.resetting">Resetting&#8230;</t><t t-else="">Reset</t>
                            </button>
                        </div>
                    </div>
                </div>
            </t>

            <!-- Compare with the shipped template -->
            <t t-if="state.showDiff">
                <div class="gf-modal" t-on-click="onDiffBackdrop">
                    <div class="dle-diff-card" t-on-click.stop="">
                        <div class="dle-diff-head">
                            <h3 class="gf-modal-title" style="margin:0;">
                                <t t-esc="currentDocLabel"/> &#8212; yours vs. shipped
                            </h3>
                            <span class="dle-diff-stat" t-esc="diffSummary"/>
                            <button class="btn" t-on-click="() => this.state.showDiff = false">Close</button>
                        </div>
                        <div class="dle-diff-body">
                            <div t-if="!state.diffRows.length" class="dle-diff-same">
                                Identical to the shipped template.
                            </div>
                            <t t-foreach="state.diffRows" t-as="dr" t-key="dr_index">
                                <div t-attf-class="dle-diff-row dle-diff-{{dr.kind}}">
                                    <span class="dle-diff-gutter" t-esc="dr.mark"/>
                                    <span class="dle-diff-text" t-esc="dr.text"/>
                                </div>
                            </t>
                        </div>
                    </div>
                </div>
            </t>

            <!-- 3-panel main area -->
            <div class="dle-main">
                <!-- LEFT PANEL — tabs -->
                <div class="dle-left">
                    <div class="dle-left-tabs">
                        <button t-attf-class="dle-ltab{{ state.leftTab==='blocks'?' active':'' }}"
                                t-on-click="()=>this.setLeftTab('blocks')">Blocks</button>
                        <button t-attf-class="dle-ltab{{ state.leftTab==='html'?' active':'' }}"
                                t-on-click="()=>this.setLeftTab('html')">HTML</button>
                        <t t-if="state.leftTab==='html'">
                            <button class="dle-ltab-popout" title="Open in separate window"
                                    t-on-click="onPopoutHtml">&#x2197;</button>
                        </t>
                    </div>
                    <!-- Blocks tab -->
                    <t t-if="state.leftTab==='blocks'">
                        <div class="dle-acc-body dle-blocks-list" t-on-dragover="onDragOver" t-on-drop="onDrop">
                            <t t-foreach="state.blocks" t-as="blk" t-key="blk_index">
                                <div t-attf-class="dle-block-row{{ state.selectedBlock===blk_index?' selected':'' }}"
                                     draggable="true"
                                     t-att-data-idx="blk_index"
                                     t-on-dragstart="onDragStart"
                                     t-on-click="()=>this.selectBlock(blk_index)">
                                    <span class="dle-drag-handle">&#10783;</span>
                                    <span class="dle-block-icon" t-esc="getBlockDef(blk.type).icon"/>
                                    <span class="dle-block-label" t-esc="getBlockDef(blk.type).label"/>
                                    <span t-attf-class="dle-block-vis{{ blk.visible?' on':' off' }}"
                                          t-on-click.stop="()=>this.toggleBlock(blk_index)">
                                        <t t-if="blk.visible">&#128065;</t><t t-else="">&#9675;</t>
                                    </span>
                                    <span class="dle-block-del" title="Remove block"
                                          t-on-click.stop="()=>this.removeBlock(blk_index)">&#x2715;</span>
                                </div>
                            </t>
                        </div>
                    </t>
                    <!-- HTML Source tab -->
                    <t t-if="state.leftTab==='html'">
                        <div class="dle-acc-body">
                            <textarea class="dle-html-editor"
                                      t-att-value="state.templateHtml"
                                      t-on-input="onHtmlInput"/>
                        </div>
                    </t>
                </div>

                <!-- CENTER: preview iframe + log -->
                <div class="dle-center">
                    <div class="dle-preview-bar">
                        <span class="dle-preview-label">Preview &#8212; dummy data</span>
                        <t t-if="state.previewRecordId">
                            <button class="btn" style="font-size:.75rem;padding:2px 8px;"
                                    t-on-click="onOpenRealPreview">Open real record &#x2197;</button>
                        </t>
                    </div>
                    <div class="dle-preview-wrap">
                        <iframe class="dle-preview-frame"
                                t-att-srcdoc="state.previewDoc"/>
                    </div>
                    <!-- Foldable log panel -->
                    <div class="dle-log-panel">
                        <div class="dle-log-bar" t-on-click="toggleLog">
                            <span>&#x1F4CB; Log</span>
                            <span t-if="state.logLines.length" style="font-size:.7rem;color:var(--muted);"
                                  t-esc="state.logLines.length + ' entries'"/>
                            <span class="dle-acc-icon" t-esc="state.logOpen ? '\u25BE' : '\u25B8'"/>
                        </div>
                        <t t-if="state.logOpen">
                            <div class="dle-log-body">
                                <div class="dle-log-empty" t-if="!state.logLines.length">No log entries yet.</div>
                                <t t-foreach="state.logLines" t-as="ln" t-key="ln_index">
                                    <div class="dle-log-line" t-esc="ln"/>
                                </t>
                            </div>
                        </t>
                    </div>
                </div>

                <!-- Sidebar resize handle -->
                <div class="dle-sidebar-resizer" t-on-mousedown="onSidebarResizeStart"></div>

                <!-- RIGHT: sidebar -->
                <div class="dle-sidebar" t-att-style="'width:' + state.sidebarWidth + 'px'">
                    <div class="dle-sidebar-tabs">
                        <button t-attf-class="dle-stab{{ state.sidebarTab==='props'?' active':'' }}"
                                t-on-click="()=>this.setSidebarTab('props')">Properties</button>
                        <button t-attf-class="dle-stab{{ state.sidebarTab==='objects'?' active':'' }}"
                                t-on-click="()=>this.setSidebarTab('objects')">Objects</button>
                    </div>

                    <!-- Properties tab -->
                    <t t-if="state.sidebarTab==='props'">
                        <div class="dle-props-panel">
                            <t t-if="state.selectedBlock !== null">
                                <div class="dle-props-title" t-esc="getBlockDef(state.blocks[state.selectedBlock].type).label"/>
                                <!-- Visible toggle always shown -->
                                <div class="dle-prop-row">
                                    <label>Visible</label>
                                    <input type="checkbox"
                                           t-att-checked="state.blocks[state.selectedBlock].visible"
                                           t-on-change="()=>this.toggleBlock(state.selectedBlock)"/>
                                </div>
                                <!-- Grouped property accordions -->
                                <t t-foreach="getBlockPropGroups(state.blocks[state.selectedBlock].type)" t-as="grp" t-key="grp.label || grp_index">
                                    <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                         t-on-click="()=>this.togglePropSect(grp.label)">
                                        <span t-esc="grp.label"/>
                                        <span class="dle-acc-icon" t-esc="isPropSectOpen(grp.label) ? '\u25BE' : '\u25B8'"/>
                                    </div>
                                    <t t-if="isPropSectOpen(grp.label)">
                                        <t t-foreach="grp.items" t-as="pd" t-key="pd.key">
                                            <!-- info label: spans full width, no input -->
                                            <t t-if="pd.type === 'label'">
                                                <div class="dle-prop-row" style="display:block;font-size:.72rem;color:#aaa;padding:4px 0;line-height:1.3;" t-esc="pd.label"/>
                                            </t>
                                            <div t-else="" class="dle-prop-row">
                                                <label t-esc="pd.label"/>
                                                <!-- number -->
                                                <t t-if="pd.type === 'number'">
                                                    <div class="dle-prop-num-row">
                                                        <input type="number" class="dle-prop-input"
                                                               t-att-min="pd.min" t-att-max="pd.max"
                                                               t-att-value="state.blocks[state.selectedBlock].props[pd.key] ?? ''"
                                                               t-on-input="onPropInput"
                                                               t-att-data-prop="pd.key" data-numtype="1"/>
                                                        <span t-if="pd.unit" class="dle-prop-unit" t-esc="pd.unit"/>
                                                    </div>
                                                </t>
                                                <!-- color -->
                                                <t t-if="pd.type === 'color'">
                                                    <div class="dle-color-row">
                                                        <input type="color" class="dle-color-pick"
                                                               t-att-value="state.blocks[state.selectedBlock].props[pd.key] || '#000000'"
                                                               t-on-input="onPropInput"
                                                               t-att-data-prop="pd.key"/>
                                                        <input type="text" class="dle-prop-input dle-color-text"
                                                               t-att-value="state.blocks[state.selectedBlock].props[pd.key] || ''"
                                                               t-on-input="onPropInput"
                                                               t-att-data-prop="pd.key"/>
                                                    </div>
                                                </t>
                                                <!-- select -->
                                                <t t-if="pd.type === 'select'">
                                                    <select class="dle-prop-input" t-on-change="onPropInput" t-att-data-prop="pd.key">
                                                        <t t-foreach="pd.options" t-as="opt" t-key="opt.v">
                                                            <option t-att-value="opt.v"
                                                                    t-att-selected="(state.blocks[state.selectedBlock].props[pd.key] || '') === opt.v"
                                                                    t-esc="opt.l"/>
                                                        </t>
                                                    </select>
                                                </t>
                                                <!-- boolean -->
                                                <t t-if="pd.type === 'boolean'">
                                                    <input type="checkbox"
                                                           t-att-checked="!!state.blocks[state.selectedBlock].props[pd.key]"
                                                           t-on-change="onPropCheck"
                                                           t-att-data-prop="pd.key"/>
                                                </t>
                                                <!-- text -->
                                                <t t-if="pd.type === 'text'">
                                                    <input type="text" class="dle-prop-input"
                                                           t-att-value="state.blocks[state.selectedBlock].props[pd.key] || ''"
                                                           t-att-placeholder="pd.placeholder || ''"
                                                           t-on-input="onPropInput"
                                                           t-att-data-prop="pd.key"/>
                                                </t>
                                                <!-- textarea -->
                                                <t t-if="pd.type === 'textarea'">
                                                    <textarea class="dle-prop-input dle-prop-textarea" rows="4"
                                                              t-att-value="state.blocks[state.selectedBlock].props[pd.key] || ''"
                                                              t-on-input="onPropInput"
                                                              t-att-data-prop="pd.key"/>
                                                </t>
                                            </div>
                                        </t>
                                    </t>
                                </t>
                                <!-- Precision section — visible only for items_table block -->
                                <t t-if="state.blocks[state.selectedBlock].type === 'items_table'">
                                    <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                         t-on-click="()=>this.togglePropSect('Precision')">
                                        <span>Precision (decimal digits)</span>
                                        <span class="dle-acc-icon" t-esc="isPropSectOpen('Precision') ? '\u25BE' : '\u25B8'"/>
                                    </div>
                                    <t t-if="isPropSectOpen('Precision')">
                                        <div class="dle-prop-row">
                                            <label>Quantity</label>
                                            <select class="dle-prop-input"
                                                    t-on-change="setDecimalQty">
                                                <t t-foreach="[0,1,2,3,4,5,6]" t-as="n" t-key="n">
                                                    <option t-att-value="n"
                                                            t-att-selected="state.docSettings.decimal_qty === n ? true : undefined"
                                                            t-esc="n"/>
                                                </t>
                                            </select>
                                        </div>
                                        <div class="dle-prop-row">
                                            <label>Unit Price</label>
                                            <select class="dle-prop-input"
                                                    t-on-change="setDecimalPrice">
                                                <t t-foreach="[0,1,2,3,4,5,6]" t-as="n" t-key="n">
                                                    <option t-att-value="n"
                                                            t-att-selected="state.docSettings.decimal_price === n ? true : undefined"
                                                            t-esc="n"/>
                                                </t>
                                            </select>
                                        </div>
                                        <div class="dle-prop-row">
                                            <label>Subtotal</label>
                                            <select class="dle-prop-input"
                                                    t-on-change="setDecimalSubtotal">
                                                <t t-foreach="[0,1,2,3,4,5,6]" t-as="n" t-key="n">
                                                    <option t-att-value="n"
                                                            t-att-selected="state.docSettings.decimal_subtotal === n ? true : undefined"
                                                            t-esc="n"/>
                                                </t>
                                            </select>
                                        </div>
                                    </t>
                                </t>
                            </t>
                            <t t-else="">
                                <div class="dle-props-title">Document Settings</div>

                                <!-- Format -->
                                <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                     t-on-click="()=>this.togglePropSect('DS_Format')">
                                    <span>Format</span>
                                    <span class="dle-acc-icon" t-esc="isPropSectOpen('DS_Format') ? '\u25BE' : '\u25B8'"/>
                                </div>
                                <t t-if="isPropSectOpen('DS_Format')">
                                    <div class="dle-prop-row">
                                        <label>Paper Format</label>
                                        <select class="dle-prop-input" t-model="state.docSettings.paper_format">
                                            <option value="A4">A4</option>
                                            <option value="Letter">Letter</option>
                                            <option value="A3">A3</option>
                                            <option value="Legal">Legal</option>
                                        </select>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Orientation</label>
                                        <select class="dle-prop-input" t-model="state.docSettings.orientation">
                                            <option value="portrait">Portrait</option>
                                            <option value="landscape">Landscape</option>
                                        </select>
                                    </div>
                                </t>

                                <!-- Page Margins -->
                                <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                     t-on-click="()=>this.togglePropSect('DS_Margins')">
                                    <span>Page Margins (mm)</span>
                                    <span class="dle-acc-icon" t-esc="isPropSectOpen('DS_Margins') ? '\u25BE' : '\u25B8'"/>
                                </div>
                                <t t-if="isPropSectOpen('DS_Margins')">
                                    <div class="dle-prop-row">
                                        <label>Top</label>
                                        <input type="number" class="dle-prop-input" min="0" max="50" step="1"
                                               t-att-value="state.docSettings.margin_top"
                                               t-on-input="ev=>{ this.state.docSettings.margin_top = parseFloat(ev.target.value)||0; }"/>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Right</label>
                                        <input type="number" class="dle-prop-input" min="0" max="50" step="1"
                                               t-att-value="state.docSettings.margin_right"
                                               t-on-input="ev=>{ this.state.docSettings.margin_right = parseFloat(ev.target.value)||0; }"/>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Bottom</label>
                                        <input type="number" class="dle-prop-input" min="0" max="50" step="1"
                                               t-att-value="state.docSettings.margin_bottom"
                                               t-on-input="ev=>{ this.state.docSettings.margin_bottom = parseFloat(ev.target.value)||0; }"/>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Left</label>
                                        <input type="number" class="dle-prop-input" min="0" max="50" step="1"
                                               t-att-value="state.docSettings.margin_left"
                                               t-on-input="ev=>{ this.state.docSettings.margin_left = parseFloat(ev.target.value)||0; }"/>
                                    </div>
                                </t>

                                <!-- Typography -->
                                <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                     t-on-click="()=>this.togglePropSect('DS_Typography')">
                                    <span>Typography</span>
                                    <span class="dle-acc-icon" t-esc="isPropSectOpen('DS_Typography') ? '\u25BE' : '\u25B8'"/>
                                </div>
                                <t t-if="isPropSectOpen('DS_Typography')">
                                    <div class="dle-prop-row">
                                        <label>Font Family</label>
                                        <select class="dle-prop-input" t-model="state.docSettings.font_family">
                                            <option value="Arial, sans-serif">Arial</option>
                                            <option value="'Times New Roman', serif">Times New Roman</option>
                                            <option value="Helvetica, sans-serif">Helvetica</option>
                                            <option value="Georgia, serif">Georgia</option>
                                            <option value="'Courier New', monospace">Courier New</option>
                                        </select>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Font Size (pt)</label>
                                        <input type="number" class="dle-prop-input" min="6" max="24" step="1"
                                               t-att-value="state.docSettings.font_size"
                                               t-on-input="ev=>{ this.state.docSettings.font_size = parseInt(ev.target.value)||10; }"/>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Font Color</label>
                                        <div class="dle-color-row">
                                            <input type="color" class="dle-color-pick"
                                                   t-att-value="state.docSettings.font_color"
                                                   t-on-input="ev=>{ this.state.docSettings.font_color = ev.target.value; }"/>
                                            <input type="text" class="dle-prop-input dle-color-text"
                                                   t-att-value="state.docSettings.font_color"
                                                   t-on-input="ev=>{ this.state.docSettings.font_color = ev.target.value; }"/>
                                        </div>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Accent Color</label>
                                        <div class="dle-color-row">
                                            <input type="color" class="dle-color-pick"
                                                   t-att-value="state.docSettings.accent_color"
                                                   t-on-input="onAccentColorInput"/>
                                            <input type="text" class="dle-prop-input dle-color-text"
                                                   t-att-value="state.docSettings.accent_color"
                                                   t-on-input="onAccentColorInput"/>
                                        </div>
                                    </div>
                                    <div class="dle-prop-row">
                                        <label>Line Height</label>
                                        <input type="number" class="dle-prop-input" min="1" max="3" step="0.1"
                                               t-att-value="state.docSettings.line_height"
                                               t-on-input="ev=>{ this.state.docSettings.line_height = parseFloat(ev.target.value)||1.5; }"/>
                                    </div>
                                </t>

                                <!-- Preview -->
                                <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                     t-on-click="()=>this.togglePropSect('DS_Preview')">
                                    <span>Preview</span>
                                    <span class="dle-acc-icon" t-esc="isPropSectOpen('DS_Preview') ? '\u25BE' : '\u25B8'"/>
                                </div>
                                <t t-if="isPropSectOpen('DS_Preview')">
                                    <div class="dle-prop-row">
                                        <label>Record ID</label>
                                        <input type="text" class="dle-prop-input" placeholder="optional — opens in new tab"
                                               t-att-value="state.previewRecordId"
                                               t-on-input="ev=>this.state.previewRecordId=ev.target.value"/>
                                    </div>
                                </t>
                            </t>
                        </div>
                    </t>

                    <!-- Objects tab: block palette -->
                    <t t-if="state.sidebarTab==='objects'">
                        <div class="dle-objects-panel">
                            <div class="dle-objects-hint">Click a block to add it to the document</div>
                            <!-- Content blocks accordion -->
                            <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                 t-on-click="()=>this.toggleObjSect('content')">
                                <span>Content Blocks</span>
                                <span class="dle-acc-icon" t-esc="isObjSectOpen('content') ? '\u25BE' : '\u25B8'"/>
                            </div>
                            <t t-if="isObjSectOpen('content')">
                                <t t-foreach="state.blockDefs.filter(d=>d.group==='content')" t-as="def" t-key="def.type">
                                    <div class="dle-object-item"
                                         t-on-click="()=>this.addBlock(def.type)"
                                         draggable="true"
                                         t-att-data-type="def.type">
                                        <span class="dle-obj-icon" t-esc="def.icon"/>
                                        <span class="dle-obj-label" t-esc="def.label"/>
                                        <span class="dle-obj-add">+</span>
                                    </div>
                                </t>
                            </t>
                            <!-- Layout blocks accordion -->
                            <div class="dle-acc-hdr" style="font-size:.73rem;padding:5px 10px;"
                                 t-on-click="()=>this.toggleObjSect('layout')">
                                <span>Layout Blocks</span>
                                <span class="dle-acc-icon" t-esc="isObjSectOpen('layout') ? '\u25BE' : '\u25B8'"/>
                            </div>
                            <t t-if="isObjSectOpen('layout')">
                                <t t-foreach="state.blockDefs.filter(d=>d.group==='layout')" t-as="def" t-key="def.type">
                                    <div class="dle-object-item dle-object-layout"
                                         t-on-click="()=>this.addBlock(def.type)"
                                         draggable="true"
                                         t-att-data-type="def.type">
                                        <span class="dle-obj-icon" t-esc="def.icon"/>
                                        <span class="dle-obj-label" t-esc="def.label"/>
                                        <span class="dle-obj-add">+</span>
                                    </div>
                                </t>
                            </t>
                        </div>
                    </t>
                </div>
            </div>
        </div>
    `;

    static components = {};

    setup() {
        this.state = useState({
            docModel:        'account.move',
            leftTab:         'blocks',
            sidebarTab:      'props',
            blocks:          [],
            selectedBlock:   null,
            templateHtml:    '',
            templateId:      null,
            previewDoc:      '',
            previewRecordId: '',
            saving:          false,
            saved:           false,
            docSettings:     { paper_format:'A4', orientation:'portrait', font_family:'Arial, sans-serif', accent_color:'#4a4a4a', decimal_qty:2, decimal_price:2, decimal_subtotal:2, margin_top:15, margin_right:18, margin_bottom:18, margin_left:18, font_size:10, font_color:'#333333', line_height:1.5, footer_text:'' },
            loadingTemplate: false,
            docTypes:        DLE_DOC_TYPES,
            blockDefs:       DLE_BLOCK_DEFS,
            sidebarWidth:    260,
            // Right panel prop-section collapsed state (key = section label)
            propSectClosed:  {},
            // Right panel objects collapsed state
            objSectClosed:   {},
            // Log window
            logOpen:         false,
            logLines:        [],
            // Shipped-template baseline (ir_report_template.default_html)
            defaultHtml:     '',
            isCustomized:    false,
            confirmReset:    false,
            resetting:       false,
            showDiff:        false,
            diffRows:        [],
        });
        this.shellRef = useRef('shell');
        this._updateHeight = () => {
            const el = this.shellRef.el;
            if (!el) return;
            const top = el.getBoundingClientRect().top;
            el.style.height = (window.innerHeight - top) + 'px';
        };
        onMounted(() => {
            this._updateHeight();
            window.addEventListener('resize', this._updateHeight);
            this.onDocTypeChange('account.move');
            this._onMsgFromPopout = (ev) => {
                if (ev.data && ev.data.type === 'dle-html-update') {
                    this.state.templateHtml = ev.data.html;
                    this.state.previewDoc   = dleRenderPreview(ev.data.html, this.state.docModel, this.state.docSettings);
                }
            };
            window.addEventListener('message', this._onMsgFromPopout);
        });
        onWillUnmount(() => {
            window.removeEventListener('resize', this._updateHeight);
            if (this._onMsgFromPopout)
                window.removeEventListener('message', this._onMsgFromPopout);
        });
    }

    onSidebarResizeStart(ev) {
        ev.preventDefault();
        const startX     = ev.clientX;
        const startWidth = this.state.sidebarWidth;
        document.body.style.userSelect = 'none';
        const onMove = (e) => {
            const delta = startX - e.clientX;          // dragging left → widens sidebar
            this.state.sidebarWidth = Math.max(180, Math.min(600, startWidth + delta));
        };
        const onUp = () => {
            document.body.style.userSelect = '';
            document.removeEventListener('mousemove', onMove);
            document.removeEventListener('mouseup',   onUp);
        };
        document.addEventListener('mousemove', onMove);
        document.addEventListener('mouseup',   onUp);
    }

    onPopoutHtml() {
        const win = window.open('', '_blank', 'width=980,height=740,menubar=no,toolbar=no');
        if (!win) return;
        const initJson = JSON.stringify(this.state.templateHtml || '');
        win.document.write(`<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>HTML Editor \u2014 Document Template</title>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.css">
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/theme/dracula.min.css">
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { display: flex; flex-direction: column; height: 100vh; font-family: sans-serif; }
  #toolbar { display: flex; align-items: center; gap: 8px; padding: 6px 10px; background: #2a2a3c; border-bottom: 1px solid #555; flex-shrink: 0; height: 38px; }
  #toolbar span { color: #aaa; font-size: 12px; flex: 1; }
  button { padding: 3px 12px; border: none; border-radius: 4px; cursor: pointer; font-size: 12px; }
  #btn-apply { background: #4c8af4; color: #fff; }
  #btn-apply:hover { background: #3a72d8; }
  #btn-close { background: #555; color: #eee; }
  #btn-close:hover { background: #333; }
  /* Make CodeMirror fill the remaining height */
  .CodeMirror { height: calc(100vh - 38px) !important; font-family: 'Consolas','Cascadia Code','Courier New',monospace !important; font-size: 13px; line-height: 1.55; }
  .CodeMirror-scroll { height: 100%; }
</style>
</head><body>
<div id="toolbar">
  <span>HTML Editor \u2014 Ctrl+S or Apply to push changes back</span>
  <button id="btn-apply">\u2713 Apply</button>
  <button id="btn-close">\u2715 Close</button>
</div>
<textarea id="ed"></textarea>
<script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.js"><\/script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/xml/xml.min.js"><\/script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/javascript/javascript.min.js"><\/script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/css/css.min.js"><\/script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/htmlmixed/htmlmixed.min.js"><\/script>
<script>
  let ready = false;
  function apply() {
    if (!ready) return;
    if (window.opener && !window.opener.closed)
      window.opener.postMessage({ type: 'dle-html-update', html: cm.getValue() }, '*');
  }

  const cm = CodeMirror.fromTextArea(document.getElementById('ed'), {
    mode:           'htmlmixed',
    theme:          'dracula',
    indentUnit:     2,
    tabSize:        2,
    indentWithTabs: false,
    lineNumbers:    true,
    lineWrapping:   false,
    autofocus:      true,
    extraKeys: {
      'Ctrl-S': () => { ready = true; apply(); },
      'Tab':    (c) => c.execCommand('insertSoftTab'),
    },
  });
  cm.setValue(${initJson});
  cm.refresh();

  cm.on('focus', () => { ready = true; });
  // Auto-apply when user switches back to the main window
  window.addEventListener('blur', apply);

  document.getElementById('btn-apply').addEventListener('click', () => { ready = true; apply(); });
  document.getElementById('btn-close').addEventListener('click', () => window.close());
<\/script>
</body></html>`);
        win.document.close();
    }

    getBlockDef(type) {
        return DLE_BLOCK_DEFS.find(d => d.type === type) || { icon: '?', label: type };
    }

    // ---- Left tab ----
    setLeftTab(tab) {
        this.state.leftTab = tab;
        if (tab === 'blocks') this.state.selectedBlock = null;
    }

    togglePropSect(label) {
        this.state.propSectClosed = { ...this.state.propSectClosed, [label]: !this.state.propSectClosed[label] };
    }
    isPropSectOpen(label) { return !this.state.propSectClosed[label]; }

    toggleObjSect(key) {
        this.state.objSectClosed = { ...this.state.objSectClosed, [key]: !this.state.objSectClosed[key] };
    }
    isObjSectOpen(key) { return !this.state.objSectClosed[key]; }

    // ---- Log ----
    toggleLog() { this.state.logOpen = !this.state.logOpen; }

    addLog(msg) {
        const t = new Date().toLocaleTimeString('en-GB', { hour12: false });
        this.state.logLines.unshift(`[${t}] ${msg}`);
        if (this.state.logLines.length > 100) this.state.logLines.pop();
    }

    // ---- Prop groups for accordion ----
    getBlockPropGroups(type) {
        const defs = DLE_PROP_DEFS[type] || [];
        const groups = [];
        let cur = null;
        for (const d of defs) {
            if (d.type === 'divider') { cur = { label: d.label, items: [] }; groups.push(cur); }
            else { if (!cur) { cur = { label: 'General', items: [] }; groups.push(cur); } cur.items.push(d); }
        }
        return groups;
    }

    setSidebarTab(tab) {
        this.state.sidebarTab = tab;
        if (tab !== 'props') this.state.selectedBlock = null;
    }

    selectBlock(idx) {
        this.state.selectedBlock = idx;
        this.state.sidebarTab = 'props';
    }

    addBlock(type) {
        this.state.blocks.push({ type, visible: true, props: {} });
        this.state.leftTab = 'blocks';
        this.addLog(`Added: ${this.getBlockDef(type).label}`);
        this.rebuildHtml();
    }

    removeBlock(idx) {
        const label = this.getBlockDef(this.state.blocks[idx].type).label;
        this.state.blocks.splice(idx, 1);
        if (this.state.selectedBlock === idx) this.state.selectedBlock = null;
        else if (this.state.selectedBlock > idx) this.state.selectedBlock--;
        this.addLog(`Removed: ${label}`);
        this.rebuildHtml();
    }

    toggleBlock(idx) {
        this.state.blocks[idx].visible = !this.state.blocks[idx].visible;
        this.rebuildHtml();
    }

    setDecimalQty(ev)      { this.state.docSettings.decimal_qty      = parseInt(ev.target.value); this.rebuildHtml(); }
    setDecimalPrice(ev)    { this.state.docSettings.decimal_price    = parseInt(ev.target.value); this.rebuildHtml(); }
    setDecimalSubtotal(ev) { this.state.docSettings.decimal_subtotal = parseInt(ev.target.value); this.rebuildHtml(); }

    rebuildHtml() {
        this.state.templateHtml = dleBuildHtml(this.state.blocks, this.state.docModel);
        this.state.previewDoc   = dleRenderPreview(this.state.templateHtml, this.state.docModel, this.state.docSettings);
    }

    onDragStart(ev) {
        ev.dataTransfer.setData('text/plain', ev.currentTarget.dataset.idx);
    }

    onDragOver(ev) { ev.preventDefault(); }

    onDrop(ev) {
        ev.preventDefault();
        const fromIdx = parseInt(ev.dataTransfer.getData('text/plain'));
        if (isNaN(fromIdx)) return;
        const toRow = ev.target.closest('.dle-block-row');
        const toIdx = toRow ? parseInt(toRow.dataset.idx) : this.state.blocks.length - 1;
        if (fromIdx === toIdx) return;
        const blocks = [...this.state.blocks];
        const [moved] = blocks.splice(fromIdx, 1);
        blocks.splice(toIdx, 0, moved);
        this.state.blocks = blocks;
        this.state.selectedBlock = null;
        this.rebuildHtml();
    }

    onHtmlInput(ev) {
        this.state.templateHtml = ev.target.value;
        this.state.previewDoc   = dleRenderPreview(this.state.templateHtml, this.state.docModel, this.state.docSettings);
    }

    getBlockPropDefs(type) {
        return DLE_PROP_DEFS[type] || [];
    }

    onPropInput(ev) {
        const idx = this.state.selectedBlock;
        if (idx === null || idx === undefined) return;
        const prop = ev.target.dataset.prop;
        const val  = ev.target.dataset.numtype ? (parseFloat(ev.target.value) || 0) : ev.target.value;
        this.state.blocks[idx].props = { ...this.state.blocks[idx].props, [prop]: val };
        this.rebuildHtml();
    }

    onPropCheck(ev) {
        const idx = this.state.selectedBlock;
        if (idx === null || idx === undefined) return;
        const prop = ev.target.dataset.prop;
        this.state.blocks[idx].props = { ...this.state.blocks[idx].props, [prop]: ev.target.checked };
        this.rebuildHtml();
    }

    onAccentColorInput(ev) { this.state.docSettings.accent_color = ev.target.value; }

    onRefreshPreview() { this.rebuildHtml(); }

    // ---- shipped-template baseline: compare & reset ----------------
    // template_html is owned by the database; default_html is what the source
    // tree ships. Keeping the two side by side is what lets a template be
    // reset without a hand-written SQL migration (docs/089, docs/090).

    get currentDocLabel() {
        const dt = DLE_DOC_TYPES.find(d => d.model === this.state.docModel);
        return dt ? dt.label : this.state.docModel;
    }

    get diffSummary() {
        let add = 0, del = 0;
        for (const r of this.state.diffRows) {
            if (r.kind === 'add') add++;
            else if (r.kind === 'del') del++;
        }
        return add || del ? `+${add} / -${del} lines` : 'no differences';
    }

    // Line diff, longest-common-subsequence. Templates are a few hundred lines,
    // so the O(n·m) table is not worth avoiding. Unchanged runs longer than
    // six lines collapse to a single "…" row to keep the panel readable.
    buildDiff(mine, shipped) {
        const A = (shipped || '').split('\n');
        const B = (mine    || '').split('\n');
        const n = A.length, m = B.length;
        const lcs = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
        for (let i = n - 1; i >= 0; i--)
            for (let j = m - 1; j >= 0; j--)
                lcs[i][j] = A[i] === B[j] ? lcs[i + 1][j + 1] + 1
                                          : Math.max(lcs[i + 1][j], lcs[i][j + 1]);
        const raw = [];
        let i = 0, j = 0;
        while (i < n && j < m) {
            if (A[i] === B[j])                       { raw.push({ kind: 'same', mark: ' ', text: A[i] }); i++; j++; }
            else if (lcs[i + 1][j] >= lcs[i][j + 1]) { raw.push({ kind: 'del',  mark: '-', text: A[i] }); i++; }
            else                                     { raw.push({ kind: 'add',  mark: '+', text: B[j] }); j++; }
        }
        while (i < n) raw.push({ kind: 'del', mark: '-', text: A[i++] });
        while (j < m) raw.push({ kind: 'add', mark: '+', text: B[j++] });

        const out = [];
        let run = 0;
        for (let k = 0; k < raw.length; k++) {
            if (raw[k].kind !== 'same') { run = 0; out.push(raw[k]); continue; }
            // keep 3 lines of context either side of a change
            const nearChange = raw.slice(Math.max(0, k - 3), k + 4).some(r => r.kind !== 'same');
            if (nearChange) { run = 0; out.push(raw[k]); }
            else if (run++ === 0) out.push({ kind: 'skip', mark: '…', text: '' });
        }
        return out.every(r => r.kind === 'same' || r.kind === 'skip') ? [] : out;
    }

    onCompareShipped() {
        if (!this.state.defaultHtml) {
            this.addLog('No shipped baseline recorded for this template yet — restart the server once.');
            return;
        }
        // Compare what is stored, not the unsaved buffer, so the panel matches
        // the Customized badge.
        this.state.diffRows = this.buildDiff(this.storedHtml || this.state.templateHtml,
                                             this.state.defaultHtml);
        this.state.showDiff = true;
    }

    onDiffBackdrop(ev)  { if (ev.target === ev.currentTarget) this.state.showDiff    = false; }
    onResetBackdrop(ev) { if (ev.target === ev.currentTarget) this.state.confirmReset = false; }

    onAskReset() { this.state.confirmReset = true; }

    async onResetDefault() {
        if (!this.state.templateId) return;
        this.state.resetting = true;
        try {
            await RpcService.call('ir.report.template', 'action_reset_default',
                                  [[this.state.templateId]], {});
            this.addLog(`Reset ${this.currentDocLabel} to the shipped template.`);
            this.state.confirmReset = false;
            await this.onDocTypeChange(this.state.docModel);   // reload blocks + html
        } catch (e) {
            this.addLog(`Reset failed: ${e.message || e}`);
        } finally {
            this.state.resetting = false;
        }
    }

    onOpenRealPreview() {
        const id = parseInt(this.state.previewRecordId);
        if (id > 0) window.open(`/report/html/${this.state.docModel}/${id}`, '_blank');
    }

    // Bring a stored layout up to the column model.
    // Runs on every load; both steps are self-disabling once a layout is re-saved,
    // because every block then carries an explicit `col` value.
    migrateBlocks(raw) {
        const blocks = (Array.isArray(raw) ? raw : []).map(b => ({ ...b, props: { ...(b.props || {}) } }));
        const notes  = [];

        // 1. Legacy `same_line` / `width` inline pairing → column placement
        let sameLine = 0;
        for (const b of blocks) {
            if (b.props.same_line === undefined) continue;
            if (b.props.same_line && b.props.col === undefined) {
                b.props.col = 'left';
                if (b.type !== 'logo' && b.props.width) b.props.col_width = b.props.width;
                sameLine++;
            }
            delete b.props.same_line;
        }
        if (sameLine) notes.push(`${sameLine} "Same Line" block(s) converted to columns`);

        // 2. Layouts saved before columns existed: put the document details beside
        //    the recipient, which is where they could never go before.
        if (!blocks.some(b => b.props.col !== undefined)) {
            const to = blocks.find(b => b.type === 'to_address');
            const dd = blocks.find(b => b.type === 'doc_details');
            if (to && dd) {
                to.props.col = 'left';
                if (to.props.col_width === undefined) to.props.col_width = 55;
                dd.props.col = 'right';
                notes.push('document details moved beside the recipient address');
            }
        }
        return { blocks, notes };
    }

    async onDocTypeChange(model) {
        this.state.docModel      = model;
        this.state.selectedBlock = null;
        this.state.saving        = false;
        this.state.saved         = false;
        this.state.templateId    = null;
        this.state.templateHtml  = '';
        this.state.previewDoc    = '';
        this.state.defaultHtml   = '';
        this.state.isCustomized  = false;
        this.state.showDiff      = false;
        this.state.confirmReset  = false;
        // Load template record (for paper_format, orientation, and templateId)
        try {
            const tpls = await RpcService.call('ir.report.template', 'search_read',
                [[['model', '=', model]]],
                { fields: ['id', 'paper_format', 'orientation', 'decimal_qty', 'decimal_price', 'decimal_subtotal', 'margin_top', 'margin_right', 'margin_bottom', 'margin_left', 'font_size', 'font_color', 'line_height', 'footer_text'], limit: 1 });
            if (tpls && tpls.length > 0) {
                this.state.templateId = tpls[0].id;
                this.state.docSettings.paper_format      = tpls[0].paper_format      || 'A4';
                this.state.docSettings.orientation       = tpls[0].orientation       || 'portrait';
                this.state.docSettings.decimal_qty       = tpls[0].decimal_qty       ?? 2;
                this.state.docSettings.decimal_price     = tpls[0].decimal_price     ?? 2;
                this.state.docSettings.decimal_subtotal  = tpls[0].decimal_subtotal  ?? 2;
                this.state.docSettings.margin_top        = tpls[0].margin_top        ?? 15;
                this.state.docSettings.margin_right      = tpls[0].margin_right      ?? 18;
                this.state.docSettings.margin_bottom     = tpls[0].margin_bottom     ?? 18;
                this.state.docSettings.margin_left       = tpls[0].margin_left       ?? 18;
                this.state.docSettings.font_size         = tpls[0].font_size         ?? 10;
                this.state.docSettings.font_color        = tpls[0].font_color        || '#333333';
                this.state.docSettings.line_height       = tpls[0].line_height       ?? 1.5;
                this.state.docSettings.footer_text       = tpls[0].footer_text       || '';
                // default_html only comes back from read(), not search_read()
                const full = await RpcService.call('ir.report.template', 'read', [[tpls[0].id]], {});
                if (Array.isArray(full) && full.length) {
                    this.state.defaultHtml  = full[0].default_html || '';
                    this.state.isCustomized = !!full[0].is_customized;
                    this.storedHtml         = full[0].template_html || '';
                }
            }
        } catch (e) { console.error(e); }
        // Load block config
        try {
            const cfgKey = 'layout.blocks.' + model;
            const cfgRows = await RpcService.call('ir.config.parameter', 'search_read',
                [[['key', '=', cfgKey]]], { fields: ['id', 'value'], limit: 1 });
            if (cfgRows && cfgRows.length > 0 && cfgRows[0].value) {
                const migrated = this.migrateBlocks(JSON.parse(cfgRows[0].value));
                this.state.blocks = migrated.blocks;
                if (migrated.notes.length) this.addLog(`Layout upgraded — ${migrated.notes.join('; ')}`);
            } else {
                this.state.blocks = dleDefaultBlocks(model);
            }
        } catch (e) {
            this.state.blocks = dleDefaultBlocks(model);
        }
        // Load design settings
        try {
            const designRows = await RpcService.call('ir.config.parameter', 'search_read',
                [[['key', 'in', ['report.design.accent_color', 'report.design.font_family']]]],
                { fields: ['key', 'value'], limit: 10 });
            for (const row of (Array.isArray(designRows) ? designRows : [])) {
                if (row.key === 'report.design.accent_color' && row.value) this.state.docSettings.accent_color = row.value;
                if (row.key === 'report.design.font_family'  && row.value) this.state.docSettings.font_family  = row.value;
            }
        } catch (e) {}
        // Generate template HTML from blocks and refresh preview
        this.rebuildHtml();
        const docLabel = DLE_DOC_TYPES.find(d => d.model === model)?.label || model;
        this.addLog(`Loaded template: ${docLabel} (${this.state.blocks.length} blocks)`);
    }

    async onSave() {
        this.state.saving = true;
        this.state.saved  = false;
        try {
            // In blocks mode: regenerate HTML from current block arrangement before saving
            if (this.state.leftTab !== 'html') {
                this.state.templateHtml = dleBuildHtml(this.state.blocks, this.state.docModel);
            }
            // Save template HTML + settings
            if (this.state.templateId) {
                // Extract footer block settings so the PDF route reads live values
                // from ir_report_template (its source of truth for --footer-html generation).
                const footerBlk = this.state.blocks.find(b => b.type === 'footer_bar' && b.visible !== false);
                const fp = footerBlk ? (footerBlk.props || {}) : {};
                const fpSrc   = fp.text_source    || 'custom';
                const fpText  = fpSrc === 'custom' ? (fp.content || '') : '';
                const fpShow  = fp.show_page_num  !== false;
                const fpFmt   = fp.page_num_fmt   || 'Page {p} of {t}';
                const fpLCol  = fp.top_sep ? (fp.top_sep_color || '#cccccc') : '#cccccc';
                // fpLW = 0 when separator is disabled so PDF skips border-top entirely
                const fpLW    = fp.top_sep ? (parseFloat(fp.line_width) || 0.5) : 0;

                await RpcService.call('ir.report.template', 'write',
                    [[this.state.templateId], {
                        template_html:         this.state.templateHtml,
                        paper_format:          this.state.docSettings.paper_format,
                        orientation:           this.state.docSettings.orientation,
                        decimal_qty:           Number.isInteger(this.state.docSettings.decimal_qty)      ? this.state.docSettings.decimal_qty      : 2,
                        decimal_price:         Number.isInteger(this.state.docSettings.decimal_price)    ? this.state.docSettings.decimal_price    : 2,
                        decimal_subtotal:      Number.isInteger(this.state.docSettings.decimal_subtotal) ? this.state.docSettings.decimal_subtotal : 2,
                        margin_top:            parseFloat(this.state.docSettings.margin_top)    || 15,
                        margin_right:          parseFloat(this.state.docSettings.margin_right)  || 18,
                        margin_bottom:         parseFloat(this.state.docSettings.margin_bottom) || 18,
                        margin_left:           parseFloat(this.state.docSettings.margin_left)   || 18,
                        font_size:             parseInt(this.state.docSettings.font_size)       || 10,
                        font_color:            this.state.docSettings.font_color  || '#333333',
                        line_height:           parseFloat(this.state.docSettings.line_height)   || 1.5,
                        footer_text:           fpText,
                        footer_show_page_num:  fpShow,
                        footer_page_num_fmt:   fpFmt,
                        footer_text_source:    fpSrc,
                        footer_line_color:     fpLCol,
                        footer_line_width:     fpLW,
                    }], {});
            }
            // Save block config
            const cfgKey   = 'layout.blocks.' + this.state.docModel;
            const cfgValue = JSON.stringify(this.state.blocks);
            const existRows = await RpcService.call('ir.config.parameter', 'search_read',
                [[['key', '=', cfgKey]]], { fields: ['id'], limit: 1 });
            if (existRows && existRows.length > 0) {
                await RpcService.call('ir.config.parameter', 'write', [[existRows[0].id], { value: cfgValue }], {});
            } else {
                await RpcService.call('ir.config.parameter', 'create', [{ key: cfgKey, value: cfgValue }], {});
            }
            // Save design settings
            const designSaves = [
                ['report.design.accent_color', this.state.docSettings.accent_color],
                ['report.design.font_family',  this.state.docSettings.font_family],
            ];
            for (const [key, value] of designSaves) {
                const rows = await RpcService.call('ir.config.parameter', 'search_read',
                    [[['key', '=', key]]], { fields: ['id'], limit: 1 });
                if (rows && rows.length > 0) {
                    await RpcService.call('ir.config.parameter', 'write', [[rows[0].id], { value }], {});
                } else {
                    await RpcService.call('ir.config.parameter', 'create', [{ key, value }], {});
                }
            }
            this.state.saved = true;
            // Refresh the Customized badge against the shipped baseline.
            if (this.state.templateId) {
                try {
                    const full = await RpcService.call('ir.report.template', 'read', [[this.state.templateId]], {});
                    if (Array.isArray(full) && full.length) {
                        this.state.isCustomized = !!full[0].is_customized;
                        this.storedHtml         = full[0].template_html || '';
                    }
                } catch (e) {}
            }
            const docLabel = DLE_DOC_TYPES.find(d => d.model === this.state.docModel)?.label || this.state.docModel;
            this.addLog(`Saved: ${docLabel} template (${this.state.blocks.length} blocks)`);
            setTimeout(() => { if (this.state) this.state.saved = false; }, 3000);
        } catch (e) {
            this.addLog(`Error saving: ${e.message || e}`);
            console.error(e);
        }
        finally { this.state.saving = false; }
    }
}

// ----------------------------------------------------------------
// ReportSettingsView — editor for ir.report.template records + report config settings
// ----------------------------------------------------------------
class ReportSettingsView extends Component {
    static template = xml`
        <div class="report-settings">
            <!-- Tab bar -->
            <div class="report-tabs">
                <button t-attf-class="report-tab{{ state.activeTab === 'templates' ? ' active' : '' }}"
                        t-on-click="() => this.setTab('templates')">Templates</button>
                <button t-attf-class="report-tab{{ state.activeTab === 'settings' ? ' active' : '' }}"
                        t-on-click="() => this.setTab('settings')">Report Settings</button>
            </div>

            <!-- ── TAB: Templates ── -->
            <t t-if="state.activeTab === 'templates'">
                <div class="report-settings-inner">
                    <!-- Template list -->
                    <div class="report-template-list">
                        <h3>Templates</h3>
                        <t t-if="state.loading">
                            <div class="loading">Loading…</div>
                        </t>
                        <t t-elif="state.error">
                            <div class="error" t-esc="state.error"/>
                        </t>
                        <t t-else="">
                            <t t-foreach="state.templates" t-as="tpl" t-key="tpl.id">
                                <div t-attf-class="rpt-list-item{{ state.selectedId === tpl.id ? ' active' : '' }}"
                                     t-on-click="() => this.selectTemplate(tpl.id)">
                                    <div t-esc="tpl.name"/>
                                    <div class="rpt-list-model" t-esc="tpl.model"/>
                                </div>
                            </t>
                        </t>
                    </div>

                    <!-- Editor panel -->
                    <div class="report-template-editor-panel">
                        <t t-if="!state.selectedId">
                            <div class="report-empty-state">Select a template to edit</div>
                        </t>
                        <t t-elif="state.loadingTemplate">
                            <div class="loading">Loading template…</div>
                        </t>
                        <t t-else="">
                            <!-- Header row: name + save/preview actions -->
                            <div class="rpt-editor-header">
                                <h3 t-esc="state.template ? state.template.name : ''"/>
                                <div class="rpt-editor-actions">
                                    <input type="number" min="1" t-model="state.previewId"
                                           placeholder="Record ID" class="rpt-preview-id"/>
                                    <button class="btn" t-on-click="onPreview">Preview PDF</button>
                                    <button class="btn btn-primary"
                                            t-att-disabled="state.saving ? true : undefined"
                                            t-on-click="onSave">
                                        <t t-if="state.saving">Saving…</t>
                                        <t t-else="">Save</t>
                                    </button>
                                    <t t-if="state.dirty">
                                        <span class="rpt-status muted">Unsaved changes</span>
                                    </t>
                                    <t t-if="state.saved">
                                        <span class="rpt-status ok">Saved!</span>
                                    </t>
                                </div>
                            </div>

                            <!-- Page Setup -->
                            <div class="rpt-section">
                                <div class="rpt-section-title">Page Setup</div>
                                <div class="rpt-grid rpt-grid-4">
                                    <div class="rpt-field">
                                        <label>Paper Format</label>
                                        <select t-on-change="onPaperFormatChange">
                                            <option value="A4" t-att-selected="state.template.paper_format === 'A4' ? true : undefined">A4</option>
                                            <option value="Letter" t-att-selected="state.template.paper_format === 'Letter' ? true : undefined">Letter</option>
                                            <option value="A3" t-att-selected="state.template.paper_format === 'A3' ? true : undefined">A3</option>
                                            <option value="Legal" t-att-selected="state.template.paper_format === 'Legal' ? true : undefined">Legal</option>
                                        </select>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Orientation</label>
                                        <select t-on-change="onOrientationChange">
                                            <option value="portrait" t-att-selected="state.template.orientation === 'portrait' ? true : undefined">Portrait</option>
                                            <option value="landscape" t-att-selected="state.template.orientation === 'landscape' ? true : undefined">Landscape</option>
                                        </select>
                                    </div>
                                </div>
                                <div class="rpt-grid rpt-grid-4" style="margin-top:8px;">
                                    <div class="rpt-field">
                                        <label>Top Margin (mm)</label>
                                        <input type="number" step="0.5" min="0"
                                               t-att-value="state.template.margin_top"
                                               t-on-change="(e) => this.onNumField(e, 'margin_top')"/>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Right (mm)</label>
                                        <input type="number" step="0.5" min="0"
                                               t-att-value="state.template.margin_right"
                                               t-on-change="(e) => this.onNumField(e, 'margin_right')"/>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Bottom (mm)</label>
                                        <input type="number" step="0.5" min="0"
                                               t-att-value="state.template.margin_bottom"
                                               t-on-change="(e) => this.onNumField(e, 'margin_bottom')"/>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Left (mm)</label>
                                        <input type="number" step="0.5" min="0"
                                               t-att-value="state.template.margin_left"
                                               t-on-change="(e) => this.onNumField(e, 'margin_left')"/>
                                    </div>
                                </div>
                            </div>

                            <!-- Typography -->
                            <div class="rpt-section">
                                <div class="rpt-section-title">Typography</div>
                                <div class="rpt-grid rpt-grid-4">
                                    <div class="rpt-field">
                                        <label>Font Size (pt)</label>
                                        <input type="number" step="1" min="6" max="24"
                                               t-att-value="state.template.font_size"
                                               t-on-change="(e) => this.onNumField(e, 'font_size')"/>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Font Color</label>
                                        <div class="rpt-color-row">
                                            <input type="color"
                                                   t-att-value="state.template.font_color || '#333333'"
                                                   t-on-input="(e) => this.onColorField(e, 'font_color')"/>
                                            <input type="text"
                                                   t-att-value="state.template.font_color"
                                                   t-on-change="(e) => this.onTextField(e, 'font_color')"/>
                                        </div>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Line Height</label>
                                        <input type="number" step="0.1" min="1" max="3"
                                               t-att-value="state.template.line_height"
                                               t-on-change="(e) => this.onNumField(e, 'line_height')"/>
                                    </div>
                                </div>
                                <div class="rpt-grid rpt-grid-4" style="margin-top:8px;">
                                    <div class="rpt-field">
                                        <label>Qty Decimals</label>
                                        <input type="number" step="1" min="0" max="6"
                                               t-att-value="state.template.decimal_qty"
                                               t-on-change="(e) => this.onIntField(e, 'decimal_qty')"/>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Price Decimals</label>
                                        <input type="number" step="1" min="0" max="6"
                                               t-att-value="state.template.decimal_price"
                                               t-on-change="(e) => this.onIntField(e, 'decimal_price')"/>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Subtotal Decimals</label>
                                        <input type="number" step="1" min="0" max="6"
                                               t-att-value="state.template.decimal_subtotal"
                                               t-on-change="(e) => this.onIntField(e, 'decimal_subtotal')"/>
                                    </div>
                                </div>
                            </div>

                            <!-- Footer -->
                            <div class="rpt-section">
                                <div class="rpt-section-title">Footer</div>
                                <div class="rpt-grid rpt-grid-4">
                                    <div class="rpt-field">
                                        <label>Text Source</label>
                                        <select t-on-change="(e) => this.onTextField(e, 'footer_text_source')">
                                            <option value="none"
                                                    t-att-selected="state.template.footer_text_source === 'none' ? true : undefined">None</option>
                                            <option value="custom"
                                                    t-att-selected="(!state.template.footer_text_source || state.template.footer_text_source === 'custom') ? true : undefined">Custom Text</option>
                                            <option value="website"
                                                    t-att-selected="state.template.footer_text_source === 'website' ? true : undefined">Website (Report Settings)</option>
                                        </select>
                                    </div>
                                    <t t-if="!state.template.footer_text_source || state.template.footer_text_source === 'custom'">
                                        <div class="rpt-field rpt-span3">
                                            <label>Custom Text</label>
                                            <input type="text"
                                                   t-att-value="state.template.footer_text"
                                                   t-on-change="(e) => this.onTextField(e, 'footer_text')"
                                                   placeholder="e.g. www.example.com or company tagline"/>
                                        </div>
                                    </t>
                                </div>
                                <div class="rpt-grid rpt-grid-4" style="margin-top:8px;">
                                    <div class="rpt-field rpt-field-check">
                                        <label>
                                            <input type="checkbox"
                                                   t-att-checked="state.template.footer_show_page_num ? true : undefined"
                                                   t-on-change="(e) => this.onCheckField(e, 'footer_show_page_num')"/>
                                            Show Page Numbers
                                        </label>
                                    </div>
                                    <t t-if="state.template.footer_show_page_num">
                                        <div class="rpt-field">
                                            <label>Format</label>
                                            <select t-on-change="(e) => this.onTextField(e, 'footer_page_num_fmt')">
                                                <option value="Page {p} of {t}"
                                                        t-att-selected="(!state.template.footer_page_num_fmt || state.template.footer_page_num_fmt === 'Page {p} of {t}') ? true : undefined">Page X of Y</option>
                                                <option value="{p} / {t}"
                                                        t-att-selected="state.template.footer_page_num_fmt === '{p} / {t}' ? true : undefined">X / Y</option>
                                                <option value="Page {p}"
                                                        t-att-selected="state.template.footer_page_num_fmt === 'Page {p}' ? true : undefined">Page X</option>
                                                <option value="{p}"
                                                        t-att-selected="state.template.footer_page_num_fmt === '{p}' ? true : undefined">X only</option>
                                            </select>
                                        </div>
                                    </t>
                                </div>
                                <div class="rpt-grid rpt-grid-4" style="margin-top:8px;">
                                    <div class="rpt-field">
                                        <label>Separator Color</label>
                                        <div class="rpt-color-row">
                                            <input type="color"
                                                   t-att-value="state.template.footer_line_color || '#cccccc'"
                                                   t-on-input="(e) => this.onColorField(e, 'footer_line_color')"/>
                                            <input type="text"
                                                   t-att-value="state.template.footer_line_color"
                                                   t-on-change="(e) => this.onTextField(e, 'footer_line_color')"/>
                                        </div>
                                    </div>
                                    <div class="rpt-field">
                                        <label>Separator Weight (pt)</label>
                                        <input type="number" step="0.25" min="0" max="4"
                                               t-att-value="state.template.footer_line_width"
                                               t-on-change="(e) => this.onNumField(e, 'footer_line_width')"/>
                                    </div>
                                </div>
                            </div>

                            <!-- HTML Template editor -->
                            <div class="rpt-section">
                                <div class="rpt-section-title">HTML Template</div>
                                <textarea class="report-template-editor"
                                          t-on-input="onEditorInput"
                                          t-ref="editorRef">
                                    <t t-esc="state.template.template_html"/>
                                </textarea>
                            </div>
                        </t>
                    </div>
                </div>
            </t>

            <!-- ── TAB: Report Settings ── -->
            <t t-if="state.activeTab === 'settings'">
                <div class="report-cfg-panel">
                    <t t-if="state.cfgLoading">
                        <div class="loading">Loading settings…</div>
                    </t>
                    <t t-elif="state.cfgError">
                        <div class="error" t-esc="state.cfgError"/>
                    </t>
                    <t t-else="">
                        <h3>Company &amp; Invoice Settings</h3>
                        <table class="report-cfg-table">
                            <tbody>
                                <tr>
                                    <td><label>Company Registration No.</label></td>
                                    <td><input type="text" t-model="state.cfg['report.reg_number']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Company Website</label></td>
                                    <td><input type="text" t-model="state.cfg['report.website']" placeholder="e.g. www.example.com"/></td>
                                </tr>
                                <tr>
                                    <td><label>Address Line 1</label></td>
                                    <td><input type="text" t-model="state.cfg['report.addr1']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Address Line 2</label></td>
                                    <td><input type="text" t-model="state.cfg['report.addr2']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Address Line 3</label></td>
                                    <td><input type="text" t-model="state.cfg['report.addr3']"/></td>
                                </tr>
                                <tr>
                                    <td><label>City &amp; Country</label></td>
                                    <td><input type="text" t-model="state.cfg['report.city_country']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Currency Code</label></td>
                                    <td><input type="text" t-model="state.cfg['report.currency_code']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Payment Terms (Days)</label></td>
                                    <td><input type="text" t-model="state.cfg['report.payment_term_days']"/></td>
                                </tr>
                                <tr><td colspan="2"><hr/><strong>Bank Details</strong></td></tr>
                                <tr>
                                    <td><label>Bank Account Name</label></td>
                                    <td><input type="text" t-model="state.cfg['report.bank.account_name']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Bank Account No.</label></td>
                                    <td><input type="text" t-model="state.cfg['report.bank.account_no']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Bank Name</label></td>
                                    <td><input type="text" t-model="state.cfg['report.bank.bank_name']"/></td>
                                </tr>
                                <tr>
                                    <td><label>Bank Address</label></td>
                                    <td><input type="text" t-model="state.cfg['report.bank.bank_address']"/></td>
                                </tr>
                                <tr>
                                    <td><label>SWIFT Code</label></td>
                                    <td><input type="text" t-model="state.cfg['report.bank.swift_code']"/></td>
                                </tr>
                            </tbody>
                        </table>
                        <div style="margin-top:12px;">
                            <button class="btn btn-primary"
                                    t-att-disabled="state.cfgSaving ? true : undefined"
                                    t-on-click="onSaveCfg">
                                <t t-if="state.cfgSaving">Saving…</t>
                                <t t-else="">Save Settings</t>
                            </button>
                            <t t-if="state.cfgSaved">
                                <span style="color:var(--ok);font-size:.8rem;margin-left:8px;">Saved!</span>
                            </t>
                            <t t-if="state.cfgSaveError">
                                <span style="color:var(--danger);font-size:.8rem;margin-left:8px;" t-esc="state.cfgSaveError"/>
                            </t>
                        </div>
                    </t>
                </div>
            </t>
        </div>
    `;

    // Config param keys we manage
    static CFG_KEYS = [
        'report.reg_number',
        'report.website',
        'report.addr1',
        'report.addr2',
        'report.addr3',
        'report.city_country',
        'report.currency_code',
        'report.payment_term_days',
        'report.bank.account_name',
        'report.bank.account_no',
        'report.bank.bank_name',
        'report.bank.bank_address',
        'report.bank.swift_code',
    ];

    setup() {
        this.state = useState({
            activeTab:       'templates',
            // Templates tab
            loading:         true,
            error:           '',
            templates:       [],
            selectedId:      null,
            template:        null,
            loadingTemplate: false,
            dirty:           false,
            saving:          false,
            saved:           false,
            previewId:       '',
            // Settings tab
            cfgLoading:      false,
            cfgError:        '',
            cfg:             {},
            // Map key -> record id for write
            cfgIds:          {},
            cfgSaving:       false,
            cfgSaved:        false,
            cfgSaveError:    '',
        });
        this.editorRef = owl.useRef('editorRef');
        onMounted(() => this.loadTemplates());
    }

    setTab(tab) {
        this.state.activeTab = tab;
        if (tab === 'settings' && !this.state.cfgLoading && Object.keys(this.state.cfg).length === 0) {
            this.loadCfg();
        }
    }

    async loadTemplates() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const recs = await RpcService.call(
                'ir.report.template', 'search_read', [[]], { fields: ['id','name','model','paper_format','orientation'], limit: 50 });
            this.state.templates = Array.isArray(recs) ? recs : [];
        } catch (e) {
            this.state.error = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async loadCfg() {
        this.state.cfgLoading = true;
        this.state.cfgError   = '';
        try {
            // Load all report.* config params at once
            const params = await RpcService.call(
                'ir.config.parameter', 'search_read',
                [[['key', 'like', 'report.']]], { fields: ['id', 'key', 'value'], limit: 100 });

            const cfg   = {};
            const cfgIds = {};
            // Initialise all expected keys to empty string
            for (const k of ReportSettingsView.CFG_KEYS) cfg[k] = '';

            if (Array.isArray(params)) {
                for (const p of params) {
                    cfg[p.key]    = p.value || '';
                    cfgIds[p.key] = p.id;
                }
            }
            this.state.cfg    = cfg;
            this.state.cfgIds = cfgIds;
        } catch (e) {
            this.state.cfgError = e.message || 'Failed to load settings';
        } finally {
            this.state.cfgLoading = false;
        }
    }

    async onSaveCfg() {
        this.state.cfgSaving    = true;
        this.state.cfgSaved     = false;
        this.state.cfgSaveError = '';
        try {
            for (const key of ReportSettingsView.CFG_KEYS) {
                const value = this.state.cfg[key] || '';
                const id    = this.state.cfgIds[key];
                if (id) {
                    // Record exists — write
                    await RpcService.call(
                        'ir.config.parameter', 'write',
                        [[id], { value }], {});
                } else {
                    // Record missing — create
                    const newId = await RpcService.call(
                        'ir.config.parameter', 'create',
                        [{ key, value }], {});
                    this.state.cfgIds[key] = newId;
                }
            }
            this.state.cfgSaved = true;
            setTimeout(() => { this.state.cfgSaved = false; }, 2500);
        } catch (e) {
            this.state.cfgSaveError = e.message || 'Save failed';
        } finally {
            this.state.cfgSaving = false;
        }
    }

    async selectTemplate(id) {
        if (this.state.selectedId === id) return;
        this.state.selectedId      = id;
        this.state.template        = null;
        this.state.loadingTemplate = true;
        this.state.dirty           = false;
        this.state.saved           = false;
        try {
            const recs = await RpcService.call(
                'ir.report.template', 'read', [[id]], {});
            if (Array.isArray(recs) && recs.length > 0) {
                const tpl = { ...recs[0] };
                // Defaults for new footer fields (in case DB has NULLs)
                if (tpl.footer_show_page_num === null || tpl.footer_show_page_num === undefined)
                    tpl.footer_show_page_num = true;
                if (!tpl.footer_page_num_fmt) tpl.footer_page_num_fmt = 'Page {p} of {t}';
                if (!tpl.footer_text_source)  tpl.footer_text_source  = 'custom';
                if (!tpl.footer_line_color)   tpl.footer_line_color   = '#cccccc';
                if (tpl.footer_line_width === null || tpl.footer_line_width === undefined)
                    tpl.footer_line_width = 0.5;
                this.state.template = tpl;
                // After render, set textarea value and auto-size
                setTimeout(() => {
                    const el = this.editorRef.el;
                    if (el) {
                        el.value = this.state.template.template_html || '';
                        el.style.height = 'auto';
                        el.style.height = el.scrollHeight + 'px';
                    }
                }, 30);
            }
        } catch (e) {
            console.error('Failed to load template:', e);
        } finally {
            this.state.loadingTemplate = false;
        }
    }

    onEditorInput(e) {
        if (this.state.template) {
            this.state.template.template_html = e.target.value;
            this.state.dirty = true;
            this.state.saved = false;
        }
        e.target.style.height = 'auto';
        e.target.style.height = e.target.scrollHeight + 'px';
    }

    onPaperFormatChange(e) {
        if (this.state.template) {
            this.state.template.paper_format = e.target.value;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    onOrientationChange(e) {
        if (this.state.template) {
            this.state.template.orientation = e.target.value;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    // Generic field setters used by arrow-function handlers in the template
    onNumField(e, field) {
        if (this.state.template) {
            const v = parseFloat(e.target.value);
            this.state.template[field] = isNaN(v) ? 0 : v;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    onIntField(e, field) {
        if (this.state.template) {
            const v = parseInt(e.target.value, 10);
            this.state.template[field] = isNaN(v) ? 0 : v;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    onTextField(e, field) {
        if (this.state.template) {
            this.state.template[field] = e.target.value;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    onColorField(e, field) {
        if (this.state.template) {
            this.state.template[field] = e.target.value;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    onCheckField(e, field) {
        if (this.state.template) {
            this.state.template[field] = e.target.checked;
            this.state.dirty = true;
            this.state.saved = false;
        }
    }

    async onSave() {
        if (!this.state.template || !this.state.selectedId) return;
        this.state.saving = true;
        try {
            const el = this.editorRef.el;
            if (el) this.state.template.template_html = el.value;

            const t = this.state.template;
            await RpcService.call(
                'ir.report.template', 'write',
                [[this.state.selectedId], {
                    template_html:         t.template_html,
                    paper_format:          t.paper_format,
                    orientation:           t.orientation,
                    margin_top:            t.margin_top,
                    margin_right:          t.margin_right,
                    margin_bottom:         t.margin_bottom,
                    margin_left:           t.margin_left,
                    font_size:             t.font_size,
                    font_color:            t.font_color,
                    line_height:           t.line_height,
                    decimal_qty:           t.decimal_qty,
                    decimal_price:         t.decimal_price,
                    decimal_subtotal:      t.decimal_subtotal,
                    footer_text:           t.footer_text,
                    footer_show_page_num:  t.footer_show_page_num,
                    footer_page_num_fmt:   t.footer_page_num_fmt,
                    footer_text_source:    t.footer_text_source,
                    footer_line_color:     t.footer_line_color,
                    footer_line_width:     t.footer_line_width,
                }], {});
            this.state.dirty = false;
            this.state.saved = true;
            setTimeout(() => { this.state.saved = false; }, 2000);
        } catch (e) {
            console.error('Save failed:', e);
        } finally {
            this.state.saving = false;
        }
    }

    onPreview() {
        if (!this.state.template) return;
        const id = this.state.previewId || '1';
        window.open('/report/pdf/' + this.state.template.model + '/' + id, '_blank');
    }
}

// ----------------------------------------------------------------
// PortalUserListView — manage portal user access & passwords
// ----------------------------------------------------------------
class PortalUserListView extends Component {
    static template = xml`
        <div class="portal-user-list">
            <div class="portal-filter-bar">
                <label style="font-size:.85rem;font-weight:600">Filter by Company:</label>
                <select class="form-select" t-on-change="onCompanyFilter">
                    <option value="">All Companies</option>
                    <t t-foreach="state.companies" t-as="co" t-key="co.id">
                        <option t-att-value="co.id" t-esc="co.name"/>
                    </t>
                </select>
                <span t-if="state.loading" class="text-muted" style="font-size:.8rem">Loading…</span>
                <span style="flex:1"/>
                <span class="text-muted" style="font-size:.8rem" t-esc="state.users.length + ' partners'"/>
            </div>

            <table class="list-table">
                <thead>
                    <tr>
                        <th>Name</th>
                        <th>Company</th>
                        <th>Email</th>
                        <th>Portal Access</th>
                        <th>Password</th>
                        <th>Actions</th>
                    </tr>
                </thead>
                <tbody>
                    <t t-if="state.users.length === 0 and !state.loading">
                        <tr><td colspan="6" style="text-align:center;padding:32px;color:var(--muted)">
                            No partners found.
                        </td></tr>
                    </t>
                    <t t-foreach="state.users" t-as="user" t-key="user.id">
                        <tr>
                            <td style="font-weight:500" t-esc="user.name"/>
                            <td t-esc="user.company_name || '—'"/>
                            <td t-esc="user.email || '—'"/>
                            <td>
                                <label class="toggle-switch">
                                    <input type="checkbox"
                                           t-att-checked="user.portal_active ? true : undefined"
                                           t-on-change="() => this.toggleActive(user)"/>
                                    <span class="toggle-slider"/>
                                </label>
                            </td>
                            <td>
                                <span t-if="user.has_password" class="badge-yes">✓ Set</span>
                                <span t-else="" class="badge-no">✗ None</span>
                            </td>
                            <td>
                                <div style="display:flex;gap:6px;flex-wrap:wrap">
                                    <button class="btn btn-sm" t-on-click="() => this.openPasswordDialog(user)">Set Password</button>
                                    <button class="btn btn-sm" t-on-click="() => this.resetPassword(user)">Reset to Default</button>
                                </div>
                            </td>
                        </tr>
                    </t>
                </tbody>
            </table>

            <!-- Password dialog -->
            <t t-if="state.dialog">
                <div class="pay-overlay" t-on-click.stop="closeDialog">
                    <div class="pay-dialog" t-on-click.stop="" style="min-width:380px">
                        <div class="pay-dialog-title">Set Portal Password</div>
                        <div style="font-size:.85rem;color:var(--muted);margin-bottom:4px">
                            Partner: <strong t-esc="state.dialog.name"/>
                        </div>
                        <div>
                            <label style="font-size:.8rem;font-weight:600;display:block;margin-bottom:4px">New Password</label>
                            <input type="password" class="form-input" style="width:100%"
                                   placeholder="Minimum 8 characters"
                                   t-ref="pwInput"/>
                        </div>
                        <div>
                            <label style="font-size:.8rem;font-weight:600;display:block;margin-bottom:4px">Confirm Password</label>
                            <input type="password" class="form-input" style="width:100%"
                                   placeholder="Repeat password"
                                   t-ref="pwConfirm"/>
                        </div>
                        <t t-if="state.dialogError">
                            <div class="pay-dialog-error" t-esc="state.dialogError"/>
                        </t>
                        <div class="pay-dialog-actions">
                            <button class="btn" t-on-click.stop="closeDialog">Cancel</button>
                            <button class="btn btn-primary" t-on-click.stop="savePassword">Save Password</button>
                        </div>
                    </div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading: false,
            users:       [],
            companies:   [],
            companyFilter: '',
            dialog:      null,   // { id, name }
            dialogError: '',
        });
        this.pwInput   = useRef('pwInput');
        this.pwConfirm = useRef('pwConfirm');
        onMounted(() => {
            this.loadCompanies();
            this.loadUsers();
        });
    }

    async loadCompanies() {
        try {
            const cos = await RpcService.call('portal.partner', 'get_companies', [[]], {});
            this.state.companies = Array.isArray(cos) ? cos : [];
        } catch (e) {
            console.error('loadCompanies failed:', e);
        }
    }

    async loadUsers() {
        this.state.loading = true;
        try {
            const domain = this.state.companyFilter
                ? [['company_id', '=', parseInt(this.state.companyFilter)]]
                : [];
            const users = await RpcService.call(
                'portal.partner', 'search_read',
                [domain],
                { fields: ['id','name','email','company_name','portal_active','has_password'], limit: 200 });
            this.state.users = Array.isArray(users) ? users : [];
        } catch (e) {
            console.error('loadUsers failed:', e);
        } finally {
            this.state.loading = false;
        }
    }

    onCompanyFilter(ev) {
        this.state.companyFilter = ev.target.value;
        this.loadUsers();
    }

    async toggleActive(user) {
        const newVal = !user.portal_active;
        try {
            await RpcService.call('portal.partner', 'write', [[user.id], { portal_active: newVal }]);
            user.portal_active = newVal;
        } catch (e) {
            console.error('toggleActive failed:', e);
        }
    }

    openPasswordDialog(user) {
        this.state.dialog      = { id: user.id, name: user.name };
        this.state.dialogError = '';
    }

    closeDialog() {
        this.state.dialog      = null;
        this.state.dialogError = '';
    }

    async savePassword() {
        const pw  = this.pwInput.el?.value   || '';
        const pw2 = this.pwConfirm.el?.value || '';

        if (pw.length < 8) {
            this.state.dialogError = 'Password must be at least 8 characters.';
            return;
        }
        if (pw !== pw2) {
            this.state.dialogError = 'Passwords do not match.';
            return;
        }
        try {
            await RpcService.call('portal.partner', 'set_portal_password',
                [[this.state.dialog.id]], { password: pw });
            this.closeDialog();
            await this.loadUsers();
        } catch (e) {
            this.state.dialogError = e.message || 'Failed to set password.';
        }
    }

    async resetPassword(user) {
        if (!confirm(`Reset portal password for "${user.name}" to the default "Welcome1"?`)) return;
        try {
            await RpcService.call('portal.partner', 'portal_reset_password', [[user.id]], {});
            await this.loadUsers();
        } catch (e) {
            console.error('resetPassword failed:', e);
        }
    }
}

// ----------------------------------------------------------------
// GROUP_DEFS — module groupings for Access Rights tab
// ----------------------------------------------------------------
const GROUP_DEFS = [
    { label: 'Technical', type: 'checkboxes', groups: [
        { id: 2,  label: 'Internal User',  hint: 'Required for all internal access' },
        { id: 3,  label: 'Administrator',  hint: 'Full access to all modules' },
        { id: 4,  label: 'Configuration',  hint: 'Can modify system settings' },
    ]},
    { label: 'Accounting', type: 'radio', noneLabel: 'None',
      groups: [
        { id: 5, label: 'Billing' },
        { id: 6, label: 'Accountant' },
    ]},
    { label: 'Sales', type: 'radio', noneLabel: 'None',
      groups: [
        { id: 7, label: 'User' },
        { id: 8, label: 'Manager' },
    ]},
    { label: 'Purchase', type: 'radio', noneLabel: 'None',
      groups: [
        { id: 9,  label: 'User' },
        { id: 10, label: 'Manager' },
    ]},
    { label: 'Inventory', type: 'radio', noneLabel: 'None',
      groups: [
        { id: 11, label: 'User' },
        { id: 12, label: 'Manager' },
    ]},
    { label: 'Manufacturing', type: 'radio', noneLabel: 'None',
      groups: [
        { id: 13, label: 'User' },
        { id: 14, label: 'Manager' },
    ]},
    { label: 'Human Resources', type: 'radio', noneLabel: 'None',
      groups: [
        { id: 15, label: 'Employee' },
        { id: 16, label: 'Manager' },
    ]},
];

// ----------------------------------------------------------------
// UserFormView — create / edit a res.users record
// ----------------------------------------------------------------
class UserFormView extends Component {
    static template = xml`
        <div class="so-shell">
            <!-- Breadcrumb -->
            <div class="so-topbar">
                <button class="btn" t-on-click="onBack">&#8592; Users</button>
                <span class="so-ref" t-esc="state.record.login || 'New User'"/>
                <div style="flex:1"/>
                <button class="btn btn-primary" t-on-click="onSave" t-att-disabled="state.saving">
                    <t t-if="state.saving">Saving…</t><t t-else="">Save</t>
                </button>
                <button class="btn" t-on-click="onDiscard">Discard</button>
            </div>
            <t t-if="state.msg">
                <div t-attf-class="alert {{state.msgType === 'error' ? 'alert-danger' : 'alert-success'}}"
                     style="margin:12px 16px 0">
                    <t t-esc="state.msg"/>
                </div>
            </t>
            <!-- Tabs -->
            <div class="form-tabs" style="padding:0 16px;border-bottom:1px solid #e2e8f0;display:flex;gap:4px;margin-top:12px">
                <button t-attf-class="form-tab-btn{{state.tab==='info'?' active':''}}"
                        t-on-click="()=>this.state.tab='info'">General Information</button>
                <button t-attf-class="form-tab-btn{{state.tab==='access'?' active':''}}"
                        t-on-click="()=>this.state.tab='access'">Access Rights</button>
            </div>
            <!-- General tab -->
            <t t-if="state.tab === 'info'">
                <div class="form-body" style="padding:20px;max-width:600px;display:grid;gap:14px">
                    <div class="field-row">
                        <label>Login (Email)</label>
                        <input class="field-input" type="email"
                               t-att-value="state.record.login || ''"
                               t-on-input="e => this.state.record.login = e.target.value"/>
                    </div>
                    <div class="field-row">
                        <label>Password <t t-if="props.recordId">(leave blank to keep current)</t></label>
                        <input class="field-input" type="password" autocomplete="new-password"
                               t-att-value="state.record.password || ''"
                               t-on-input="e => this.state.record.password = e.target.value"/>
                    </div>
                    <div class="field-row">
                        <label>Display Name (Partner)</label>
                        <select class="field-input" t-on-change="e => this.state.record.partner_id = parseInt(e.target.value) || 0">
                            <option value="">— select —</option>
                            <t t-foreach="state.partners" t-as="p" t-key="p.id">
                                <option t-att-value="p.id"
                                        t-att-selected="state.record.partner_id === p.id"
                                        t-esc="p.name"/>
                            </t>
                        </select>
                    </div>
                    <div class="field-row">
                        <label>Company</label>
                        <select class="field-input" t-on-change="e => this.state.record.company_id = parseInt(e.target.value) || 0">
                            <option value="">— select —</option>
                            <t t-foreach="state.companies" t-as="c" t-key="c.id">
                                <option t-att-value="c.id"
                                        t-att-selected="state.record.company_id === c.id"
                                        t-esc="c.name"/>
                            </t>
                        </select>
                    </div>
                    <div class="field-row" style="display:flex;align-items:center;gap:10px">
                        <input type="checkbox" id="uf-active"
                               t-att-checked="state.record.active !== false"
                               t-on-change="e => this.state.record.active = e.target.checked"/>
                        <label for="uf-active" style="margin:0;font-weight:400">Active</label>
                    </div>
                </div>
            </t>
            <!-- Access Rights tab -->
            <t t-if="state.tab === 'access'">
                <div style="padding:20px;max-width:700px">
                    <table class="ar-table">
                        <thead>
                            <tr><th style="width:200px">Module</th><th>Access Level</th></tr>
                        </thead>
                        <tbody>
                            <t t-foreach="groupDefs" t-as="sec" t-key="sec.label">
                                <tr>
                                    <td class="ar-section-label" t-esc="sec.label"/>
                                    <td class="ar-options">
                                        <!-- Checkboxes for Technical section -->
                                        <t t-if="sec.type === 'checkboxes'">
                                            <t t-foreach="sec.groups" t-as="g" t-key="g.id">
                                                <label class="ar-check-label" t-att-title="g.hint || ''">
                                                    <input type="checkbox"
                                                           t-att-checked="state.selectedGroups[g.id] || false"
                                                           t-on-change="e => this.toggleGroup(g.id, e.target.checked)"/>
                                                    <span t-esc="g.label"/>
                                                    <span t-if="g.hint" class="ar-hint" t-esc="'— ' + g.hint"/>
                                                </label>
                                            </t>
                                        </t>
                                        <!-- Radio buttons for module sections -->
                                        <t t-if="sec.type === 'radio'">
                                            <label class="ar-radio-label">
                                                <input type="radio"
                                                       t-att-name="'ar_' + sec.label"
                                                       t-att-checked="!hasAnyInSection(sec)"
                                                       t-on-change="() => this.clearSection(sec)"/>
                                                <span t-esc="sec.noneLabel || 'None'"/>
                                            </label>
                                            <t t-foreach="sec.groups" t-as="g" t-key="g.id">
                                                <label class="ar-radio-label">
                                                    <input type="radio"
                                                           t-att-name="'ar_' + sec.label"
                                                           t-att-checked="state.selectedGroups[g.id] || false"
                                                           t-on-change="() => this.selectRadio(sec, g.id)"/>
                                                    <span t-esc="g.label"/>
                                                </label>
                                            </t>
                                        </t>
                                    </td>
                                </tr>
                            </t>
                        </tbody>
                    </table>
                </div>
            </t>
        </div>
    `;

    get groupDefs() { return GROUP_DEFS; }

    setup() {
        this.state = useState({
            tab:            'info',
            record:         { login:'', password:'', partner_id:0, company_id:0, active:true },
            selectedGroups: {},   // { [groupId]: true/false }
            partners:       [],
            companies:      [],
            saving:         false,
            msg:            '',
            msgType:        'success',
        });
        onMounted(async () => {
            const [partners, companies] = await Promise.all([
                RpcService.call('res.partner', 'search_read', [[]], { fields:['id','name'], limit:200 }),
                RpcService.call('res.company', 'search_read', [[]], { fields:['id','name'], limit:100 }),
            ]);
            this.state.partners  = partners  || [];
            this.state.companies = companies || [];
            if (this.props.recordId) await this.loadRecord();
        });
    }

    async loadRecord() {
        const rows = await RpcService.call('res.users', 'read',
            [[this.props.recordId]], { fields:['id','login','partner_id','company_id','active','groups_id'] });
        if (!rows || !rows.length) return;
        const r = rows[0];
        this.state.record = {
            login:      r.login || '',
            password:   '',
            partner_id: Array.isArray(r.partner_id) ? r.partner_id[0] : (r.partner_id || 0),
            company_id: Array.isArray(r.company_id) ? r.company_id[0] : (r.company_id || 0),
            active:     r.active !== false,
        };
        const sel = {};
        for (const gid of (r.groups_id || [])) sel[gid] = true;
        this.state.selectedGroups = sel;
    }

    toggleGroup(gid, checked) {
        this.state.selectedGroups = { ...this.state.selectedGroups, [gid]: checked };
    }

    hasAnyInSection(sec) {
        return sec.groups.some(g => this.state.selectedGroups[g.id]);
    }

    clearSection(sec) {
        const upd = { ...this.state.selectedGroups };
        sec.groups.forEach(g => { upd[g.id] = false; });
        this.state.selectedGroups = upd;
    }

    selectRadio(sec, gid) {
        const upd = { ...this.state.selectedGroups };
        sec.groups.forEach(g => { upd[g.id] = (g.id === gid); });
        this.state.selectedGroups = upd;
    }

    async onSave() {
        if (!this.state.record.login) {
            this.state.msg = 'Login is required.'; this.state.msgType = 'error'; return;
        }
        this.state.saving = true; this.state.msg = '';
        try {
            // Build groups_id command 6 (replace all)
            const gids = Object.entries(this.state.selectedGroups)
                .filter(([,v]) => v).map(([k]) => parseInt(k));
            const vals = {
                login:      this.state.record.login,
                partner_id: this.state.record.partner_id || false,
                company_id: this.state.record.company_id || false,
                active:     this.state.record.active,
                groups_id:  [[6, 0, gids]],
            };
            if (this.state.record.password)
                vals.password = this.state.record.password;

            if (this.props.recordId) {
                await RpcService.call('res.users', 'write', [[this.props.recordId], vals], {});
            } else {
                if (!this.state.record.password) {
                    this.state.msg = 'Password is required for new users.';
                    this.state.msgType = 'error'; this.state.saving = false; return;
                }
                await RpcService.call('res.users', 'create', [vals], {});
            }
            this.state.msg = 'Saved successfully.'; this.state.msgType = 'success';
            this.state.record.password = '';
        } catch (e) {
            this.state.msg = e.message || 'Save failed.'; this.state.msgType = 'error';
        } finally {
            this.state.saving = false;
        }
    }

    onDiscard() {
        if (this.props.recordId) this.loadRecord();
        else { this.state.record = { login:'', password:'', partner_id:0, company_id:0, active:true }; this.state.selectedGroups = {}; }
        this.state.msg = '';
    }

    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// ----------------------------------------------------------------
// PERMISSION_DEFS — drives the permission checkboxes in the Groups form
// Add a new section here to expose permissions without changing component code
// ----------------------------------------------------------------
const PERMISSION_DEFS = [
    { label: 'Contacts & Partners', perms: [
        { id: 'partner.view',   label: 'View Contacts' },
        { id: 'partner.create', label: 'Create / Edit Contacts' },
    ]},
    { label: 'Products', perms: [
        { id: 'product.view',        label: 'View Products' },
        { id: 'product.create',      label: 'Create / Edit Products' },
        { id: 'product.manage_cats', label: 'Manage Product Categories' },
        { id: 'product.manage_uom',  label: 'Manage Units of Measure' },
    ]},
    { label: 'Accounting', perms: [
        { id: 'account.view_invoices',     label: 'View Customer Invoices' },
        { id: 'account.create_invoices',   label: 'Create Customer Invoices' },
        { id: 'account.validate_invoices', label: 'Validate / Confirm Invoices' },
        { id: 'account.view_bills',        label: 'View Vendor Bills' },
        { id: 'account.create_bills',      label: 'Create Vendor Bills' },
        { id: 'account.manage_accounts',   label: 'Manage Chart of Accounts' },
        { id: 'account.view_journals',     label: 'View Journal Entries' },
        { id: 'account.manage_journals',   label: 'Manage Journals' },
    ]},
    { label: 'Sales', perms: [
        { id: 'sale.view_orders',    label: 'View Sales Orders' },
        { id: 'sale.create_orders',  label: 'Create Sales Orders' },
        { id: 'sale.confirm_orders', label: 'Confirm / Lock Orders' },
        { id: 'sale.override_price', label: 'Override Prices' },
    ]},
    { label: 'Purchasing', perms: [
        { id: 'purchase.view_orders',    label: 'View Purchase Orders' },
        { id: 'purchase.create_orders',  label: 'Create Purchase Orders' },
        { id: 'purchase.approve_orders', label: 'Approve Purchase Orders' },
    ]},
    { label: 'Inventory', perms: [
        { id: 'stock.view_transfers',     label: 'View Transfers' },
        { id: 'stock.create_transfers',   label: 'Create Transfers' },
        { id: 'stock.validate_transfers', label: 'Validate / Complete Transfers' },
        { id: 'stock.manage_locations',   label: 'Manage Locations' },
        { id: 'stock.manage_warehouses',  label: 'Manage Warehouses' },
        { id: 'stock.manage_op_types',    label: 'Manage Operation Types' },
    ]},
    { label: 'Manufacturing', perms: [
        { id: 'mrp.view_bom',        label: 'View Bills of Materials' },
        { id: 'mrp.manage_bom',      label: 'Manage Bills of Materials' },
        { id: 'mrp.view_orders',     label: 'View Manufacturing Orders' },
        { id: 'mrp.create_orders',   label: 'Create Manufacturing Orders' },
        { id: 'mrp.validate_orders', label: 'Validate Manufacturing Orders' },
    ]},
    { label: 'Human Resources', perms: [
        { id: 'hr.view_employees',     label: 'View Employees' },
        { id: 'hr.manage_employees',   label: 'Manage Employees' },
        { id: 'hr.view_departments',   label: 'View Departments' },
        { id: 'hr.manage_departments', label: 'Manage Departments' },
        { id: 'hr.view_schedules',     label: 'View Working Schedules' },
        { id: 'hr.manage_schedules',   label: 'Manage Working Schedules' },
    ]},
    { label: 'Settings & Administration', perms: [
        { id: 'settings.view_users',       label: 'View Users' },
        { id: 'settings.manage_users',     label: 'Manage Users & Passwords' },
        { id: 'settings.manage_groups',    label: 'Manage User Groups' },
        { id: 'settings.manage_companies', label: 'Manage Companies' },
        { id: 'settings.erp_config',       label: 'ERP System Configuration' },
        { id: 'settings.technical',        label: 'Access Technical Menus' },
    ]},
];

// ----------------------------------------------------------------
// GroupsListView — Settings → Technical → Groups (list + form)
// ----------------------------------------------------------------
class GroupsListView extends Component {
    static template = xml`
        <div class="so-shell">
            <t t-if="state.view === 'list'">
                <div class="so-topbar">
                    <span class="so-ref">Groups</span>
                </div>
                <t t-if="state.loading">
                    <div class="loading">Loading...</div>
                </t>
                <t t-else="">
                    <table class="list-table" style="margin:0">
                        <thead>
                            <tr>
                                <th style="width:50px">ID</th>
                                <th>Full Name</th>
                                <th>Name</th>
                                <th style="text-align:center">Portal</th>
                                <th style="text-align:center">Users</th>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.groups" t-as="g" t-key="g.id">
                                <tr style="cursor:pointer" t-on-click="() => this.openGroup(g)">
                                    <td t-esc="g.id"/>
                                    <td t-esc="g.full_name || g.name"/>
                                    <td t-esc="g.name"/>
                                    <td style="text-align:center">
                                        <t t-if="g.share">&#10003;</t>
                                    </td>
                                    <td style="text-align:center" t-esc="g.user_count"/>
                                </tr>
                            </t>
                        </tbody>
                    </table>
                </t>
            </t>
            <t t-else="">
                <div class="so-topbar">
                    <button class="btn" t-on-click="backToList" style="margin-right:12px">&#8592; Groups</button>
                    <span class="so-ref" t-esc="state.form ? (state.form.full_name || state.form.name) : ''"/>
                </div>
                <t t-if="state.formLoading">
                    <div class="loading">Loading...</div>
                </t>
                <t t-elif="state.form">
                    <div style="padding:16px;max-width:900px">
                        <div class="field-row" style="margin-bottom:8px">
                            <label>ID</label>
                            <span style="display:inline-block;padding:6px 10px;background:#f4f4f4;border:1px solid #ddd;border-radius:4px;color:#666;font-size:13px" t-esc="state.form.id"/>
                        </div>
                        <div class="field-row" style="margin-bottom:8px">
                            <label>Full Name</label>
                            <input class="field-input" t-att-value="state.form.full_name" t-on-input="onFullNameInput"/>
                        </div>
                        <div class="field-row" style="margin-bottom:20px">
                            <label>Portal / Shared</label>
                            <input type="checkbox" t-att-checked="state.form.share" t-on-change="onShareChange" style="width:auto;margin-top:4px"/>
                        </div>
                        <h3 style="margin:0 0 12px;border-bottom:1px solid #eee;padding-bottom:8px;font-size:14px;font-weight:600">Permissions</h3>
                        <table class="ar-table" style="width:100%">
                            <t t-foreach="permDefs" t-as="section" t-key="section.label">
                                <tr>
                                    <td class="ar-section-label" t-esc="section.label"/>
                                    <td class="ar-options">
                                        <t t-foreach="section.perms" t-as="perm" t-key="perm.id">
                                            <label class="ar-check-label">
                                                <input type="checkbox"
                                                       t-att-checked="hasPerm(perm.id)"
                                                       t-att-data-perm-id="perm.id"
                                                       t-on-change="onTogglePerm"/>
                                                <span t-esc="perm.label"/>
                                            </label>
                                        </t>
                                    </td>
                                </tr>
                            </t>
                        </table>
                        <div style="margin-top:20px;display:flex;gap:8px">
                            <button class="btn btn-primary" t-on-click="saveGroup" t-att-disabled="state.saving">
                                <t t-if="state.saving">Saving...</t>
                                <t t-else="">Save</t>
                            </button>
                            <button class="btn" t-on-click="backToList">Discard</button>
                        </div>
                    </div>
                </t>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            groups: [], loading: true,
            view: 'list',
            form: null, formLoading: false,
            saving: false,
        });
        onMounted(() => this.loadList());
    }

    get permDefs() { return PERMISSION_DEFS; }

    async loadList() {
        this.state.loading = true;
        try {
            this.state.groups = await RpcService.call('res.groups', 'search_read', [[]], {
                fields: ['id','name','full_name','share','user_count'], limit: 100
            }) || [];
        } finally { this.state.loading = false; }
    }

    async openGroup(g) {
        this.state.view = 'form';
        this.state.formLoading = true;
        this.state.form = null;
        try {
            const data = await RpcService.call('res.groups', 'read', [[g.id]], {
                fields: ['id','name','full_name','share','permissions']
            });
            this.state.form = data && data[0]
                ? { ...data[0], permissions: data[0].permissions || [] }
                : { ...g, permissions: [] };
        } finally { this.state.formLoading = false; }
    }

    backToList() {
        this.state.view = 'list';
        this.state.form = null;
    }

    hasPerm(permId) {
        return (this.state.form?.permissions || []).includes(permId);
    }

    onTogglePerm(ev) {
        const permId = ev.target.dataset.permId;
        const perms = [...(this.state.form.permissions || [])];
        if (ev.target.checked && !perms.includes(permId)) {
            perms.push(permId);
        } else if (!ev.target.checked) {
            const idx = perms.indexOf(permId);
            if (idx >= 0) perms.splice(idx, 1);
        }
        this.state.form.permissions = perms;
    }

    onFullNameInput(ev) { this.state.form.full_name = ev.target.value; }
    onShareChange(ev)   { this.state.form.share = ev.target.checked; }

    async saveGroup() {
        if (!this.state.form) return;
        this.state.saving = true;
        try {
            await RpcService.call('res.groups', 'write', [
                [this.state.form.id],
                {
                    full_name:   this.state.form.full_name,
                    share:       this.state.form.share,
                    permissions: this.state.form.permissions,
                }
            ]);
            await this.loadList();
            this.backToList();
        } finally { this.state.saving = false; }
    }
}

// ----------------------------------------------------------------
// buildCategoryPaths — compute full path strings from flat list
// returns Map<id, "Root > Parent > Child">
// ----------------------------------------------------------------
function buildCategoryPaths(cats) {
    const byId = new Map(cats.map(c => [c.id, c]));
    const cache = new Map();
    function pathFor(id) {
        if (cache.has(id)) return cache.get(id);
        const c = byId.get(id);
        if (!c) return '';
        const parentId = Array.isArray(c.parent_id) ? c.parent_id[0] : 0;
        const p = parentId ? pathFor(parentId) + ' > ' + c.name : c.name;
        cache.set(id, p);
        return p;
    }
    cats.forEach(c => pathFor(c.id));
    return cache;
}

// ----------------------------------------------------------------
// ProductCategoryTree — collapsible left-panel tree component
// ----------------------------------------------------------------
class ProductCategoryTree extends Component {
    static template = xml`
        <div class="cat-tree">
            <div class="cat-tree-header">Categories</div>
            <!-- "All" root node -->
            <div class="cat-tree-item"
                 t-att-class="{'selected': state.selectedId === null}"
                 t-on-click="() => this.select(null)">
                <span class="cat-tree-leaf-gap"/>
                <span class="cat-tree-label">All Products</span>
            </div>
            <t t-foreach="visibleNodes" t-as="node" t-key="node.id">
                <div class="cat-tree-item"
                     t-att-class="{'selected': state.selectedId === node.id}"
                     t-att-style="'padding-left:' + (node.depth * 14 + 12) + 'px'"
                     t-on-click="() => this.select(node.id)">
                    <span class="cat-tree-toggle"
                          t-if="node.hasChildren"
                          t-on-click.stop="() => this.toggle(node.id)">
                        <t t-if="state.expanded.has(node.id)">▾</t>
                        <t t-else="">▸</t>
                    </span>
                    <span class="cat-tree-leaf-gap" t-else=""/>
                    <span class="cat-tree-label" t-esc="node.name"/>
                    <span class="cat-tree-count"
                          t-if="node.productCount > 0"
                          t-esc="'(' + node.productCount + ')'"/>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ expanded: new Set(), selectedId: null });
        onMounted(() => {
            // Auto-expand the first depth level
            const cats = this.props.categories || [];
            cats.forEach(c => {
                const parentId = Array.isArray(c.parent_id) ? c.parent_id[0] : 0;
                if (!parentId) this.state.expanded.add(c.id);
            });
        });
    }

    get visibleNodes() {
        const cats = this.props.categories || [];
        // Build children map
        const children = new Map();
        cats.forEach(c => {
            const pid = Array.isArray(c.parent_id) ? c.parent_id[0] : 0;
            if (!children.has(pid)) children.set(pid, []);
            children.get(pid).push(c);
        });
        // Child counts (all descendants, not just direct)
        const hasChildrenOf = id => (children.get(id) || []).length > 0;

        // Walk tree, only include visible nodes (parent is expanded)
        const result = [];
        const walk = (parentId, depth) => {
            const kids = (children.get(parentId) || [])
                .slice().sort((a, b) => a.name.localeCompare(b.name));
            for (const c of kids) {
                result.push({
                    id:           c.id,
                    name:         c.name,
                    depth,
                    hasChildren:  hasChildrenOf(c.id),
                    productCount: c.product_count || 0,
                });
                if (this.state.expanded.has(c.id)) walk(c.id, depth + 1);
            }
        };
        // Root nodes = those whose parent is not in this set (handles both null and missing)
        const allIds = new Set(cats.map(c => c.id));
        const rootParentId = (() => {
            // Find the common root (typically id=1 "All") — nodes whose parent is not in the list
            const roots = cats.filter(c => {
                const pid = Array.isArray(c.parent_id) ? c.parent_id[0] : 0;
                return !pid || !allIds.has(pid);
            });
            return roots.length > 0 ? 0 : 0;
        })();
        // Collect all parentIds that are not in our cat list → treat as root
        const isRoot = c => {
            const pid = Array.isArray(c.parent_id) ? c.parent_id[0] : 0;
            return !pid || !allIds.has(pid);
        };
        const roots = cats.filter(isRoot).sort((a, b) => a.name.localeCompare(b.name));
        for (const root of roots) {
            result.push({
                id:           root.id,
                name:         root.name,
                depth:        0,
                hasChildren:  hasChildrenOf(root.id),
                productCount: root.product_count || 0,
            });
            if (this.state.expanded.has(root.id)) walk(root.id, 1);
        }
        return result;
    }

    toggle(id) {
        if (this.state.expanded.has(id)) this.state.expanded.delete(id);
        else                              this.state.expanded.add(id);
    }

    select(id) {
        this.state.selectedId = id;
        if (this.props.onSelect) this.props.onSelect(id);
    }
}

// ----------------------------------------------------------------
// ProductCategoryListView — category configurator (tree table)
// ----------------------------------------------------------------
class ProductCategoryListView extends Component {
    static template = xml`
        <div class="list-view-wrap">
            <div class="list-toolbar">
                <span class="list-title">Product Categories</span>
                <button class="btn btn-primary" t-on-click="newCategory">+ New Category</button>
            </div>
            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.editId !== null">
                <!-- Inline form -->
                <div class="form-card" style="max-width:520px;margin:20px auto">
                    <div class="form-card-title" t-esc="state.editId === 0 ? 'New Category' : 'Edit Category'"/>
                    <div class="field-row" style="margin-bottom:12px">
                        <label>Name</label>
                        <input class="field-input" t-att-value="state.form.name"
                               t-on-input="e => state.form.name = e.target.value" placeholder="Category name"/>
                    </div>
                    <div class="field-row" style="margin-bottom:12px">
                        <label>Parent Category</label>
                        <select class="form-input"
                                t-on-change="onParentChange">
                            <option value="0">— (no parent)</option>
                            <t t-foreach="sortedByPath" t-as="cat" t-key="cat.id">
                                <t t-if="cat.id !== state.editId">
                                    <option t-att-value="cat.id"
                                            t-att-selected="state.form.parentId === cat.id ? true : undefined"
                                            t-esc="paths.get(cat.id) || cat.name"/>
                                </t>
                            </t>
                        </select>
                    </div>
                    <div class="field-row" style="margin-bottom:16px">
                        <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
                            <input type="checkbox"
                                   t-att-checked="state.form.active"
                                   t-on-change="e => state.form.active = e.target.checked"/>
                            Active
                        </label>
                    </div>
                    <div style="display:flex;gap:8px">
                        <button class="btn btn-primary" t-on-click="saveCategory">Save</button>
                        <button class="btn" t-on-click="cancelEdit">Cancel</button>
                        <button class="btn" style="margin-left:auto;color:#ef4444"
                                t-if="state.editId !== 0"
                                t-on-click="deleteCategory">Delete</button>
                    </div>
                </div>
            </t>
            <t t-else="">
                <table class="list-table">
                    <thead>
                        <tr>
                            <th>Full Path</th>
                            <th style="width:90px;text-align:center">Children</th>
                            <th style="width:90px;text-align:center">Products</th>
                            <th style="width:60px"/>
                        </tr>
                    </thead>
                    <tbody>
                        <t t-foreach="sortedByPath" t-as="cat" t-key="cat.id">
                            <tr>
                                <td>
                                    <span class="cat-config-path" t-esc="paths.get(cat.id) || cat.name"/>
                                </td>
                                <td style="text-align:center">
                                    <span t-if="cat.child_count > 0"
                                          class="cat-config-badge"
                                          t-esc="cat.child_count"/>
                                </td>
                                <td style="text-align:center">
                                    <span t-if="cat.product_count > 0"
                                          class="cat-config-badge"
                                          t-esc="cat.product_count"/>
                                </td>
                                <td>
                                    <button class="btn" style="padding:2px 10px;font-size:.78rem"
                                            t-on-click="() => this.editCategory(cat)">Edit</button>
                                </td>
                            </tr>
                        </t>
                    </tbody>
                </table>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({
            loading: true,
            categories: [],
            editId: null,      // null=list, 0=new, N=editing id N
            form: { name: '', parentId: 0, active: true },
        });
        onMounted(() => this.load());
    }

    async load() {
        this.state.loading = true;
        try {
            this.state.categories = await RpcService.call(
                'product.category', 'search_read', [[]],
                { fields: ['id','name','parent_id','child_count','product_count'], limit: 500 }
            ) || [];
        } finally { this.state.loading = false; }
    }

    get paths() { return buildCategoryPaths(this.state.categories); }

    get sortedByPath() {
        const p = this.paths;
        return [...this.state.categories].sort((a, b) =>
            (p.get(a.id) || a.name).localeCompare(p.get(b.id) || b.name));
    }

    newCategory() {
        this.state.form   = { name: '', parentId: 0, active: true };
        this.state.editId = 0;
    }

    editCategory(cat) {
        const parentId = Array.isArray(cat.parent_id) ? cat.parent_id[0] : 0;
        this.state.form   = { name: cat.name, parentId, active: cat.active };
        this.state.editId = cat.id;
    }

    cancelEdit() { this.state.editId = null; }

    onParentChange(ev) { this.state.form.parentId = parseInt(ev.target.value) || 0; }

    async saveCategory() {
        const vals = {
            name:      this.state.form.name,
            parent_id: this.state.form.parentId || false,
            active:    this.state.form.active,
        };
        if (this.state.editId === 0) {
            await RpcService.call('product.category', 'create', [vals]);
        } else {
            await RpcService.call('product.category', 'write', [[this.state.editId], vals]);
        }
        this.state.editId = null;
        await this.load();
    }

    async deleteCategory() {
        if (!confirm('Delete this category? Products using it will be unassigned.')) return;
        await RpcService.call('product.category', 'unlink', [[this.state.editId]]);
        this.state.editId = null;
        await this.load();
    }
}

// ----------------------------------------------------------------
// AssetFormView — account.asset detail: confirm → schedule → depreciate (docs/084)
// ----------------------------------------------------------------
class AssetFormView extends Component {
    static components = { DatePicker };
    static template = xml`
        <div class="so-shell" t-on-change="onChange" t-on-input="onInput">
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Assets</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New Asset'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="isNew"><button class="btn btn-primary" t-on-click.stop="onCreate">Create</button></t>
                        <t t-else="">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Back</button>
                    </div>
                </div>
            </div>
            <div class="so-statusbar">
                <div class="so-sb-left">
                    <t t-if="!isNew and state.record.state === 'draft'">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onConfirm">Confirm — build schedule</button>
                    </t>
                    <t t-if="state.record.state === 'open'">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onDepreciate">Post depreciation</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onClose">Set to closed</button>
                    </t>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Draft</div>
                    <div t-attf-class="so-step{{stepClass('open')}}">Running</div>
                    <div t-attf-class="so-step{{stepClass('close')}}">Closed</div>
                </div>
            </div>
            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New Asset'"/>
                        <div class="so-stat-btns">
                            <div class="so-stat-btn so-stat-btn-disabled"><span class="so-stat-num" t-esc="formatMoney(state.record.value_residual)"/><span class="so-stat-lbl">Book Value</span></div>
                        </div>
                    </div>
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Asset Name</label>
                                <input class="form-input" data-field="name" t-att-value="state.record.name || ''"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Asset Type</label>
                                <select class="form-input" data-field="asset_type_id">
                                    <option value="0">—</option>
                                    <t t-foreach="state.types" t-as="o" t-key="o.id"><option t-att-value="o.id" t-att-selected="getM2oId(state.record.asset_type_id) === o.id ? true : undefined" t-esc="o.display"/></t>
                                </select></div>
                            <div class="so-field-row"><label class="so-field-lbl">Gross Value</label>
                                <input class="form-input" type="number" step="0.01" data-field="value" t-att-value="state.record.value !== undefined ? state.record.value : 0" t-att-readonly="isRunning ? true : undefined"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Acquisition Date</label>
                                <DatePicker value="formatDate(state.record.acquisition_date)" onSelect.bind="setAcqDate"/></div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl"># Depreciations</label>
                                <input class="form-input" type="number" data-field="number" t-att-value="state.record.number !== undefined ? state.record.number : 5" t-att-readonly="isRunning ? true : undefined"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Months / period</label>
                                <input class="form-input" type="number" data-field="period_months" t-att-value="state.record.period_months !== undefined ? state.record.period_months : 12" t-att-readonly="isRunning ? true : undefined"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Depreciation Journal</label>
                                <select class="form-input" data-field="journal_id"><option value="0">—</option>
                                    <t t-foreach="state.journals" t-as="o" t-key="o.id"><option t-att-value="o.id" t-att-selected="getM2oId(state.record.journal_id) === o.id ? true : undefined" t-esc="o.display"/></t></select></div>
                            <div class="so-field-row"><label class="so-field-lbl">Expense Account</label>
                                <select class="form-input" data-field="account_expense_id"><option value="0">—</option>
                                    <t t-foreach="state.accounts" t-as="o" t-key="o.id"><option t-att-value="o.id" t-att-selected="getM2oId(state.record.account_expense_id) === o.id ? true : undefined" t-esc="o.display"/></t></select></div>
                            <div class="so-field-row"><label class="so-field-lbl">Depreciation Account</label>
                                <select class="form-input" data-field="account_depreciation_id"><option value="0">—</option>
                                    <t t-foreach="state.accounts" t-as="o" t-key="o.id"><option t-att-value="o.id" t-att-selected="getM2oId(state.record.account_depreciation_id) === o.id ? true : undefined" t-esc="o.display"/></t></select></div>
                        </div>
                    </div>
                    <t t-if="state.lines.length">
                        <div class="so-tabs"><span class="so-tab active">Depreciation Board</span></div>
                        <table class="so-lines-table">
                            <thead><tr><th>#</th><th>Date</th><th class="so-col-num">Depreciation</th><th class="so-col-num">Cumulative</th><th class="so-col-num">Book Value</th><th>Status</th></tr></thead>
                            <tbody>
                                <t t-foreach="state.lines" t-as="l" t-key="l.id">
                                    <tr>
                                        <td t-esc="l.sequence"/>
                                        <td t-esc="formatDate(l.depreciation_date)"/>
                                        <td class="so-col-num" t-esc="formatMoney(l.amount)"/>
                                        <td class="so-col-num" t-esc="formatMoney(l.depreciated_value)"/>
                                        <td class="so-col-num" t-esc="formatMoney(l.remaining_value)"/>
                                        <td><span t-attf-class="asset-badge {{ l.posted ? 'posted' : 'draft' }}" t-esc="l.posted ? 'Posted' : 'Draft'"/></td>
                                    </tr>
                                </t>
                            </tbody>
                        </table>
                    </t>
                </div>
            </t>
        </div>
    `;
    setup() {
        this.state = useState({ loading: true, error: '', record: {}, lines: [],
                                types: [], journals: [], accounts: [],
                                isNew: !this.props.recordId });
        onMounted(() => this.load());
    }
    get isNew()     { return !this.props.recordId; }
    get isRunning() { return this.state.record.state === 'open' || this.state.record.state === 'close'; }
    stepClass(s) {
        const order = { draft: 0, open: 1, close: 2 };
        const cur = order[this.state.record.state] ?? 0, x = order[s];
        return x === cur ? ' active' : (x < cur ? ' done' : '');
    }
    getM2oId(v) {
        if (!v && v !== 0) return 0;
        if (typeof v === 'number') return v;
        if (Array.isArray(v) && v.length) return typeof v[0] === 'number' ? v[0] : 0;
        if (typeof v === 'string') return parseInt(v) || 0;
        return 0;
    }
    formatMoney(v) { return (parseFloat(v) || 0).toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
    formatDate(v)  { return (!v || v === false) ? '' : String(v).substring(0, 10); }
    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const [types, journals, accounts] = await Promise.all([
                RpcService.call('account.asset.type', 'search_read', [[]], { fields: ['id','name'], limit: 200 }).catch(() => []),
                RpcService.call('account.journal', 'search_read', [[]], { fields: ['id','name'], limit: 200 }).catch(() => []),
                RpcService.call('account.account', 'search_read', [[]], { fields: ['id','name','code'], limit: 500 }).catch(() => []),
            ]);
            this.state.types    = (types || []).map(t => ({ id: t.id, display: t.name }));
            this.state.journals = (journals || []).map(j => ({ id: j.id, display: j.name }));
            this.state.accounts = (accounts || []).map(a => ({ id: a.id, display: (a.code ? a.code + ' ' : '') + a.name }));
            if (this.props.recordId) {
                const r = await RpcService.call('account.asset', 'read', [[this.props.recordId]],
                    { fields: ['name','asset_type_id','value','value_residual','acquisition_date',
                               'number','period_months','account_asset_id','account_depreciation_id',
                               'account_expense_id','journal_id','state'] });
                this.state.record = (Array.isArray(r) ? r[0] : r) || {};
                const lines = await RpcService.call('account.asset.depreciation.line', 'search_read',
                    [[['asset_id','=',this.props.recordId]]],
                    { fields: ['id','sequence','depreciation_date','amount','remaining_value','depreciated_value','posted'], limit: 500 });
                this.state.lines = (Array.isArray(lines) ? lines : []).sort((a, b) => a.sequence - b.sequence);
            } else {
                this.state.record = { state: 'draft', number: 5, period_months: 12 };
                this.state.lines = [];
            }
        } catch (e) { this.state.error = e.message || 'Failed to load asset'; }
        this.state.loading = false;
    }
    onChange(e) { const f = e.target.dataset.field; if (f && e.target.tagName === 'SELECT') this.state.record[f] = parseInt(e.target.value) || 0; }
    onInput(e)  { const f = e.target.dataset.field; if (f && e.target.tagName !== 'SELECT') this.state.record[f] = e.target.value; }
    setAcqDate(v) { this.state.record.acquisition_date = v; }
    _vals() {
        const r = this.state.record;
        return {
            name: r.name || '', asset_type_id: this.getM2oId(r.asset_type_id) || false,
            value: parseFloat(r.value) || 0, acquisition_date: r.acquisition_date || false,
            number: parseInt(r.number) || 5, period_months: parseInt(r.period_months) || 12,
            journal_id: this.getM2oId(r.journal_id) || false,
            account_expense_id: this.getM2oId(r.account_expense_id) || false,
            account_depreciation_id: this.getM2oId(r.account_depreciation_id) || false,
        };
    }
    async onCreate() { try { await RpcService.call('account.asset', 'create', [this._vals()], {}); this.props.onBack(); } catch (e) { this.state.error = e.message; } }
    async onSave()   { try { await RpcService.call('account.asset', 'write', [[this.props.recordId], this._vals()], {}); await this.load(); } catch (e) { alert('Save failed: ' + (e.message || e)); } }
    async onDelete() { try { await RpcService.call('account.asset', 'unlink', [[this.props.recordId]], {}); this.props.onBack(); } catch (e) { alert(e.message || e); } }
    async onConfirm() { try { await this.onSaveQuiet(); await RpcService.call('account.asset', 'action_confirm', [[this.props.recordId]], {}); await this.load(); } catch (e) { alert('Confirm failed: ' + (e.message || e)); } }
    async onSaveQuiet() { if (!this.isNew) await RpcService.call('account.asset', 'write', [[this.props.recordId], this._vals()], {}); }
    async onDepreciate() { try { const n = await RpcService.call('account.asset', 'action_depreciate', [[this.props.recordId]], {}); await this.load(); alert(n ? (n + ' depreciation entr' + (n === 1 ? 'y' : 'ies') + ' posted.') : 'Nothing due yet.'); } catch (e) { alert('Depreciation failed: ' + (e.message || e)); } }
    async onClose() { try { await RpcService.call('account.asset', 'action_close', [[this.props.recordId]], {}); await this.load(); } catch (e) { alert(e.message || e); } }
    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// BudgetFormView — account.budget: planned vs actual per position (docs/085)
// ----------------------------------------------------------------
class BudgetFormView extends Component {
    static components = { DatePicker };
    static template = xml`
        <div class="so-shell" t-on-change="onChange" t-on-input="onInput" t-on-click="onClick">
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Budgets</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New Budget'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="isNew"><button class="btn btn-primary" t-on-click.stop="onCreate">Create</button></t>
                        <t t-else="">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Back</button>
                    </div>
                </div>
            </div>
            <div class="so-statusbar">
                <div class="so-sb-left">
                    <t t-if="!isNew">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onCompute">Refresh actuals</button>
                        <t t-if="state.record.state === 'draft'">
                            <button class="btn so-wf-btn" t-on-click.stop="onConfirm">Confirm</button>
                        </t>
                        <t t-if="state.record.state === 'confirm'">
                            <button class="btn so-wf-btn" t-on-click.stop="onDone">Mark done</button>
                        </t>
                        <t t-if="state.record.state === 'done'">
                            <button class="btn ghost so-wf-btn" t-on-click.stop="onDraft">Reset to draft</button>
                        </t>
                    </t>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Draft</div>
                    <div t-attf-class="so-step{{stepClass('confirm')}}">Confirmed</div>
                    <div t-attf-class="so-step{{stepClass('done')}}">Done</div>
                </div>
            </div>
            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New Budget'"/>
                        <div class="so-stat-btns">
                            <div class="so-stat-btn so-stat-btn-disabled"><span class="so-stat-num" t-esc="formatMoney(totalPlanned)"/><span class="so-stat-lbl">Planned</span></div>
                            <div class="so-stat-btn so-stat-btn-disabled"><span class="so-stat-num" t-esc="formatMoney(totalActual)"/><span class="so-stat-lbl">Actual</span></div>
                        </div>
                    </div>
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Budget Name</label>
                                <input class="form-input" data-field="name" t-att-value="state.record.name || ''"/></div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Period From</label>
                                <DatePicker value="formatDate(state.record.date_from)" onSelect.bind="setFrom"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Period To</label>
                                <DatePicker value="formatDate(state.record.date_to)" onSelect.bind="setTo"/></div>
                        </div>
                    </div>

                    <div class="so-tabs"><span class="so-tab active">Budget Lines</span></div>
                    <table class="so-lines-table">
                        <thead><tr>
                            <th>Budgetary Position</th>
                            <th class="so-col-num">Planned</th>
                            <th class="so-col-num">Actual</th>
                            <th class="so-col-num">Remaining</th>
                            <th style="width:180px">Achieved</th>
                            <th class="so-col-del"/>
                        </tr></thead>
                        <tbody>
                            <t t-foreach="state.lines" t-as="l" t-key="l._key">
                                <tr>
                                    <td>
                                        <select class="o2m-input" data-line-field="post_id" t-att-data-key="l._key">
                                            <option value="0">—</option>
                                            <t t-foreach="state.posts" t-as="o" t-key="o.id">
                                                <option t-att-value="o.id" t-att-selected="getM2oId(l.post_id) === o.id ? true : undefined" t-esc="o.display"/>
                                            </t>
                                        </select>
                                    </td>
                                    <td class="so-col-num">
                                        <input class="o2m-input" type="number" step="0.01" data-line-field="planned_amount"
                                               t-att-data-key="l._key" t-att-value="l.planned_amount !== undefined ? l.planned_amount : 0"/>
                                    </td>
                                    <td class="so-col-num" t-esc="formatMoney(l.practical_amount)"/>
                                    <td class="so-col-num" t-esc="formatMoney(remainingOf(l))"/>
                                    <td>
                                        <div class="bg-meter"><div t-attf-class="bg-fill {{ pctOf(l) > 100 ? 'over' : '' }}" t-attf-style="width:{{ pctWidth(l) }}%"/></div>
                                        <span class="bg-pct" t-esc="pctOf(l) + '%'"/>
                                    </td>
                                    <td class="so-col-del">
                                        <button class="btn btn-sm btn-danger" data-del-line="1" t-att-data-key="l._key">✕</button>
                                    </td>
                                </tr>
                            </t>
                            <t t-if="!state.lines.length">
                                <tr><td colspan="6" class="trn-empty-row">No budget lines yet — add one below.</td></tr>
                            </t>
                        </tbody>
                    </table>
                    <div class="so-line-adds"><button class="btn so-add-line" data-add-line="1">+ Add a line</button></div>
                </div>
            </t>
        </div>
    `;
    setup() {
        this.state = useState({ loading: true, error: '', record: {}, lines: [], posts: [], deleted: [] });
        this._key = 1;
        onMounted(() => this.load());
    }
    get isNew() { return !this.props.recordId; }
    get totalPlanned() { return this.state.lines.reduce((s, l) => s + (parseFloat(l.planned_amount) || 0), 0); }
    get totalActual()  { return this.state.lines.reduce((s, l) => s + (parseFloat(l.practical_amount) || 0), 0); }
    stepClass(s) {
        const order = { draft: 0, confirm: 1, done: 2 };
        const cur = order[this.state.record.state] ?? 0, x = order[s];
        return x === cur ? ' active' : (x < cur ? ' done' : '');
    }
    pctOf(l) {
        const p = parseFloat(l.planned_amount) || 0, a = parseFloat(l.practical_amount) || 0;
        return p ? Math.round(a / p * 100) : 0;
    }
    remainingOf(l) { return (parseFloat(l.planned_amount) || 0) - (parseFloat(l.practical_amount) || 0); }
    pctWidth(l) { return Math.max(0, Math.min(100, this.pctOf(l))); }
    getM2oId(v) {
        if (!v && v !== 0) return 0;
        if (typeof v === 'number') return v;
        if (Array.isArray(v) && v.length) return typeof v[0] === 'number' ? v[0] : 0;
        if (typeof v === 'string') return parseInt(v) || 0;
        return 0;
    }
    formatMoney(v) { return (parseFloat(v) || 0).toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
    formatDate(v)  { return (!v || v === false) ? '' : String(v).substring(0, 10); }
    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const posts = await RpcService.call('account.budget.post', 'search_read', [[]],
                { fields: ['id', 'name'], limit: 200 }).catch(() => []);
            this.state.posts = (posts || []).map(p => ({ id: p.id, display: p.name }));
            if (this.props.recordId) {
                const r = await RpcService.call('account.budget', 'read', [[this.props.recordId]],
                    { fields: ['name', 'date_from', 'date_to', 'state'] });
                this.state.record = (Array.isArray(r) ? r[0] : r) || {};
                const lines = await RpcService.call('account.budget.line', 'search_read',
                    [[['budget_id', '=', this.props.recordId]]],
                    { fields: ['id', 'post_id', 'planned_amount', 'practical_amount'], limit: 500 });
                this.state.lines = (Array.isArray(lines) ? lines : []).map(l => ({ _key: String(this._key++), ...l }));
            } else {
                this.state.record = { state: 'draft' };
                this.state.lines = [];
            }
        } catch (e) { this.state.error = e.message || 'Failed to load budget'; }
        this.state.loading = false;
    }
    onChange(e) {
        const lf = e.target.dataset.lineField;
        if (lf) { const l = this.state.lines.find(x => x._key === e.target.dataset.key); if (l) l[lf] = parseInt(e.target.value) || 0; return; }
        const f = e.target.dataset.field; if (f && e.target.tagName === 'SELECT') this.state.record[f] = parseInt(e.target.value) || 0;
    }
    onInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const lf = e.target.dataset.lineField;
        if (lf) { const l = this.state.lines.find(x => x._key === e.target.dataset.key); if (l) l[lf] = e.target.value; return; }
        const f = e.target.dataset.field; if (f) this.state.record[f] = e.target.value;
    }
    onClick(e) {
        if (e.target.dataset.addLine) {
            e.preventDefault();
            this.state.lines.push({ _key: String(this._key++), id: null, post_id: 0, planned_amount: 0, practical_amount: 0 });
            return;
        }
        if (e.target.dataset.delLine) {
            e.preventDefault();
            const k = e.target.dataset.key;
            const l = this.state.lines.find(x => x._key === k);
            if (l && l.id) this.state.deleted.push(l.id);
            this.state.lines = this.state.lines.filter(x => x._key !== k);
        }
    }
    setFrom(v) { this.state.record.date_from = v; }
    setTo(v)   { this.state.record.date_to   = v; }
    _vals() {
        const r = this.state.record;
        return { name: r.name || '', date_from: r.date_from || false, date_to: r.date_to || false };
    }
    async syncLines(budgetId) {
        if (this.state.deleted.length) {
            await RpcService.call('account.budget.line', 'unlink', [this.state.deleted], {});
            this.state.deleted = [];
        }
        for (const l of this.state.lines) {
            const vals = { budget_id: budgetId, post_id: this.getM2oId(l.post_id) || false,
                           planned_amount: parseFloat(l.planned_amount) || 0 };
            if (!l.id) await RpcService.call('account.budget.line', 'create', [vals], {});
            else       await RpcService.call('account.budget.line', 'write', [[l.id], vals], {});
        }
    }
    async onCreate() {
        try {
            const id = await RpcService.call('account.budget', 'create', [this._vals()], {});
            await this.syncLines(id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }
    async onSave() {
        try {
            await RpcService.call('account.budget', 'write', [[this.props.recordId], this._vals()], {});
            await this.syncLines(this.props.recordId);
            await this.onCompute();
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }
    async onDelete() { try { await RpcService.call('account.budget', 'unlink', [[this.props.recordId]], {}); this.props.onBack(); } catch (e) { alert(e.message || e); } }
    async onCompute() { try { await RpcService.call('account.budget', 'action_compute', [[this.props.recordId]], {}); await this.load(); } catch (e) { alert('Could not refresh actuals: ' + (e.message || e)); } }
    async onConfirm() { await this._state('action_confirm'); }
    async onDone()    { await this._state('action_done'); }
    async onDraft()   { await this._state('action_draft'); }
    async _state(m)   { try { await RpcService.call('account.budget', m, [[this.props.recordId]], {}); await this.load(); } catch (e) { alert(e.message || e); } }
    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// BankAccountFormView — account.bank.account: the debit/credit register (docs/087)
// Index · Description · Date · Debit · Credit, with a running balance.
// ----------------------------------------------------------------
class BankAccountFormView extends Component {
    static template = xml`
        <div class="so-shell" t-on-change="onChange" t-on-input="onInput" t-on-click="onClick">
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Bank Accounts</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New Bank Account'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="isNew"><button class="btn btn-primary" t-on-click.stop="onCreate">Create</button></t>
                        <t t-else="">
                            <button class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Back</button>
                    </div>
                </div>
            </div>
            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New Bank Account'"/>
                        <div class="so-stat-btns">
                            <div class="so-stat-btn so-stat-btn-disabled">
                                <span class="so-stat-num" t-esc="formatMoney(balance)"/>
                                <span class="so-stat-lbl">Balance</span>
                            </div>
                        </div>
                    </div>
                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Account Name</label>
                                <input class="form-input" data-field="name" t-att-value="state.record.name || ''"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Bank</label>
                                <input class="form-input" data-field="bank_name" t-att-value="state.record.bank_name || ''"/></div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Account Number</label>
                                <input class="form-input" data-field="account_number" t-att-value="state.record.account_number || ''"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Journal</label>
                                <select class="form-input" data-field="journal_id"><option value="0">—</option>
                                    <t t-foreach="state.journals" t-as="o" t-key="o.id">
                                        <option t-att-value="o.id" t-att-selected="getM2oId(state.record.journal_id) === o.id ? true : undefined" t-esc="o.display"/>
                                    </t></select></div>
                        </div>
                    </div>

                    <div class="so-tabs"><span class="so-tab active">Entries</span></div>
                    <table class="so-lines-table">
                        <thead><tr>
                            <th style="width:60px">#</th>
                            <th>Description</th>
                            <th style="width:150px">Date</th>
                            <th class="so-col-num">Debit</th>
                            <th class="so-col-num">Credit</th>
                            <th class="so-col-num">Balance</th>
                            <th class="so-col-del"/>
                        </tr></thead>
                        <tbody>
                            <t t-foreach="rowsWithBalance" t-as="l" t-key="l._key">
                                <tr>
                                    <td t-esc="l._idx"/>
                                    <td><input class="o2m-input" data-line-field="name" t-att-data-key="l._key"
                                               t-att-value="l.name || ''" placeholder="Description"/></td>
                                    <td><input class="o2m-input" type="date" data-line-field="date" t-att-data-key="l._key"
                                               t-att-value="formatDate(l.date)"/></td>
                                    <td class="so-col-num"><input class="o2m-input" type="number" step="0.01"
                                               data-line-field="debit" t-att-data-key="l._key"
                                               t-att-value="l.debit !== undefined ? l.debit : 0"/></td>
                                    <td class="so-col-num"><input class="o2m-input" type="number" step="0.01"
                                               data-line-field="credit" t-att-data-key="l._key"
                                               t-att-value="l.credit !== undefined ? l.credit : 0"/></td>
                                    <td class="so-col-num" t-esc="formatMoney(l._balance)"/>
                                    <td class="so-col-del">
                                        <button class="btn btn-sm btn-danger" data-del-line="1" t-att-data-key="l._key">✕</button>
                                    </td>
                                </tr>
                            </t>
                            <t t-if="!state.lines.length">
                                <tr><td colspan="7" class="trn-empty-row">No entries yet — add the first line below.</td></tr>
                            </t>
                        </tbody>
                        <tfoot>
                            <tr class="bank-total-row">
                                <td colspan="3">Totals</td>
                                <td class="so-col-num" t-esc="formatMoney(totalDebit)"/>
                                <td class="so-col-num" t-esc="formatMoney(totalCredit)"/>
                                <td class="so-col-num" t-esc="formatMoney(balance)"/>
                                <td/>
                            </tr>
                        </tfoot>
                    </table>
                    <div class="so-line-adds"><button class="btn so-add-line" data-add-line="1">+ Add a line</button></div>
                </div>
            </t>
        </div>
    `;
    setup() {
        this.state = useState({ loading: true, error: '', record: {}, lines: [], journals: [], deleted: [] });
        this._key = 1;
        onMounted(() => this.load());
    }
    get isNew() { return !this.props.recordId; }
    get totalDebit()  { return this.state.lines.reduce((s, l) => s + (parseFloat(l.debit)  || 0), 0); }
    get totalCredit() { return this.state.lines.reduce((s, l) => s + (parseFloat(l.credit) || 0), 0); }
    get balance()     { return this.totalDebit - this.totalCredit; }
    // Index + running balance, computed for display (never stored, so it can't drift).
    get rowsWithBalance() {
        let run = 0;
        return this.state.lines.map((l, i) => {
            run += (parseFloat(l.debit) || 0) - (parseFloat(l.credit) || 0);
            return Object.assign({}, l, { _idx: i + 1, _balance: run });
        });
    }
    getM2oId(v) {
        if (!v && v !== 0) return 0;
        if (typeof v === 'number') return v;
        if (Array.isArray(v) && v.length) return typeof v[0] === 'number' ? v[0] : 0;
        if (typeof v === 'string') return parseInt(v) || 0;
        return 0;
    }
    formatMoney(v) { return (parseFloat(v) || 0).toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
    formatDate(v)  { return (!v || v === false) ? '' : String(v).substring(0, 10); }
    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const js = await RpcService.call('account.journal', 'search_read', [[]],
                { fields: ['id', 'name'], limit: 200 }).catch(() => []);
            this.state.journals = (js || []).map(j => ({ id: j.id, display: j.name }));
            if (this.props.recordId) {
                const r = await RpcService.call('account.bank.account', 'read', [[this.props.recordId]],
                    { fields: ['name', 'bank_name', 'account_number', 'journal_id', 'active'] });
                this.state.record = (Array.isArray(r) ? r[0] : r) || {};
                const lines = await RpcService.call('account.bank.account.line', 'search_read',
                    [[['bank_account_id', '=', this.props.recordId]]],
                    { fields: ['id', 'sequence', 'date', 'name', 'debit', 'credit'], limit: 1000 });
                this.state.lines = (Array.isArray(lines) ? lines : [])
                    .sort((a, b) => (a.sequence - b.sequence) || (a.id - b.id))
                    .map(l => ({ _key: String(this._key++), ...l }));
            } else {
                this.state.record = { active: true };
                this.state.lines = [];
            }
        } catch (e) { this.state.error = e.message || 'Failed to load bank account'; }
        this.state.loading = false;
    }
    onChange(e) {
        const lf = e.target.dataset.lineField;
        if (lf) { const l = this.state.lines.find(x => x._key === e.target.dataset.key); if (l) l[lf] = e.target.value; return; }
        const f = e.target.dataset.field; if (f && e.target.tagName === 'SELECT') this.state.record[f] = parseInt(e.target.value) || 0;
    }
    onInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const lf = e.target.dataset.lineField;
        if (lf) { const l = this.state.lines.find(x => x._key === e.target.dataset.key); if (l) l[lf] = e.target.value; return; }
        const f = e.target.dataset.field; if (f) this.state.record[f] = e.target.value;
    }
    onClick(e) {
        if (e.target.dataset.addLine) {
            e.preventDefault();
            const today = new Date().toISOString().slice(0, 10);
            this.state.lines.push({ _key: String(this._key++), id: null,
                sequence: this.state.lines.length + 1, date: today, name: '', debit: 0, credit: 0 });
            return;
        }
        if (e.target.dataset.delLine) {
            e.preventDefault();
            const k = e.target.dataset.key;
            const l = this.state.lines.find(x => x._key === k);
            if (l && l.id) this.state.deleted.push(l.id);
            this.state.lines = this.state.lines.filter(x => x._key !== k);
        }
    }
    _vals() {
        const r = this.state.record;
        return { name: r.name || '', bank_name: r.bank_name || '', account_number: r.account_number || '',
                 journal_id: this.getM2oId(r.journal_id) || false };
    }
    async syncLines(id) {
        if (this.state.deleted.length) {
            await RpcService.call('account.bank.account.line', 'unlink', [this.state.deleted], {});
            this.state.deleted = [];
        }
        let seq = 0;
        for (const l of this.state.lines) {
            seq += 1;
            const vals = { bank_account_id: id, sequence: seq, date: l.date || false,
                           name: l.name || '', debit: parseFloat(l.debit) || 0, credit: parseFloat(l.credit) || 0 };
            if (!l.id) await RpcService.call('account.bank.account.line', 'create', [vals], {});
            else       await RpcService.call('account.bank.account.line', 'write', [[l.id], vals], {});
        }
    }
    async onCreate() {
        try {
            const id = await RpcService.call('account.bank.account', 'create', [this._vals()], {});
            await this.syncLines(id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }
    async onSave() {
        try {
            await RpcService.call('account.bank.account', 'write', [[this.props.recordId], this._vals()], {});
            await this.syncLines(this.props.recordId);
            await this.load();
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }
    async onDelete() { try { await RpcService.call('account.bank.account', 'unlink', [[this.props.recordId]], {}); this.props.onBack(); } catch (e) { alert(e.message || e); } }
    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// AccountDashboard — adjustable journal cards (docs/087)
// ----------------------------------------------------------------
class AccountDashboard extends Component {
    static template = xml`
        <div class="dash-screen">
            <div class="dash-head">
                <div>
                    <h2>Accounting Dashboard</h2>
                    <p class="dash-sub">A quick read on what is owed, owing and on hand.</p>
                </div>
                <button class="btn" t-on-click="toggleCustomize" t-esc="state.customizing ? 'Done' : 'Customize'"/>
            </div>

            <t t-if="state.customizing">
                <div class="dash-customize">
                    <div class="dash-customize-title">Show these cards</div>
                    <t t-foreach="state.cards" t-as="c" t-key="c.id">
                        <label class="dash-toggle">
                            <input type="checkbox" t-att-checked="isOn(c.id)" t-on-change="() => this.toggle(c.id)"/>
                            <span t-esc="c.title"/>
                        </label>
                    </t>
                </div>
            </t>

            <t t-if="state.loading"><div class="dash-msg">Loading…</div></t>
            <t t-elif="state.error"><div class="dash-msg dash-err" t-esc="state.error"/></t>
            <t t-else="">
                <div class="dash-grid">
                    <t t-foreach="visibleCards" t-as="c" t-key="c.id">
                        <div class="dash-card">
                            <div class="dash-card-title" t-esc="c.title"/>
                            <div class="dash-card-amount" t-esc="c.amount"/>
                            <div class="dash-card-sub"><t t-esc="c.subtitle"/> · <t t-esc="c.count"/></div>
                        </div>
                    </t>
                    <t t-if="!visibleCards.length">
                        <div class="dash-msg">No cards selected — press <b>Customize</b> to choose some.</div>
                    </t>
                </div>
            </t>
        </div>
    `;
    setup() {
        this.state = useState({ loading: true, error: '', cards: [], enabled: [], customizing: false });
        onMounted(() => this.load());
    }
    get visibleCards() { return this.state.cards.filter(c => this.state.enabled.includes(c.id)); }
    isOn(id) { return this.state.enabled.includes(id); }
    toggleCustomize() { this.state.customizing = !this.state.customizing; }
    async toggle(id) {
        this.state.enabled = this.isOn(id)
            ? this.state.enabled.filter(x => x !== id)
            : [...this.state.enabled, id];
        await this.save();
    }
    async save() {
        try {
            await fetch('/web/account/dashboard?cards=' + encodeURIComponent(this.state.enabled.join(',')),
                        { credentials: 'same-origin' });
        } catch (_) {}
    }
    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const res = await fetch('/web/account/dashboard', { credentials: 'same-origin' });
            if (!res.ok) throw new Error('Could not load the dashboard (' + res.status + ').');
            const data = await res.json();
            this.state.cards   = data.cards || [];
            this.state.enabled = (data.enabled || '').split(',').map(s => s.trim()).filter(Boolean);
        } catch (e) { this.state.error = (e && e.message) || 'Failed to load dashboard.'; }
        this.state.loading = false;
    }
}

// ----------------------------------------------------------------
// AccountSettings — the Accounting settings screen (docs/088)
// Surfaces the new switches (fiscal year, lock dates, tax periodicity,
// defaults) and links to the config lists that already have their own menus.
// ----------------------------------------------------------------
class AccountSettings extends Component {
    static template = xml`
        <div class="set-screen">
            <div class="set-head">
                <div>
                    <h2>Accounting Settings</h2>
                    <p class="set-sub">Changes save as you edit.</p>
                </div>
                <t t-if="state.saved"><span class="set-saved">Saved</span></t>
            </div>
            <t t-if="state.loading"><div class="dash-msg">Loading…</div></t>
            <t t-elif="state.error"><div class="dash-msg dash-err" t-esc="state.error"/></t>
            <t t-else="">
                <div class="set-section">
                    <h3>Fiscal year</h3>
                    <div class="set-row">
                        <label class="set-lbl">Last day</label>
                        <input class="set-in" type="number" min="1" max="31"
                               t-att-value="val('account.fiscal_year_last_day') || 31"
                               t-on-change="(e) => this.save('account.fiscal_year_last_day', e.target.value)"/>
                        <label class="set-lbl">Last month</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.fiscal_year_last_month', e.target.value)">
                            <t t-foreach="months" t-as="m" t-key="m.v">
                                <option t-att-value="m.v" t-att-selected="(val('account.fiscal_year_last_month') || '12') === m.v ? true : undefined" t-esc="m.n"/>
                            </t>
                        </select>
                    </div>
                </div>

                <div class="set-section set-lock">
                    <h3>Lock dates</h3>
                    <p class="set-hint">Entries dated on or before a lock date can no longer be posted — this is what protects a closed period.</p>
                    <div class="set-row">
                        <label class="set-lbl">All users lock date</label>
                        <input class="set-in" type="date" t-att-value="val('account.lock_date')"
                               t-on-change="(e) => this.save('account.lock_date', e.target.value)"/>
                        <label class="set-lbl">Tax return lock date</label>
                        <input class="set-in" type="date" t-att-value="val('account.tax_lock_date')"
                               t-on-change="(e) => this.save('account.tax_lock_date', e.target.value)"/>
                    </div>
                </div>

                <div class="set-section">
                    <h3>Taxes</h3>
                    <div class="set-row">
                        <label class="set-lbl">Default sales tax</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.default_sale_tax_id', e.target.value)">
                            <option value="">—</option>
                            <t t-foreach="salesTaxes" t-as="t" t-key="t.id">
                                <option t-att-value="sid(t.id)" t-att-selected="val('account.default_sale_tax_id') === sid(t.id) ? true : undefined" t-esc="t.name"/>
                            </t>
                        </select>
                        <label class="set-lbl">Default purchase tax</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.default_purchase_tax_id', e.target.value)">
                            <option value="">—</option>
                            <t t-foreach="purchaseTaxes" t-as="t" t-key="t.id">
                                <option t-att-value="sid(t.id)" t-att-selected="val('account.default_purchase_tax_id') === sid(t.id) ? true : undefined" t-esc="t.name"/>
                            </t>
                        </select>
                    </div>
                    <div class="set-row">
                        <label class="set-lbl">Tax return periodicity</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.tax_periodicity', e.target.value)">
                            <t t-foreach="periodicities" t-as="p" t-key="p.v">
                                <option t-att-value="p.v" t-att-selected="(val('account.tax_periodicity') || 'bimonthly') === p.v ? true : undefined" t-esc="p.n"/>
                            </t>
                        </select>
                        <label class="set-lbl">Tax rounding</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.tax_rounding', e.target.value)">
                            <option value="per_line"  t-att-selected="(val('account.tax_rounding') || 'per_line') === 'per_line' ? true : undefined">Round per line</option>
                            <option value="per_tax"   t-att-selected="val('account.tax_rounding') === 'per_tax' ? true : undefined">Round globally per tax</option>
                        </select>
                    </div>
                </div>

                <div class="set-section">
                    <h3>Default journals</h3>
                    <div class="set-row">
                        <label class="set-lbl">Sales journal</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.default_sale_journal_id', e.target.value)">
                            <option value="">—</option>
                            <t t-foreach="saleJournals" t-as="j" t-key="j.id">
                                <option t-att-value="sid(j.id)" t-att-selected="val('account.default_sale_journal_id') === sid(j.id) ? true : undefined" t-esc="j.name"/>
                            </t>
                        </select>
                        <label class="set-lbl">Purchase journal</label>
                        <select class="set-in" t-on-change="(e) => this.save('account.default_purchase_journal_id', e.target.value)">
                            <option value="">—</option>
                            <t t-foreach="purchaseJournals" t-as="j" t-key="j.id">
                                <option t-att-value="sid(j.id)" t-att-selected="val('account.default_purchase_journal_id') === sid(j.id) ? true : undefined" t-esc="j.name"/>
                            </t>
                        </select>
                    </div>
                    <div class="set-row">
                        <label class="set-check">
                            <input type="checkbox" t-att-checked="val('account.multi_currency') === '1'"
                                   t-on-change="(e) => this.save('account.multi_currency', e.target.checked ? '1' : '0')"/>
                            <span>Enable multi-currency</span>
                        </label>
                    </div>
                </div>

                <div class="set-section">
                    <h3>Configured elsewhere</h3>
                    <p class="set-hint">These already have their own screens — this is just where to find them.</p>
                    <div class="set-links">
                        <t t-foreach="links" t-as="l" t-key="l">
                            <span class="set-link" t-esc="l"/>
                        </t>
                    </div>
                </div>
            </t>
        </div>
    `;
    setup() {
        this.state = useState({ loading: true, error: '', values: {}, taxes: [], journals: [], saved: false });
        this.months = [['1','January'],['2','February'],['3','March'],['4','April'],['5','May'],['6','June'],
                       ['7','July'],['8','August'],['9','September'],['10','October'],['11','November'],['12','December']]
                      .map(([v, n]) => ({ v, n }));
        this.periodicities = [{ v: 'monthly', n: 'Monthly' }, { v: 'bimonthly', n: 'Bi-monthly (SST-02)' },
                              { v: 'quarterly', n: 'Quarterly' }, { v: 'yearly', n: 'Yearly' }];
        this.links = ['Chart of Accounts', 'Taxes', 'Journals', 'Payment Terms', 'Fiscal Positions',
                      'Account Types', 'Currencies', 'Incoterms', 'Journal Groups', 'Bank Accounts',
                      'Budgetary Positions', 'Asset Types', 'Analytic Accounts', 'Document Templates'];
        onMounted(() => this.load());
    }
    val(k) { return this.state.values[k] || ''; }
    // String() is not available inside OWL templates - use this instead.
    sid(v) { return v == null ? '' : ('' + v); }
    get salesTaxes()       { return this.state.taxes.filter(t => t.scope === 'sale'); }
    get purchaseTaxes()    { return this.state.taxes.filter(t => t.scope === 'purchase'); }
    get saleJournals()     { return this.state.journals.filter(j => j.type === 'sale'); }
    get purchaseJournals() { return this.state.journals.filter(j => j.type === 'purchase'); }
    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const res = await fetch('/web/account/settings', { credentials: 'same-origin' });
            if (!res.ok) throw new Error('Could not load settings (' + res.status + ').');
            const d = await res.json();
            this.state.values = d.values || {};
            this.state.taxes = d.taxes || [];
            this.state.journals = d.journals || [];
        } catch (e) { this.state.error = (e && e.message) || 'Failed to load settings.'; }
        this.state.loading = false;
    }
    async save(key, value) {
        try {
            const res = await fetch('/web/account/settings?key=' + encodeURIComponent(key) +
                                    '&value=' + encodeURIComponent(value == null ? '' : value),
                                    { credentials: 'same-origin' });
            if (!res.ok) throw new Error('Save failed (' + res.status + ').');
            const d = await res.json();
            this.state.values = d.values || this.state.values;
            this.state.saved = true;
            setTimeout(() => { this.state.saved = false; }, 1500);
        } catch (e) { this.state.error = (e && e.message) || 'Could not save that setting.'; }
    }
}

// ----------------------------------------------------------------
// ExpenseSheetFormView — hr.expense.sheet: the claim, its receipts, and the
// approval chain that turns them into a journal entry (docs/090).
//
// Draft → Submitted → Approved → Posted → Paid. The expense lines are only
// editable while the report is a draft: once it has been submitted, the totals
// are what somebody is approving, and after posting they are in the ledger.
// ----------------------------------------------------------------
class ExpenseSheetFormView extends Component {
    static components = { DatePicker, AttachmentPanel };
    static template = xml`
        <div class="so-shell" t-on-change="onChange" t-on-input="onInput">
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack">Expense Reports</span>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'New Expense Report'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="isNew"><button class="btn btn-primary" t-on-click.stop="onCreate">Create</button></t>
                        <t t-else="">
                            <button t-if="isDraft" class="btn btn-primary" t-on-click.stop="onSave">Save</button>
                            <button t-if="isDraft" class="btn btn-danger"  t-on-click.stop="onDelete">Delete</button>
                        </t>
                        <button class="btn" t-on-click.stop="onBack">Back</button>
                    </div>
                </div>
            </div>

            <div class="so-statusbar">
                <div class="so-sb-left">
                    <button t-if="!isNew and state.record.state === 'draft'" class="btn btn-primary so-wf-btn"
                            t-on-click.stop="onSubmit">Submit to Manager</button>
                    <t t-if="state.record.state === 'submit'">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onApprove">Approve</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onRefuse">Refuse</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onDraft">Reset to Draft</button>
                    </t>
                    <t t-if="state.record.state === 'approve'">
                        <button class="btn btn-primary so-wf-btn" t-on-click.stop="onPost">Post Journal Entry</button>
                        <button class="btn ghost so-wf-btn"       t-on-click.stop="onRefuse">Refuse</button>
                    </t>
                    <button t-if="state.record.state === 'post' and !isCompanyPaid" class="btn btn-primary so-wf-btn"
                            t-on-click.stop="onPay">Register Reimbursement</button>
                    <span t-if="state.record.state === 'post' and isCompanyPaid" class="exp-note">
                        Paid by the company — nothing to reimburse.
                    </span>
                    <button t-if="state.record.state === 'cancel'" class="btn ghost so-wf-btn"
                            t-on-click.stop="onDraft">Reset to Draft</button>
                </div>
                <div class="so-stepper">
                    <div t-attf-class="so-step{{stepClass('draft')}}">Draft</div>
                    <div t-attf-class="so-step{{stepClass('submit')}}">Submitted</div>
                    <div t-attf-class="so-step{{stepClass('approve')}}">Approved</div>
                    <div t-attf-class="so-step{{stepClass('post')}}">Posted</div>
                    <div t-attf-class="so-step{{stepClass('done')}}">Paid</div>
                </div>
            </div>

            <t t-if="state.loading"><div class="loading">Loading…</div></t>
            <t t-elif="state.error"><div class="error" t-esc="state.error"/></t>
            <t t-else="">
                <div class="so-card">
                    <div class="so-card-head">
                        <h1 class="so-doc-id" t-esc="state.record.name || 'New Expense Report'"/>
                        <div class="so-stat-btns">
                            <div class="so-stat-btn so-stat-btn-disabled">
                                <span class="so-stat-num" t-esc="formatMoney(sheetTotal)"/>
                                <span class="so-stat-lbl">Total</span>
                            </div>
                            <div t-if="state.record.move_id" class="so-stat-btn" t-on-click.stop="onOpenMove">
                                <span class="so-stat-num">📄</span>
                                <span class="so-stat-lbl">Journal Entry</span>
                            </div>
                            <div t-if="state.record.payment_move_id" class="so-stat-btn" t-on-click.stop="onOpenPayment">
                                <span class="so-stat-num">💵</span>
                                <span class="so-stat-lbl">Reimbursement</span>
                            </div>
                        </div>
                    </div>

                    <div class="so-info-grid">
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Report Name</label>
                                <input class="form-input" data-field="name" placeholder="e.g. March client visits"
                                       t-att-value="state.record.name || ''" t-att-readonly="isDraft ? undefined : true"/></div>
                            <div class="so-field-row"><label class="so-field-lbl">Employee</label>
                                <select class="form-input" data-field="employee_id" t-att-disabled="isDraft ? undefined : true">
                                    <option value="0">—</option>
                                    <t t-foreach="state.employees" t-as="o" t-key="o.id">
                                        <option t-att-value="o.id"
                                                t-att-selected="getM2oId(state.record.employee_id) === o.id ? true : undefined"
                                                t-esc="o.display"/>
                                    </t>
                                </select></div>
                            <div class="so-field-row"><label class="so-field-lbl">Date</label>
                                <DatePicker value="formatDate(state.record.date)" onSelect.bind="setDate"/></div>
                        </div>
                        <div class="so-info-col">
                            <div class="so-field-row"><label class="so-field-lbl">Paid By</label>
                                <select class="form-input" data-field="payment_mode" t-att-disabled="isDraft ? undefined : true">
                                    <option value="own_account"
                                            t-att-selected="!isCompanyPaid ? true : undefined">Employee (to reimburse)</option>
                                    <option value="company_account"
                                            t-att-selected="isCompanyPaid ? true : undefined">Company</option>
                                </select></div>
                            <div class="so-field-row"><label class="so-field-lbl">Journal</label>
                                <select class="form-input" data-field="journal_id" t-att-disabled="isDraft ? undefined : true">
                                    <option value="0">— default —</option>
                                    <t t-foreach="state.journals" t-as="o" t-key="o.id">
                                        <option t-att-value="o.id"
                                                t-att-selected="getM2oId(state.record.journal_id) === o.id ? true : undefined"
                                                t-esc="o.display"/>
                                    </t>
                                </select></div>
                            <div class="so-field-row"><label class="so-field-lbl">Notes</label>
                                <input class="form-input" data-field="note"
                                       t-att-value="state.record.note === false ? '' : (state.record.note || '')"
                                       t-att-readonly="isDraft ? undefined : true"/></div>
                        </div>
                    </div>

                    <div class="so-tabs"><span class="so-tab active">Expenses</span></div>
                    <table class="so-lines-table">
                        <thead>
                            <tr>
                                <th style="width:26%">Description</th>
                                <th style="width:12%">Date</th>
                                <th style="width:20%">Expense Account</th>
                                <th class="so-col-num" style="width:8%">Qty</th>
                                <th class="so-col-num" style="width:12%">Unit Price</th>
                                <th style="width:12%">Tax</th>
                                <th class="so-col-num" style="width:10%">Total</th>
                                <th t-if="isDraft" style="width:34px"></th>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.lines" t-as="l" t-key="l._key">
                                <tr>
                                    <td><input class="o2m-input" data-lf="name" t-att-data-line="l._key"
                                               t-att-value="l.name || ''" t-att-readonly="isDraft ? undefined : true"
                                               placeholder="What was bought"/></td>
                                    <td><input class="o2m-input" type="date" data-lf="date" t-att-data-line="l._key"
                                               t-att-value="formatDate(l.date)" t-att-readonly="isDraft ? undefined : true"/></td>
                                    <td>
                                        <select class="o2m-input" data-lf="account_id" t-att-data-line="l._key"
                                                t-att-disabled="isDraft ? undefined : true">
                                            <option value="0">— default —</option>
                                            <t t-foreach="state.accounts" t-as="a" t-key="a.id">
                                                <option t-att-value="a.id"
                                                        t-att-selected="getM2oId(l.account_id) === a.id ? true : undefined"
                                                        t-esc="a.display"/>
                                            </t>
                                        </select>
                                    </td>
                                    <td class="so-col-num"><input class="o2m-input" type="number" step="0.01"
                                               data-lf="quantity" t-att-data-line="l._key"
                                               t-att-value="l.quantity !== undefined ? l.quantity : 1"
                                               t-att-readonly="isDraft ? undefined : true"/></td>
                                    <td class="so-col-num"><input class="o2m-input" type="number" step="0.01"
                                               data-lf="unit_amount" t-att-data-line="l._key"
                                               t-att-value="l.unit_amount !== undefined ? l.unit_amount : 0"
                                               t-att-readonly="isDraft ? undefined : true"/></td>
                                    <td>
                                        <select class="o2m-input" data-lf="tax_id" t-att-data-line="l._key"
                                                t-att-disabled="isDraft ? undefined : true">
                                            <option value="0">— none —</option>
                                            <t t-foreach="state.taxes" t-as="tx" t-key="tx.id">
                                                <option t-att-value="tx.id"
                                                        t-att-selected="getM2oId(l.tax_id) === tx.id ? true : undefined"
                                                        t-esc="tx.display"/>
                                            </t>
                                        </select>
                                    </td>
                                    <td class="so-col-num" t-esc="formatMoney(lineTotal(l))"/>
                                    <td t-if="isDraft" class="so-col-del">
                                        <button class="btn btn-sm btn-danger"
                                                t-on-click.stop="() => this.removeLine(l._key)">✕</button>
                                    </td>
                                </tr>
                            </t>
                            <tr t-if="!state.lines.length">
                                <td t-att-colspan="isDraft ? 8 : 7" class="trn-empty-row">
                                    No expenses on this report yet.
                                </td>
                            </tr>
                        </tbody>
                    </table>
                    <div t-if="isDraft" class="so-line-adds">
                        <button class="btn so-add-line" t-on-click.stop="addLine">+ Add an expense</button>
                    </div>

                    <div class="so-footer">
                        <div class="so-notes-wrap">
                            <p class="exp-note">
                                SST paid on an expense is not recoverable input tax, so the full
                                tax-inclusive amount is charged to the expense account.
                            </p>
                        </div>
                        <div class="so-totals">
                            <div class="so-total-row"><span class="so-total-lbl">Tax included</span>
                                 <span class="so-total-val" t-esc="formatMoney(taxTotal)"/></div>
                            <div class="so-total-row so-total-grand"><span class="so-total-lbl">Total</span>
                                 <span class="so-total-val" t-esc="formatMoney(sheetTotal)"/></div>
                        </div>
                    </div>
                </div>

                <!-- Receipts. Read-only once the report leaves draft: what the
                     approver saw is part of the record from that point on. -->
                <AttachmentPanel model="'hr.expense.sheet'"
                                 recordId="props.recordId"
                                 title="'Receipts'"
                                 readonly="!isDraft"/>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ loading: true, error: '', record: {}, lines: [], deleted: [],
                                employees: [], journals: [], accounts: [], taxes: [] });
        this._key = 1;
        onMounted(() => this.load());
    }

    get isNew()         { return !this.props.recordId; }
    get isDraft()       { return this.isNew || (this.state.record.state || 'draft') === 'draft'; }
    get isCompanyPaid() { return this.state.record.payment_mode === 'company_account'; }

    get sheetTotal() { return this.state.lines.reduce((s, l) => s + this.lineTotal(l), 0); }
    get taxTotal()   { return this.state.lines.reduce((s, l) => s + this.lineTax(l), 0); }

    stepClass(s) {
        const order = { draft: 0, submit: 1, approve: 2, post: 3, done: 4 };
        const st = this.state.record.state || 'draft';
        if (st === 'cancel') return s === 'draft' ? ' active' : '';
        const cur = order[st] ?? 0, x = order[s];
        return x === cur ? ' active' : (x < cur ? ' done' : '');
    }

    getM2oId(v) {
        if (!v && v !== 0) return 0;
        if (typeof v === 'number') return v;
        if (Array.isArray(v) && v.length) return typeof v[0] === 'number' ? v[0] : 0;
        if (typeof v === 'string') return parseInt(v) || 0;
        return 0;
    }
    formatMoney(v) { return (parseFloat(v) || 0).toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
    formatDate(v)  { return (!v || v === false) ? '' : String(v).substring(0, 10); }

    // Mirrors the server's computation so the totals move as the user types,
    // instead of only after a round trip.
    lineTax(l) {
        const base = (parseFloat(l.quantity) || 0) * (parseFloat(l.unit_amount) || 0);
        const tax  = this.state.taxes.find(t => t.id === this.getM2oId(l.tax_id));
        if (!tax) return 0;
        if (tax.fixed) return tax.priceInclude ? 0 : tax.rate;
        return tax.priceInclude ? base - (base * 100) / (100 + tax.rate)
                                : (base * tax.rate) / 100;
    }
    lineTotal(l) {
        const base = (parseFloat(l.quantity) || 0) * (parseFloat(l.unit_amount) || 0);
        const tax  = this.state.taxes.find(t => t.id === this.getM2oId(l.tax_id));
        return (tax && !tax.priceInclude) ? base + this.lineTax(l) : base;
    }

    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const [emps, journals, accounts, taxes] = await Promise.all([
                RpcService.call('hr.employee', 'search_read', [[]], { fields: ['id','name'], limit: 300 }).catch(() => []),
                RpcService.call('account.journal', 'search_read', [[]], { fields: ['id','name','code'], limit: 100 }).catch(() => []),
                RpcService.call('account.account', 'search_read', [[]], { fields: ['id','name','code'], limit: 500 }).catch(() => []),
                RpcService.call('account.tax', 'search_read', [[]], { fields: ['id','name','amount','amount_type','price_include'], limit: 100 }).catch(() => []),
            ]);
            this.state.employees = (emps || []).map(e => ({ id: e.id, display: e.name }));
            this.state.journals  = (journals || []).map(j => ({ id: j.id, display: (j.code ? j.code + ' — ' : '') + j.name }));
            this.state.accounts  = (accounts || []).map(a => ({ id: a.id, display: (a.code ? a.code + ' ' : '') + a.name }));
            this.state.taxes     = (taxes || []).map(t => ({
                id: t.id, display: t.name, rate: parseFloat(t.amount) || 0,
                fixed: t.amount_type === 'fixed', priceInclude: !!t.price_include,
            }));

            if (this.props.recordId) {
                const r = await RpcService.call('hr.expense.sheet', 'read', [[this.props.recordId]],
                    { fields: ['name','employee_id','date','total_amount','payment_mode','state',
                               'journal_id','move_id','payment_move_id','note'] });
                this.state.record = (Array.isArray(r) ? r[0] : r) || {};
                const lines = await RpcService.call('hr.expense', 'search_read',
                    [[['sheet_id','=',this.props.recordId]]],
                    { fields: ['id','name','date','account_id','quantity','unit_amount',
                               'tax_id','tax_amount','total_amount','state'], limit: 200 });
                this.state.lines = (Array.isArray(lines) ? lines : [])
                    .map(l => ({ _key: String(this._key++), ...l }));
            } else {
                this.state.record = { state: 'draft', payment_mode: 'own_account',
                                      date: new Date().toISOString().substring(0, 10) };
                this.state.lines  = [];
            }
            this.state.deleted = [];
        } catch (e) { this.state.error = e.message || 'Failed to load the expense report'; }
        this.state.loading = false;
    }

    onChange(e) {
        const key = e.target.dataset.line;
        if (key) return this.setLineField(key, e.target.dataset.lf, e.target.value);
        const f = e.target.dataset.field;
        if (!f) return;
        this.state.record[f] = (f === 'employee_id' || f === 'journal_id')
            ? (parseInt(e.target.value) || 0) : e.target.value;
    }
    onInput(e) {
        const key = e.target.dataset.line;
        if (key) return this.setLineField(key, e.target.dataset.lf, e.target.value);
        const f = e.target.dataset.field;
        if (f && e.target.tagName !== 'SELECT') this.state.record[f] = e.target.value;
    }
    setLineField(key, field, value) {
        const l = this.state.lines.find(x => x._key === key);
        if (!l || !field) return;
        l[field] = (field === 'account_id' || field === 'tax_id') ? (parseInt(value) || 0) : value;
    }
    setDate(v) { this.state.record.date = v; }

    addLine() {
        this.state.lines.push({ _key: String(this._key++), name: '', quantity: 1, unit_amount: 0,
                                account_id: 0, tax_id: 0,
                                date: this.state.record.date || new Date().toISOString().substring(0, 10) });
    }
    removeLine(key) {
        const i = this.state.lines.findIndex(x => x._key === key);
        if (i < 0) return;
        if (this.state.lines[i].id) this.state.deleted.push(this.state.lines[i].id);
        this.state.lines.splice(i, 1);
    }

    _vals() {
        const r = this.state.record;
        return {
            name:         r.name || '',
            employee_id:  this.getM2oId(r.employee_id) || false,
            date:         r.date || false,
            payment_mode: r.payment_mode || 'own_account',
            journal_id:   this.getM2oId(r.journal_id) || false,
            note:         r.note === false ? '' : (r.note || ''),
        };
    }

    async syncLines(sheetId) {
        if (this.state.deleted.length) {
            await RpcService.call('hr.expense', 'unlink', [this.state.deleted], {});
            this.state.deleted = [];
        }
        for (const l of this.state.lines) {
            const vals = {
                sheet_id:     sheetId,
                name:         l.name || 'Expense',
                date:         l.date || false,
                employee_id:  this.getM2oId(this.state.record.employee_id) || false,
                account_id:   this.getM2oId(l.account_id) || false,
                quantity:     parseFloat(l.quantity) || 0,
                unit_amount:  parseFloat(l.unit_amount) || 0,
                tax_id:       this.getM2oId(l.tax_id) || false,
                payment_mode: this.state.record.payment_mode || 'own_account',
            };
            if (!l.id) await RpcService.call('hr.expense', 'create', [vals], {});
            else       await RpcService.call('hr.expense', 'write', [[l.id], vals], {});
        }
    }

    async onCreate() {
        try {
            const id = await RpcService.call('hr.expense.sheet', 'create', [this._vals()], {});
            await this.syncLines(id);
            this.props.onBack();
        } catch (e) { this.state.error = e.message || String(e); }
    }
    async onSave() {
        try {
            await RpcService.call('hr.expense.sheet', 'write', [[this.props.recordId], this._vals()], {});
            await this.syncLines(this.props.recordId);
            await this.load();
        } catch (e) { alert('Save failed: ' + (e.message || e)); }
    }
    async onDelete() {
        if (!confirm('Delete this expense report and its expenses?')) return;
        try {
            const ids = this.state.lines.filter(l => l.id).map(l => l.id);
            if (ids.length) await RpcService.call('hr.expense', 'unlink', [ids], {});
            await RpcService.call('hr.expense.sheet', 'unlink', [[this.props.recordId]], {});
            this.props.onBack();
        } catch (e) { alert(e.message || e); }
    }

    // Unsaved line edits are flushed before a workflow step, so what gets
    // approved is what is on screen.
    async _wf(method, confirmMsg) {
        if (confirmMsg && !confirm(confirmMsg)) return;
        try {
            if (this.isDraft) { await this.syncLines(this.props.recordId); }
            await RpcService.call('hr.expense.sheet', method, [[this.props.recordId]], {});
            await this.load();
        } catch (e) { alert(e.message || e); }
    }
    async onSubmit()  { await this._wf('action_submit'); }
    async onApprove() { await this._wf('action_approve'); }
    async onRefuse()  { await this._wf('action_refuse', 'Refuse this expense report?'); }
    async onDraft()   { await this._wf('action_reset_draft'); }
    async onPost()    { await this._wf('action_post', 'Post the journal entry for this expense report?'); }
    async onPay()     { await this._wf('action_register_payment', 'Register the reimbursement payment?'); }

    onOpenMove()    { const id = this.getM2oId(this.state.record.move_id);         if (id) window.open('/report/html/account.move/' + id, '_blank'); }
    onOpenPayment() { const id = this.getM2oId(this.state.record.payment_move_id); if (id) window.open('/report/html/account.move/' + id, '_blank'); }
    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// CUSTOM_VIEWS — models that replace ActionView entirely
//
// These take over the whole action regardless of list/form mode, so they
// short-circuit before the mode switch below.
//
// A map, not a t-elif ladder. The ladder needed a new rung AND a new
// `isXxxModel` getter per model — two edits in two places, in a 9,000-line
// file, for what is a single key/value fact. Rental adds three models; at
// that point the ladder is seven rungs of identical shape.
//
// Every class named here is defined above this point in the file.
// ----------------------------------------------------------------
const CUSTOM_VIEWS = {
    'ir.erp.settings':    ERPSettingsView,
    'ir.report.template': DocumentLayoutEditor,
    'portal.partner':     PortalUserListView,
    'res.groups':         GroupsListView,
    // Rental — loaded from components/rental/ by index.html, before this
    // file. Adding a screen is now one line here instead of a t-elif rung
    // plus a matching isXxxModel getter.
    'rental.unit':        RentalUnitGrid,
    'rental.dashboard':   RentalDashboard,
    'rental.demo.data':   RentalDemoData,
    // Custom screens for the new modules (components loaded by index.html).
    'barcode.scan':       BarcodeScan,
    'part.search':        PartSearch,
    'bank.reconcile':     BankReconcile,
    'company.admin':      CompanyAdmin,
    'db.backups':         DbBackups,
    'db.studio':          DbStudio,
    'part.lookup':        PartLookup,
    'part.catalog':       PartCatalog,
    'project.board':      TaskBoard,
    'project.timegrid':   TimesheetGrid,
    'help.center':        HelpCenter,
    'bom.editor':         BomEditor,
    'account.report':     AccountReports,
    'account.dashboard':  AccountDashboard,
    'account.settings':   AccountSettings,
};

// ----------------------------------------------------------------
// ActionView — orchestrates list ↔ form switching
// ----------------------------------------------------------------
class ActionView extends Component {
    static template = xml`
        <div class="action-view">
            <t t-if="state.loading">
                <div class="loading">Loading views…</div>
            </t>
            <t t-elif="customView">
                <t t-component="customView"/>
            </t>
            <t t-elif="state.mode === 'list'">
                <!-- docs/095: the generic view switcher. Every action used to be
                     list-or-form; these four are driven by read_group and work on
                     any model, so they are offered everywhere rather than being
                     configured per action. -->
                <div class="view-switch-bar">
                    <span class="rv-switch">
                        <button t-foreach="altViews" t-as="v" t-key="v.id"
                                t-attf-class="{{ state.altView === v.id ? 'active' : '' }}"
                                t-att-title="v.title"
                                t-on-click="() => this.setAltView(v.id)" t-esc="v.label"/>
                    </span>
                </div>
                <t t-if="state.altView === 'grouped'">
                    <GroupedListView action="currentAction" onOpenForm.bind="openForm"/>
                </t>
                <t t-elif="state.altView === 'kanban'">
                    <KanbanView action="currentAction" onOpenForm.bind="openForm"/>
                </t>
                <t t-elif="state.altView === 'pivot'">
                    <PivotView action="currentAction"/>
                </t>
                <t t-elif="state.altView === 'graph'">
                    <GraphView action="currentAction"/>
                </t>
                <t t-elif="state.altView === 'calendar'">
                    <CalendarView action="currentAction" onOpenForm.bind="openForm"/>
                </t>
                <t t-elif="isCategoryModel">
                    <ProductCategoryListView/>
                </t>
                <t t-elif="isProductModel">
                    <div class="product-browser">
                        <div class="product-browser-sidebar"
                             t-att-style="'width:' + state.catSidebarWidth + 'px'">
                            <ProductCategoryTree
                                categories="state.allCategories"
                                onSelect.bind="onCategorySelect"/>
                        </div>
                        <div class="product-browser-resizer"
                             t-on-mousedown="onCatSidebarResizeStart"/>
                        <div class="product-browser-content">
                            <ListView action="productFilteredAction"
                                      viewDef="state.listView"
                                      onOpenForm.bind="openForm"/>
                        </div>
                    </div>
                </t>
                <t t-elif="isPartnerModel">
                    <div class="contact-filter-bar">
                        <button t-attf-class="btn{{state.contactFilter==='all'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('all')">All</button>
                        <button t-attf-class="btn{{state.contactFilter==='customers'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('customers')">Customers</button>
                        <button t-attf-class="btn{{state.contactFilter==='vendors'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('vendors')">Vendors</button>
                        <button t-attf-class="btn{{state.contactFilter==='contractors'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('contractors')">Contractors</button>
                        <button t-attf-class="btn{{state.contactFilter==='companies'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('companies')">Companies</button>
                        <button t-attf-class="btn{{state.contactFilter==='individuals'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('individuals')">Individuals</button>
                        <button t-attf-class="btn{{state.contactFilter==='none'?' btn-primary':''}}"
                                t-on-click.stop="()=>this.setContactFilter('none')">None</button>
                    </div>
                    <ListView action="partnerFilteredAction"
                              viewDef="state.listView"
                              onOpenForm.bind="openForm"/>
                </t>
                <t t-else="">
                    <ListView action="currentAction"
                              viewDef="state.listView"
                              onOpenForm.bind="openForm"/>
                </t>
            </t>
            <t t-elif="state.mode === 'form'">
                <t t-if="isSaleOrderModel">
                    <SaleOrderFormView action="props.action"
                                       recordId="state.recordId"
                                       onBack.bind="backToList"/>
                </t>
                <t t-elif="isPurchaseOrderModel">
                    <PurchaseOrderFormView action="props.action"
                                           recordId="state.recordId"
                                           onBack.bind="backToList"
                                           onNavigate.bind="navigateTo"/>
                </t>
                <t t-elif="isInvoiceModel">
                    <InvoiceFormView recordId="state.recordId"
                                     onBack.bind="backToList"/>
                </t>
                <t t-elif="isStockPickingModel">
                    <TransferFormView recordId="state.recordId"
                                      onBack.bind="backToList"/>
                </t>
                <t t-elif="isStockLocationModel">
                    <LocationFormView recordId="state.recordId"
                                      onBack.bind="backToList"/>
                </t>
                <t t-elif="isStockWarehouseModel">
                    <WarehouseFormView recordId="state.recordId"
                                       onBack.bind="backToList"/>
                </t>
                <t t-elif="isProductModel">
                    <ProductFormView recordId="state.recordId"
                                     onBack.bind="backToList"
                                     onNavigate.bind="navigateTo"/>
                </t>
                <t t-elif="isBomModel">
                    <BomFormView recordId="state.recordId"
                                 onBack.bind="backToList"/>
                </t>
                <t t-elif="isPartnerModel">
                    <ContactFormView recordId="state.recordId"
                                     onBack.bind="backToList"/>
                </t>
                <t t-elif="isUsersModel">
                    <UserFormView recordId="state.recordId"
                                  onBack.bind="backToList"/>
                </t>
                <t t-elif="isAssetModel">
                    <AssetFormView recordId="state.recordId"
                                   onBack.bind="backToList"/>
                </t>
                <t t-elif="isBudgetModel">
                    <BudgetFormView recordId="state.recordId"
                                    onBack.bind="backToList"/>
                </t>
                <t t-elif="isBankAccountModel">
                    <BankAccountFormView recordId="state.recordId"
                                         onBack.bind="backToList"/>
                </t>
                <t t-elif="isExpenseSheetModel">
                    <ExpenseSheetFormView recordId="state.recordId"
                                          onBack.bind="backToList"/>
                </t>
                <t t-else="">
                    <FormView action="currentAction"
                              viewDef="state.formView"
                              recordId="state.recordId"
                              onBack.bind="backToList"/>
                </t>
            </t>
        </div>
    `;

    static components = { ListView, FormView, SaleOrderFormView, PurchaseOrderFormView, InvoiceFormView, TransferFormView, LocationFormView, WarehouseFormView, ProductFormView, BomFormView, ContactFormView, ReportSettingsView, ERPSettingsView, DocumentLayoutEditor, PortalUserListView, UserFormView, GroupsListView, ProductCategoryTree, ProductCategoryListView, AssetFormView, BudgetFormView, BankAccountFormView, ExpenseSheetFormView,
        // docs/095 — generic views, loaded from components/RecordViews.js
        GroupedListView, KanbanView, PivotView, GraphView, CalendarView };

    // docs/095 — the alternate views offered on every list.
    get altViews() {
        return [
            { id: 'list',     label: 'List',     title: 'The plain list' },
            { id: 'grouped',  label: 'Grouped',  title: 'Group rows and subtotal them' },
            { id: 'kanban',   label: 'Kanban',   title: 'A card per record, a column per group' },
            { id: 'pivot',    label: 'Pivot',    title: 'Cross-tabulate two fields' },
            { id: 'graph',    label: 'Graph',    title: 'Bar, line or pie' },
            { id: 'calendar', label: 'Calendar', title: 'Place records on a month grid' },
        ];
    }
    setAltView(v) { this.state.altView = v; }

    // Use overrideAction when navigateTo() has been called, else fall back to props.action
    get currentAction()          { return this.state.overrideAction || this.props.action; }

    get isSaleOrderModel()       { return this.currentAction.res_model === 'sale.order'; }
    get isPurchaseOrderModel()   { return this.currentAction.res_model === 'purchase.order'; }
    get isInvoiceModel()         { return this.currentAction.res_model === 'account.move'; }
    get isStockPickingModel()    { return this.currentAction.res_model === 'stock.picking'; }
    get isStockLocationModel()   { return this.currentAction.res_model === 'stock.location'; }
    get isStockWarehouseModel()  { return this.currentAction.res_model === 'stock.warehouse'; }
    get isProductModel()         { return this.currentAction.res_model === 'product.product'; }
    get isAssetModel()           { return this.currentAction.res_model === 'account.asset'; }
    get isBudgetModel()          { return this.currentAction.res_model === 'account.budget'; }
    get isBankAccountModel()     { return this.currentAction.res_model === 'account.bank.account'; }
    get isExpenseSheetModel()    { return this.currentAction.res_model === 'hr.expense.sheet'; }
    get isBomModel()             { return this.currentAction.res_model === 'mrp.bom'; }
    get isPartnerModel()         { return this.currentAction.res_model === 'res.partner'; }
    get isUsersModel()           { return this.currentAction.res_model === 'res.users'; }
    get isCategoryModel()        { return this.currentAction.res_model === 'product.category'; }

    // Whole-action takeover, or null to fall through to the mode switch.
    // Replaces isERPSettingsModel / isReportTemplateModel /
    // isPortalPartnerModel / isGroupsModel — one lookup instead of four
    // getters that differed only in the string they compared against.
    get customView()             { return CUSTOM_VIEWS[this.currentAction.res_model] || null; }

    setContactFilter(f) { this.state.contactFilter = f; }

    onCategorySelect(id) { this.state.selectedCategoryId = id; }

    onCatSidebarResizeStart(ev) {
        ev.preventDefault();
        const startX     = ev.clientX;
        const startWidth = this.state.catSidebarWidth;
        document.body.style.userSelect = 'none';
        const onMove = (e) => {
            const delta = e.clientX - startX;   // drag right → widens sidebar
            this.state.catSidebarWidth = Math.max(160, Math.min(520, startWidth + delta));
        };
        const onUp = () => {
            document.body.style.userSelect = '';
            document.removeEventListener('mousemove', onMove);
            document.removeEventListener('mouseup',   onUp);
        };
        document.addEventListener('mousemove', onMove);
        document.addEventListener('mouseup',   onUp);
    }

    // Collect id + all descendant ids for a category (for categ_id IN [...] filter)
    categoryDescendantIds(id) {
        const cats = this.state.allCategories;
        const result = [id];
        const walk = (parentId) => {
            cats.forEach(c => {
                const pid = Array.isArray(c.parent_id) ? c.parent_id[0] : 0;
                if (pid === parentId) { result.push(c.id); walk(c.id); }
            });
        };
        walk(id);
        return result;
    }

    get productFilteredAction() {
        const selId = this.state.selectedCategoryId;
        if (!selId) return this.currentAction;
        const ids = this.categoryDescendantIds(selId);
        return { ...this.currentAction, domain: [['categ_id','in',ids]] };
    }

    get partnerFilteredAction() {
        let domain = [];
        switch (this.state.contactFilter) {
            case 'customers':   domain = [['customer_rank','>',0]];        break;
            case 'vendors':     domain = [['vendor_rank','>',0]];          break;
            case 'contractors': domain = [['is_contractor','=',true]];     break;
            case 'companies':   domain = [['is_company','=',true]];        break;
            case 'individuals': domain = [['is_company','=',false]];       break;
            case 'none':        domain = [['is_company','=',false],['is_individual','=',false],['is_contractor','=',false],['customer_rank','=',0],['vendor_rank','=',0]]; break;
        }
        return { ...this.currentAction, domain };
    }

    setup() {
        this.state = useState({
            loading:            true,
            mode:               'list',
            altView:            'list',   // docs/095 — list | grouped | kanban | pivot | graph | calendar
            recordId:           null,
            listView:           null,
            formView:           null,
            overrideAction:     null,   // set by navigateTo() to browse a different model
            contactFilter:      'all',
            allCategories:      [],     // loaded for product browser tree
            selectedCategoryId: null,   // category filter for product browser
            catSidebarWidth:    260,    // draggable sidebar width (px)
        });
        onMounted(() => this.loadViews());
    }

    async loadViews() {
        this.state.loading = true;
        try {
            const model = this.props.action.res_model;
            const result = await RpcService.getViews(model, [[false, 'list'], [false, 'form']]);
            this.state.listView = result.views?.list || null;
            this.state.formView = result.views?.form || null;
            // Load all categories for the product browser tree
            if (model === 'product.product') {
                this.state.allCategories = await RpcService.call(
                    'product.category', 'search_read', [[]],
                    { fields: ['id','name','parent_id','child_count','product_count'], limit: 500 }
                ) || [];
                this.state.selectedCategoryId = null;
            }
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
        this.state.recordId      = null;
        this.state.mode          = 'list';
        this.state.overrideAction = null;   // return to original model
    }

    async navigateTo(model, domain) {
        this.state.loading = true;
        try {
            const result = await RpcService.getViews(model, [[false,'list'],[false,'form']]);
            this.state.listView       = result.views?.list || null;
            this.state.formView       = result.views?.form || null;
            this.state.overrideAction = { res_model: model, domain: domain || [] };
            this.state.recordId       = null;
            this.state.mode           = 'list';
            // docs/095 — land on the plain list. Carrying, say, a pivot across
            // to a different model would open on a cross-tab of fields that
            // model may not have.
            this.state.altView        = 'list';
        } catch (e) {
            console.error('navigateTo failed:', e);
        } finally {
            this.state.loading = false;
        }
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
        const icons = {
            accounting:    '📒',
            contacts:      '👥',
            settings:      '⚙',
            sales:         '💰',
            purchase:      '🛒',
            hr:            '🧑‍💼',
            inventory:     '📋',
            products:      '🏷️',
            manufacturing: '🏭',
        };
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
