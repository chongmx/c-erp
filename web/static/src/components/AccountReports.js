/**
 * AccountReports.js — Financial statement reports (docs/081).
 *
 * Trial Balance, Profit & Loss, Balance Sheet, General Ledger, Aged Receivable.
 * Reads /web/account/report (JSON) for the on-screen table and opens
 * /web/account/report/print for a printable (browser → PDF) version. All figures
 * are computed server-side from posted account.move.line (double-entry safe).
 */
class AccountReports extends owl.Component {
    static template = owl.xml`
        <div class="fr-screen">
            <div class="fr-nav">
                <t t-foreach="reports" t-as="r" t-key="r.id">
                    <button t-attf-class="fr-tab{{ state.report === r.id ? ' active' : '' }}"
                            t-on-click="() => this.selectReport(r.id)" t-esc="r.name"/>
                </t>
            </div>

            <div class="fr-toolbar">
                <t t-if="current.range">
                    <label class="fr-lbl">From</label>
                    <input type="date" class="fr-date" t-att-value="state.dateFrom" t-on-change="onFrom"/>
                </t>
                <label class="fr-lbl"><t t-esc="current.range ? 'To' : 'As at'"/></label>
                <input type="date" class="fr-date" t-att-value="state.dateTo" t-on-change="onTo"/>
                <button class="fr-btn" t-on-click="load">Refresh</button>
                <button class="fr-btn fr-print" t-on-click="onPrint">🖨 Print / PDF</button>
            </div>

            <div class="fr-report">
                <t t-if="state.loading"><div class="fr-msg">Loading…</div></t>
                <t t-elif="state.error"><div class="fr-msg fr-err" t-esc="state.error"/></t>
                <t t-elif="state.data">
                    <h2 class="fr-title" t-esc="state.data.title"/>
                    <div class="fr-subtitle" t-esc="state.data.subtitle"/>
                    <table class="fr-table">
                        <thead>
                            <tr>
                                <t t-foreach="state.data.columns" t-as="c" t-key="c_index">
                                    <th t-attf-class="{{ c.align === 'right' ? 'fr-r' : '' }}" t-esc="c.label"/>
                                </t>
                            </tr>
                        </thead>
                        <tbody>
                            <t t-foreach="state.data.rows" t-as="row" t-key="row_index">
                                <tr t-attf-class="fr-row-{{ row.type }}">
                                    <t t-foreach="row.cells" t-as="cell" t-key="cell_index">
                                        <td t-attf-class="{{ cell_index === 0 ? '' : 'fr-r' }}" t-esc="cell"/>
                                    </t>
                                </tr>
                            </t>
                            <t t-if="state.data.rows.length === 0">
                                <tr><td class="fr-empty" t-att-colspan="state.data.columns.length">No entries for this period.</td></tr>
                            </t>
                        </tbody>
                    </table>
                </t>
            </div>
        </div>
    `;

    setup() {
        this.reports = [
            { id: 'trial_balance',   name: 'Trial Balance',   range: false },
            { id: 'profit_loss',     name: 'Profit & Loss',   range: true  },
            { id: 'balance_sheet',   name: 'Balance Sheet',   range: false },
            { id: 'general_ledger',  name: 'General Ledger',  range: true  },
            { id: 'aged_receivable', name: 'Aged Receivable', range: false },
            { id: 'aged_payable',    name: 'Aged Payable',    range: false },
            { id: 'partner_ledger',  name: 'Partner Ledger',  range: true  },
            { id: 'tax_report',      name: 'Tax Report (SST-02)', range: true },
            { id: 'journals_audit',  name: 'Journals Audit',  range: true  },
            { id: 'invoice_analysis',name: 'Invoice Analysis',range: true  },
            { id: 'product_margins', name: 'Product Margins', range: true  },
        ];
        const today  = new Date().toISOString().slice(0, 10);
        const yStart = today.slice(0, 4) + '-01-01';
        this.state = owl.useState({
            report: 'trial_balance', dateFrom: yStart, dateTo: today,
            data: null, loading: false, error: '',
        });
        owl.onMounted(() => this.load());
    }

    get current() { return this.reports.find(r => r.id === this.state.report) || this.reports[0]; }

    _qs() {
        return 'report=' + encodeURIComponent(this.state.report) +
               '&date_from=' + encodeURIComponent(this.state.dateFrom) +
               '&date_to='   + encodeURIComponent(this.state.dateTo);
    }

    async load() {
        this.state.loading = true; this.state.error = '';
        try {
            const res = await fetch('/web/account/report?' + this._qs(), { credentials: 'same-origin' });
            if (!res.ok) throw new Error('Could not load this report (' + res.status + ').');
            this.state.data = await res.json();
        } catch (e) {
            this.state.error = (e && e.message) || 'Failed to load report.';
            this.state.data = null;
        }
        this.state.loading = false;
    }

    selectReport(id) { this.state.report = id; this.load(); }
    onFrom(e) { this.state.dateFrom = e.target.value; this.load(); }
    onTo(e)   { this.state.dateTo   = e.target.value; this.load(); }
    onPrint() { window.open('/web/account/report/print?' + this._qs(), '_blank'); }
}
