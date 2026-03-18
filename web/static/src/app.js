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
                                       t-att-type="f.type === 'boolean' ? 'checkbox' : f.type === 'date' || f.type === 'datetime' ? 'date' : 'text'"
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
// InvoiceFormView — Odoo 14-style Invoice (account.move) form
// ----------------------------------------------------------------
class InvoiceFormView extends Component {
    static components = { DatePicker };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput">

            <!-- Page header -->
            <div class="so-page-header">
                <div class="so-header-left">
                    <div class="so-breadcrumbs">
                        <span class="so-bc-link" t-on-click.stop="onBack" t-esc="backLabel"/>
                        <span class="so-bc-sep">›</span>
                        <span class="so-bc-cur" t-esc="state.record.name || 'Draft Invoice'"/>
                    </div>
                    <div class="so-action-btns">
                        <t t-if="isDraft">
                            <button class="btn btn-primary" t-on-click.stop="onPost">Confirm</button>
                            <button class="btn"             t-on-click.stop="onSave">Save</button>
                        </t>
                        <t t-if="isPosted">
                            <button class="btn btn-danger"  t-on-click.stop="onCancel">Cancel</button>
                        </t>
                        <t t-if="isCancelled">
                            <button class="btn"             t-on-click.stop="onResetDraft">Reset to Draft</button>
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

                    <!-- Invoice lines (read-only) -->
                    <table class="so-lines-table" style="margin-top:16px;">
                        <thead>
                            <tr>
                                <th>Description</th>
                                <th class="so-col-num">Qty</th>
                                <th class="so-col-num">Unit Price</th>
                                <th class="so-col-subtotal">Subtotal</th>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.lines" t-as="ln" t-key="ln.id">
                                <tr>
                                    <td t-esc="ln.name || ''"/>
                                    <td class="so-col-num" t-esc="ln.quantity"/>
                                    <td class="so-col-num" t-esc="formatMoney(ln.price_unit)"/>
                                    <td class="so-col-subtotal" t-esc="formatMoney(ln.credit)"/>
                                </tr>
                            </t>
                            <t t-if="state.lines.length === 0">
                                <tr><td colspan="4" class="empty-row">No invoice lines</td></tr>
                            </t>
                        </tbody>
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
            </t>
        </div>
    `;

    get backLabel()  { return this.props.backLabel || 'Invoices'; }
    get isDraft()    { return this.state.record.state === 'draft'; }
    get isPosted()   { return this.state.record.state === 'posted'; }
    get isCancelled(){ return this.state.record.state === 'cancel'; }

    stepClass(step) {
        const order = { draft: 0, posted: 1, cancel: 2 };
        const cur   = order[this.state.record.state] ?? -1;
        const s     = order[step];
        if (s === cur) return ' active';
        if (s < cur)   return ' done';
        return '';
    }

    setup() {
        this.state = useState({
            loading:      true,
            error:        '',
            record:       {},
            lines:        [],
            partners:     [],
            journals:     [],
            paymentTerms: [],
        });
        onMounted(() => this.load());
    }

    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const fields = [
                'name', 'state', 'move_type', 'partner_id', 'journal_id',
                'invoice_date', 'due_date', 'date', 'ref', 'narration',
                'payment_term_id', 'invoice_origin', 'payment_state',
                'amount_untaxed', 'amount_tax', 'amount_total', 'amount_residual',
            ];
            const [rec] = await Promise.all([
                this.props.recordId
                    ? RpcService.call('account.move', 'read', [[this.props.recordId]], { fields })
                          .then(r => (Array.isArray(r) ? r[0] : r) || {})
                    : Promise.resolve({}),
                this.loadOpts('res.partner',          'partners',     ['id', 'name']),
                this.loadOpts('account.journal',      'journals',     ['id', 'name']),
                this.loadOpts('account.payment.term', 'paymentTerms', ['id', 'name']),
            ]);
            this.state.record = rec;
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
            const lines = await RpcService.call('account.move.line', 'search_read',
                [[['move_id', '=', this.props.recordId], ['credit', '>', 0]]],
                { fields: ['id', 'name', 'quantity', 'price_unit', 'credit'], limit: 200 });
            this.state.lines = Array.isArray(lines) ? lines : [];
        } catch (_) { this.state.lines = []; }
    }

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

    onAnyChange(e) {
        const field = e.target.dataset.field;
        if (!field) return;
        if (e.target.tagName === 'SELECT') {
            this.state.record[field] = parseInt(e.target.value) || 0;
        }
    }

    onAnyInput(e) {
        if (e.target.tagName === 'SELECT') return;
        const field = e.target.dataset.field;
        if (field) this.state.record[field] = e.target.value;
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

    async onSave() {
        try {
            if (this.state.record.id) {
                await RpcService.call('account.move', 'write',
                    [[this.state.record.id], this.collectRecord()], {});
                await this.load();
            }
        } catch (e) { this.state.error = e.message; }
    }

    async onPost() {
        try {
            if (this.state.record.id) {
                await RpcService.call('account.move', 'write',
                    [[this.state.record.id], this.collectRecord()], {});
            }
            await RpcService.call('account.move', 'action_post',
                [[this.state.record.id]], {});
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    async onCancel() {
        try {
            await RpcService.call('account.move', 'button_cancel',
                [[this.state.record.id]], {});
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    async onResetDraft() {
        try {
            await RpcService.call('account.move', 'button_draft',
                [[this.state.record.id]], {});
            await this.load();
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }
}

// ----------------------------------------------------------------
// SaleOrderFormView — Odoo 14-style Sales Order form
// ----------------------------------------------------------------
class SaleOrderFormView extends Component {
    static components = { DatePicker, InvoiceFormView };
    static template = xml`
        <div class="so-shell"
             t-on-change="onAnyChange"
             t-on-input="onAnyInput"
             t-on-click="onAnyClick">

