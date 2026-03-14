/**
 * PartnerList.js — search_read on res.partner.
 * owl globals are destructured in app.js.
 */
class PartnerList extends owl.Component {
    static template = owl.xml`
        <div>
            <div class="card">
                <h2>Search Partners</h2>
                <div class="form-row">
                    <label>Name</label>
                    <input t-model="state.searchName"
                           placeholder="filter by name…"
                           t-on-keydown="onKey"/>
                    <select t-model="state.limit">
                        <option value="10">10</option>
                        <option value="25">25</option>
                        <option value="80">80</option>
                    </select>
                    <button t-on-click="search" t-att-disabled="state.loading">
                        <t t-if="state.loading"><span class="spinner"/></t>
                        <t t-else="">Search</t>
                    </button>
                </div>
                <t t-if="state.error">
                    <span class="badge err" t-esc="state.error"/>
                </t>
            </div>

            <div class="card" t-if="state.records.length">
                <h2>Results — <span class="badge ok" t-esc="state.records.length"/> records</h2>
                <table>
                    <thead>
                        <tr>
                            <th>ID</th><th>Name</th>
                            <th>Email</th><th>Phone</th><th>Company?</th>
                        </tr>
                    </thead>
                    <tbody>
                        <t t-foreach="state.records" t-as="r" t-key="r.id">
                            <tr>
                                <td t-esc="r.id"/>
                                <td t-esc="r.name || '—'"/>
                                <td t-esc="r.email || '—'"/>
                                <td t-esc="r.phone || '—'"/>
                                <td>
                                    <span t-attf-class="badge {{ r.is_company ? 'ok' : 'ghost' }}"
                                          t-esc="r.is_company ? 'Yes' : 'No'"/>
                                </td>
                            </tr>
                        </t>
                    </tbody>
                </table>
            </div>
        </div>
    `;

    state = owl.useState({
        searchName: '', limit: '10', loading: false, records: [], error: null
    });

    onKey(ev) { if (ev.key === 'Enter') this.search(); }

    async search() {
        this.state.loading = true;
        this.state.error   = null;
        try {
            const domain = this.state.searchName
                ? [['name', 'ilike', this.state.searchName]]
                : [];
            this.state.records = await RpcService.call(
                'res.partner', 'search_read', [domain],
                { fields: ['name','email','phone','is_company'],
                  limit: parseInt(this.state.limit) }
            );
        } catch (e) {
            this.state.error   = e.message;
            this.state.records = [];
        }
        this.state.loading = false;
    }
}