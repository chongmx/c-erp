/**
 * BankReconcile.js — Accounting → Bank Reconciliation.
 *
 * Pick a bank statement; each unreconciled line shows the open invoices/bills
 * that would clear it (suggest_matches). One click reconciles: the bank entry
 * is posted, the invoice is paid down, and the line drops off the list.
 */
class BankReconcile extends owl.Component {
    static template = owl.xml`
        <div class="br-wrap">
            <div class="br-head">
                <h2>Bank Reconciliation</h2>
                <div class="br-pick">
                    <label>Statement
                        <select class="br-sel" t-on-change="onPick">
                            <option value="">— choose —</option>
                            <t t-foreach="state.statements" t-as="s" t-key="s.id">
                                <option t-att-value="s.id" t-esc="(s.name || ('Statement #' + s.id))"/>
                            </t>
                        </select>
                    </label>
                </div>
            </div>
            <t t-if="state.error"><div class="br-note br-err" t-esc="state.error"/></t>
            <t t-if="state.loading"><div class="br-note">Loading…</div></t>

            <div class="br-lines" t-if="state.statementId">
                <t t-if="!state.lines.length">
                    <div class="br-note br-ok">Nothing left to reconcile on this statement.</div>
                </t>
                <t t-foreach="state.lines" t-as="l" t-key="l.id">
                    <div class="br-line">
                        <div class="br-line-info">
                            <span class="br-date" t-esc="l.date"/>
                            <span class="br-label" t-esc="l.name || l.payment_ref || '(no label)'"/>
                            <span class="br-partner" t-if="l.partner_id" t-esc="l.partner_id[1]"/>
                            <span t-attf-class="br-amt {{l.amount &lt; 0 ? 'br-neg' : 'br-pos'}}" t-esc="l.amount"/>
                        </div>
                        <div class="br-matches">
                            <t t-if="state.matches[l.id] &amp;&amp; state.matches[l.id].length">
                                <t t-foreach="state.matches[l.id]" t-as="m" t-key="m.id">
                                    <button class="br-match" t-att-disabled="state.busy"
                                            t-on-click="() => this.reconcile(l.id, m.id)">
                                        <span t-esc="m.name"/>
                                        <span class="br-match-amt" t-esc="m.amount_residual"/>
                                    </button>
                                </t>
                            </t>
                            <span class="br-nomatch" t-else="">No suggested match</span>
                        </div>
                    </div>
                </t>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({ statements: [], statementId: '', lines: [], matches: {}, loading: false, busy: false, error: '' });
        owl.onWillStart(async () => {
            try { this.state.statements = await RpcService.call('account.bank.statement', 'search_read', [[]], { limit: 100 }); }
            catch (e) { this.state.error = (e && e.message) || 'Could not load statements'; }
        });
    }

    async onPick(ev) {
        const id = parseInt(ev.target.value, 10);
        this.state.statementId = id || '';
        this.state.lines = []; this.state.matches = {};
        if (!id) return;
        await this.refresh();
    }

    async refresh() {
        this.state.loading = true; this.state.error = '';
        try {
            const lines = await RpcService.call('account.bank.statement.line', 'search_read',
                [[['statement_id', '=', this.state.statementId]]], {});
            this.state.lines = (lines || []).filter(l => !l.is_reconciled);
            const matches = {};
            for (const l of this.state.lines) {
                try { matches[l.id] = await RpcService.call('account.bank.statement.line', 'suggest_matches', [{ line_id: l.id }]); }
                catch (e) { matches[l.id] = []; }
            }
            this.state.matches = matches;
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load lines';
        }
        this.state.loading = false;
    }

    async reconcile(lineId, moveId) {
        this.state.busy = true; this.state.error = '';
        try {
            await RpcService.call('account.bank.statement.line', 'reconcile', [{ line_id: lineId, move_id: moveId }]);
            await this.refresh();
        } catch (e) {
            this.state.error = (e && e.message) || 'Reconcile failed';
        }
        this.state.busy = false;
    }
}
