/**
 * PartSearch.js — Products → Parametric Search.
 *
 * The electronics-catalogue differentiator: find parts by a parameter's
 * numeric range (e.g. Resistance between 1k and 10k) via
 * part.parameter.search_parts. Results link back to the product form.
 */
class PartSearch extends owl.Component {
    static template = owl.xml`
        <div class="ps-wrap">
            <div class="ps-head">
                <h2>Parametric Search</h2>
                <p>Find parts whose parameter value falls in a numeric range.</p>
            </div>
            <div class="ps-form">
                <label>Parameter
                    <input list="ps-names" class="ps-in" t-model="state.name"
                           placeholder="e.g. Resistance"/>
                </label>
                <datalist id="ps-names">
                    <t t-foreach="state.names" t-as="n" t-key="n"><option t-att-value="n"/></t>
                </datalist>
                <label>Min <input class="ps-in ps-num" t-model="state.min" placeholder="min"/></label>
                <label>Max <input class="ps-in ps-num" t-model="state.max" placeholder="max"/></label>
                <button class="btn btn-primary" t-on-click="onSearch">Search</button>
            </div>
            <t t-if="state.error"><div class="ps-note ps-err" t-esc="state.error"/></t>
            <div class="ps-card">
                <h3><t t-esc="state.results.length"/> matching part(s)</h3>
                <table class="ps-table">
                    <thead><tr><th>Product</th><th class="num">Value</th></tr></thead>
                    <tbody>
                        <t t-foreach="state.results" t-as="r" t-key="r.id">
                            <tr class="ps-row" t-on-click="() => this.openProduct(r.id)">
                                <td t-esc="r.name"/>
                                <td class="num" t-esc="r.value"/>
                            </tr>
                        </t>
                        <tr t-if="!state.results.length &amp;&amp; state.searched">
                            <td colspan="2" class="ps-empty">No parts match.</td>
                        </tr>
                    </tbody>
                </table>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({ name: '', min: '', max: '', results: [], names: [], searched: false, error: '' });
        owl.onWillStart(async () => {
            try {
                const params = await RpcService.call('part.parameter', 'search_read', [[]], { fields: ['name'], limit: 500 });
                this.state.names = [...new Set((params || []).map(p => p.name).filter(Boolean))].sort();
            } catch (e) { /* names are a convenience */ }
        });
    }

    async onSearch() {
        this.state.error = '';
        const q = {};
        if ((this.state.name || '').trim()) q.name = this.state.name.trim();
        if (this.state.min !== '' && !isNaN(parseFloat(this.state.min))) q.min = parseFloat(this.state.min);
        if (this.state.max !== '' && !isNaN(parseFloat(this.state.max))) q.max = parseFloat(this.state.max);
        try {
            this.state.results = await RpcService.call('part.parameter', 'search_parts', [q]);
            this.state.searched = true;
        } catch (e) {
            this.state.error = (e && e.message) || 'Search failed';
        }
    }

    openProduct(id) {
        // Navigate to the product form via a hashchange the shell listens to.
        window.location.hash = '#action=products&view=form&id=' + id;
    }
}
