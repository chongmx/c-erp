/**
 * RentalDemoData.js — Settings → Technical → Demo Data.
 *
 * Creates and removes the demo facility that the rental screens are
 * easiest to evaluate against.
 *
 * The Remove button is DESTRUCTIVE, so the panel:
 *   - shows exactly what exists before you press anything, so the
 *     confirmation is about real numbers rather than a vague warning
 *   - requires typing the word REMOVE, because an "Are you sure?" on a
 *     data-deleting action is a reflex click, not a decision
 *   - states plainly what it will NOT touch
 */

class RentalDemoData extends owl.Component {
    static template = owl.xml`
        <div class="demo-wrap">
            <div class="demo-head">
                <h2>Demo Data</h2>
                <p>
                    A sample warehouse — 45 units across five zones, a tenant with
                    recurring tenancies, and seven recurring expense budgets — so the
                    rental screens have something to show.
                </p>
            </div>

            <t t-if="state.loading">
                <div class="demo-note">Working…</div>
            </t>

            <t t-if="state.error">
                <div class="demo-note demo-err"><t t-esc="state.error"/></div>
            </t>
            <t t-if="state.message">
                <div class="demo-note demo-ok"><t t-esc="state.message"/></div>
            </t>

            <div class="demo-card">
                <h3>Currently in the database</h3>
                <table class="demo-table">
                    <tbody>
                        <t t-foreach="rows" t-as="r" t-key="r.k">
                            <tr>
                                <td t-esc="r.label"/>
                                <td class="num" t-esc="r.n"/>
                            </tr>
                        </t>
                    </tbody>
                </table>
                <p class="demo-hint" t-if="!present">
                    No demo data present.
                </p>
            </div>

            <div class="demo-actions">
                <button class="demo-btn demo-add"
                        t-att-disabled="state.loading"
                        t-on-click="onSeed">
                    <t t-esc="present ? 'Top up demo data' : 'Add demo data'"/>
                </button>
                <span class="demo-hint">
                    Safe to run more than once — it only creates what is missing.
                </span>
            </div>

            <div class="demo-card demo-danger" t-if="present">
                <h3>Remove demo data</h3>
                <p>
                    This deletes the
                    <b><t t-esc="state.status.units"/></b> demo unit(s),
                    <b><t t-esc="state.status.tenancies"/></b> tenancy(ies) and
                    <b><t t-esc="state.status.expense_templates"/></b> expense budget(s)
                    listed above, and cannot be undone.
                </p>
                <p class="demo-hint">
                    It will <b>not</b> touch anything outside the demo set, and it
                    deliberately leaves the
                    <b><t t-esc="state.status.invoices"/></b> invoice(s) already
                    generated from it — those are posted accounting documents with
                    sequence numbers, and deleting them would leave a gap in the
                    invoice series.
                </p>
                <div class="demo-confirm">
                    <label>Type <code>REMOVE</code> to confirm</label>
                    <input t-model="state.confirm" placeholder="REMOVE"/>
                    <button class="demo-btn demo-del"
                            t-att-disabled="state.loading or state.confirm !== 'REMOVE'"
                            t-on-click="onClear">
                        Remove demo data
                    </button>
                </div>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            status: {}, loading: true, error: '', message: '', confirm: '',
        });
        owl.onWillStart(() => this.refresh());
    }

    get present() { return !!this.state.status.present; }

    get rows() {
        const s = this.state.status || {};
        return [
            { k: 'units',     label: 'Units',                    n: s.units || 0 },
            { k: 'tenancies', label: 'Tenancies',                n: s.tenancies || 0 },
            { k: 'contracts', label: 'Contracts',                n: s.contracts || 0 },
            { k: 'tmpl',      label: 'Recurring expense budgets', n: s.expense_templates || 0 },
            { k: 'entries',   label: 'Expense entries generated', n: s.expense_entries || 0 },
            { k: 'invoices',  label: 'Invoices generated (kept)', n: s.invoices || 0 },
        ];
    }

    async call(path, method) {
        this.state.loading = true;
        this.state.error = '';
        try {
            const res = await fetch(path, {
                method: method || 'GET',
                credentials: 'same-origin',
            });
            if (res.status === 401) throw new Error('Your session has expired — sign in again.');
            if (!res.ok) throw new Error(`Request failed (HTTP ${res.status})`);
            const data = await res.json();
            if (data.error) throw new Error(data.error);
            this.state.status = data;
            return data;
        } catch (e) {
            this.state.error = (e && e.message) ? e.message : 'Something went wrong.';
            return null;
        } finally {
            this.state.loading = false;
        }
    }

    async refresh() { await this.call('/rental/demo/status'); }

    async onSeed() {
        this.state.message = '';
        const d = await this.call('/rental/demo/seed', 'POST');
        if (!d) return;
        const c = d.created || {};
        // Report what actually happened, not "Done". Re-running creates
        // nothing, and saying so is more useful than a success tick that
        // looks identical either way.
        this.state.message = (c.units || c.tenancies || c.expense_templates)
            ? `Added ${c.units || 0} unit(s), ${c.tenancies || 0} tenancy(ies), ` +
              `${c.expense_templates || 0} expense budget(s).`
            : 'Everything was already present — nothing to add.';
    }

    async onClear() {
        if (this.state.confirm !== 'REMOVE') return;
        this.state.message = '';
        const d = await this.call('/rental/demo/clear', 'POST');
        if (!d) return;
        const r = d.removed || {};
        this.state.confirm = '';
        this.state.message =
            `Removed ${r.units || 0} unit(s), ${r.tenancies || 0} tenancy(ies), ` +
            `${r.contracts || 0} contract(s), ` +
            `${(r.expense_templates || 0) + (r.expense_entries || 0)} expense row(s).`;
    }
}
