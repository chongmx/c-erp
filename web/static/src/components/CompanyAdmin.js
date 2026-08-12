/**
 * CompanyAdmin.js — control-plane admin (docs/072 Phase 2/3).
 *
 * Admin screen to manage cross-company identity memberships and the shared
 * product catalogue. Backed by /web/control/admin (admin-gated). Shows a clear
 * message when the control plane is not enabled (single-company servers).
 */
class CompanyAdmin extends owl.Component {
    static template = owl.xml`
        <div class="ca-screen">
            <h2>Companies &amp; Access</h2>
            <p class="ca-sub">Cross-company identities and the shared product catalogue (control plane).</p>
            <t t-if="state.error"><div class="ca-error" t-esc="state.error"/></t>

            <div class="ca-section">
                <h3>Identity memberships</h3>
                <p class="ca-hint">Map a person (email) to a company database and their login in it.</p>
                <table class="ca-table">
                    <thead><tr><th>Identity (email)</th><th>Company (db)</th><th>Login in company</th><th></th></tr></thead>
                    <tbody>
                        <tr t-foreach="state.memberships" t-as="m" t-key="m.identity + '|' + m.tenant_db">
                            <td t-esc="m.identity"/><td t-esc="m.tenant_db"/><td t-esc="m.local_login"/>
                            <td><button class="ca-del" t-on-click="() => this.removeMembership(m)">Remove</button></td>
                        </tr>
                        <tr t-if="!state.memberships.length"><td colspan="4" class="ca-empty">No memberships yet.</td></tr>
                    </tbody>
                </table>
                <div class="ca-add">
                    <input placeholder="email" t-model="state.mIdentity"/>
                    <input placeholder="company db" t-model="state.mTenant"/>
                    <input placeholder="login in company" t-model="state.mLogin"/>
                    <button t-on-click="addMembership">Add</button>
                </div>
            </div>

            <div class="ca-section">
                <h3>Shared product catalogue</h3>
                <p class="ca-hint">Products any company can opt-in import into its own catalogue.</p>
                <table class="ca-table">
                    <thead><tr><th>Code</th><th>Name</th><th>List price</th><th></th></tr></thead>
                    <tbody>
                        <tr t-foreach="state.shared" t-as="p" t-key="p.code">
                            <td t-esc="p.code"/><td t-esc="p.name"/><td t-esc="fmt(p.list_price)"/>
                            <td><button class="ca-del" t-on-click="() => this.removeShared(p)">Remove</button></td>
                        </tr>
                        <tr t-if="!state.shared.length"><td colspan="4" class="ca-empty">No shared products.</td></tr>
                    </tbody>
                </table>
                <div class="ca-add">
                    <input placeholder="code" t-model="state.pCode"/>
                    <input placeholder="name" t-model="state.pName"/>
                    <input placeholder="price" t-model="state.pPrice"/>
                    <button t-on-click="addShared">Add</button>
                </div>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            memberships: [], shared: [], error: null,
            mIdentity: '', mTenant: '', mLogin: '',
            pCode: '', pName: '', pPrice: '',
        });
        this.reload();
    }

    async reload() {
        try {
            const a = await RpcService.controlAdmin('list_memberships');
            this.state.memberships = (a && a.memberships) || [];
            const b = await RpcService.controlAdmin('list_shared');
            this.state.shared = (b && b.shared_products) || [];
            this.state.error = null;
        } catch (e) { this.state.error = e.message; }
    }

    fmt(micros) { return (Number(micros) / 1e6).toFixed(2); }

    async addMembership() {
        try {
            await RpcService.controlAdmin('add_membership',
                { identity: this.state.mIdentity, tenant_db: this.state.mTenant, local_login: this.state.mLogin });
            this.state.mIdentity = this.state.mTenant = this.state.mLogin = '';
            this.reload();
        } catch (e) { this.state.error = e.message; }
    }
    async removeMembership(m) {
        try { await RpcService.controlAdmin('remove_membership', { identity: m.identity, tenant_db: m.tenant_db }); this.reload(); }
        catch (e) { this.state.error = e.message; }
    }
    async addShared() {
        try {
            await RpcService.controlAdmin('add_shared',
                { code: this.state.pCode, name: this.state.pName,
                  list_price: Math.round(parseFloat(this.state.pPrice || '0') * 1e6) });
            this.state.pCode = this.state.pName = this.state.pPrice = '';
            this.reload();
        } catch (e) { this.state.error = e.message; }
    }
    async removeShared(p) {
        try { await RpcService.controlAdmin('remove_shared', { code: p.code }); this.reload(); }
        catch (e) { this.state.error = e.message; }
    }
}