            <!-- Invoice sub-view overlay -->
            <t t-if="state.invoiceMode">
                <InvoiceFormView recordId="state.invoiceMode.invoiceId"
                                 backLabel="'← Sales Order'"
                                 onBack.bind="closeInvoiceView"/>
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
                            <div class="so-stat-btn so-stat-btn-disabled" title="PDF preview — coming soon">
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
            invoiceMode:    null,   // null | { invoiceId: N }
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
        return this.state.record.state === 'sale';
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
                        [[['invoice_origin', '=', this.state.record.name],
                          ['move_type', '=', 'out_invoice']]],
                        { fields: ['id'], limit: 500 });
                    const ids = (Array.isArray(moves) ? moves : []).map(m => m.id);
                    this.state.invoiceIds   = ids;
                    this.state.invoiceCount = ids.length;
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
                'price_unit', 'discount', 'price_subtotal',
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
        if (this.state.invoiceMode) return;
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
        if (this.state.invoiceMode) return;
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
        if (this.state.invoiceMode) return;
        if (e.target.dataset.addLine) {
            e.preventDefault();
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
            this.applyProductDefaults(key, val);
        } else if (field === 'product_uom_qty' || field === 'price_unit' || field === 'discount') {
            this.recalcLine(line);
        }
    }

    async applyProductDefaults(key, productId) {
        try {
            const rows = await RpcService.call('product.product', 'search_read',
                [[['id', '=', productId]]],
                { fields: ['id', 'name', 'list_price', 'uom_id'], limit: 1 });
            if (!rows || !rows.length) return;
            const prod = rows[0];
            const line = this.state.lines.find(l => l._key === key);
            if (!line) return;
            line.name           = prod.name || '';
            line.price_unit     = prod.list_price || 0;
            const uomRaw        = prod.uom_id;
            line.product_uom_id = Array.isArray(uomRaw) ? uomRaw[0] : (uomRaw || 0);
            if (!line.product_uom_qty) line.product_uom_qty = 1;
            this.recalcLine(line);
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
            currency_id:         this.getM2oId(r.currency_id),
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
            const vals = {
                order_id:        parentId,
                product_id:      this.getM2oId(line.product_id),
                name:            line.name            || '',
                product_uom_qty: parseFloat(line.product_uom_qty) || 1,
                product_uom_id:  this.getM2oId(line.product_uom_id) || false,
                price_unit:      parseFloat(line.price_unit)      || 0,
                discount:        parseFloat(line.discount)         || 0,
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
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCancel() {
        try {
            await RpcService.call('sale.order', 'action_cancel',
                [[this.state.record.id]], {});
            this.state.record.state = 'cancel';
        } catch (e) { this.state.error = e.message; }
    }

    async onCreateInvoice() {
        try {
            await RpcService.call('sale.order', 'action_create_invoices',
                [[this.state.record.id]], {});
            await this.load();   // reload to update invoice count
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }

    setDateOrder(v)    { this.state.record.date_order    = v; }
    setValidityDate(v) { this.state.record.validity_date = v; }

    onViewDeliveries() {
        // Full delivery view is in the Inventory app; navigate back to list for now
        this.props.onBack();
    }

    onViewInvoices() {
        if (this.state.invoiceIds.length === 0) return;
        // Open the first (or only) invoice inline
        this.state.invoiceMode = { invoiceId: this.state.invoiceIds[0] };
    }

    closeInvoiceView() {
        this.state.invoiceMode = null;
    }
}

// ----------------------------------------------------------------
// PurchaseOrderFormView — Odoo 14-style Purchase Order form
// ----------------------------------------------------------------
class PurchaseOrderFormView extends Component {
    static components = { DatePicker };
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
                            <div class="so-stat-btn">
                                <span class="so-stat-num" t-esc="state.receiptCount"/>
                                <span class="so-stat-lbl">Receipts</span>
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
            receiptCount:   0,
        });
        this._nextKey = 1;
        onMounted(() => this.load());
    }

    get showConfirm() {
        const s = this.state.record.state;
        return this.state.isNew || s === 'draft';
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
            if (this.props.recordId) {
                await this.loadLines();
                try {
                    const picks = await RpcService.call('stock.picking', 'search_read',
                        [[['purchase_id', '=', this.props.recordId]]],
                        { fields: ['id'], limit: 500 });
                    this.state.receiptCount = Array.isArray(picks) ? picks.length : 0;
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
            this.applyProductDefaults(key, val);
        } else if (field === 'product_qty' || field === 'price_unit' || field === 'discount') {
            this.recalcLine(line);
        }
    }

    async applyProductDefaults(key, productId) {
        try {
            const rows = await RpcService.call('product.product', 'search_read',
                [[['id', '=', productId]]],
                { fields: ['id', 'name', 'standard_price', 'uom_po_id', 'uom_id'], limit: 1 });
            if (!rows || !rows.length) return;
            const prod = rows[0];
            const line = this.state.lines.find(l => l._key === key);
            if (!line) return;
            line.name           = prod.name || '';
            line.price_unit     = prod.standard_price || 0;
            const uomRaw        = prod.uom_po_id || prod.uom_id;
            line.product_uom_id = Array.isArray(uomRaw) ? uomRaw[0] : (uomRaw || 0);
            if (!line.product_qty) line.product_qty = 1;
            this.recalcLine(line);
        } catch (_) {}
    }

    collectRecord() {
        const r = this.state.record;
        return {
            partner_id:      this.getM2oId(r.partner_id),
            payment_term_id: this.getM2oId(r.payment_term_id),
            user_id:         this.getM2oId(r.user_id),
            currency_id:     this.getM2oId(r.currency_id),
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
            this.props.onBack();
        } catch (e) { this.state.error = e.message; }
    }

    async onCancel() {
        try {
            await RpcService.call('purchase.order', 'action_cancel', [[this.state.record.id]], {});
            this.state.record.state = 'cancel';
        } catch (e) { this.state.error = e.message; }
    }

    onBack() { this.props.onBack(); }

    setDateOrder(v)   { this.state.record.date_order   = v; }
    setDatePlanned(v) { this.state.record.date_planned = v; }
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
                <t t-if="isSaleOrderModel">
                    <SaleOrderFormView action="props.action"
                                       recordId="state.recordId"
                                       onBack.bind="backToList"/>
                </t>
                <t t-elif="isPurchaseOrderModel">
                    <PurchaseOrderFormView action="props.action"
                                           recordId="state.recordId"
                                           onBack.bind="backToList"/>
                </t>
                <t t-elif="isInvoiceModel">
                    <InvoiceFormView recordId="state.recordId"
                                     onBack.bind="backToList"/>
                </t>
                <t t-else="">
                    <FormView action="props.action"
                              viewDef="state.formView"
                              recordId="state.recordId"
                              onBack.bind="backToList"/>
                </t>
            </t>
        </div>
    `;

    static components = { ListView, FormView, SaleOrderFormView, PurchaseOrderFormView, InvoiceFormView };

    get isSaleOrderModel()    { return this.props.action.res_model === 'sale.order'; }
    get isPurchaseOrderModel(){ return this.props.action.res_model === 'purchase.order'; }
    get isInvoiceModel()      { return this.props.action.res_model === 'account.move'; }

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
