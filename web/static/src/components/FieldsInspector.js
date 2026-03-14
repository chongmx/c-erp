/**
 * FieldsInspector.js — calls fields_get on any model.
 * owl globals are destructured in app.js.
 */
class FieldsInspector extends owl.Component {
    static template = owl.xml`
        <div>
            <div class="card">
                <h2>Field Inspector</h2>
                <div class="form-row">
                    <label>Model</label>
                    <input t-model="state.model"
                           placeholder="e.g. res.partner"
                           t-on-keydown="onKey"/>
                    <button t-on-click="load" t-att-disabled="state.loading">
                        <t t-if="state.loading"><span class="spinner"/></t>
                        <t t-else="">Load Fields</t>
                    </button>
                </div>
                <t t-if="state.error">
                    <span class="badge err" t-esc="state.error"/>
                </t>
            </div>

            <t t-if="Object.keys(state.fields).length">
                <div class="card">
                    <h2>Fields — <t t-esc="state.model"/></h2>
                    <div class="field-grid">
                        <t t-foreach="Object.entries(state.fields)" t-as="entry" t-key="entry[0]">
                            <div class="field-item">
                                <div class="field-name"  t-esc="entry[0]"/>
                                <div class="field-label" t-esc="entry[1].string || ''"/>
                                <div class="field-type"  t-esc="entry[1].type   || ''"/>
                            </div>
                        </t>
                    </div>
                </div>
            </t>
        </div>
    `;

    state = owl.useState({ model: 'res.partner', loading: false, fields: {}, error: null });

    onKey(ev) { if (ev.key === 'Enter') this.load(); }

    async load() {
        this.state.loading = true;
        this.state.error   = null;
        this.state.fields  = {};
        try {
            this.state.fields = await RpcService.call(
                this.state.model, 'fields_get', [],
                { attributes: ['string', 'type'] }
            );
        } catch (e) {
            this.state.error = e.message;
        }
        this.state.loading = false;
    }
}