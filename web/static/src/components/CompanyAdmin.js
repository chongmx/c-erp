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
            <p class="ca-sub">Who may work in which company, plus cross-database identities.</p>
            <t t-if="state.error"><div class="ca-error" t-esc="state.error"/></t>

            <!-- docs/094 — companies inside THIS database -->
            <div class="ca-section">
                <h3>Company access (this database)</h3>
                <p class="ca-hint">
                    Tick a company to let that user work in it. Users only ever see the records of the
                    company they are currently switched into — plus records shared across all companies.
                    Everyone needs at least one.
                </p>
                <t t-if="state.accessError"><div class="ca-error" t-esc="state.accessError"/></t>
                <div style="overflow-x:auto">
                    <table class="ca-table ca-matrix">
                        <thead>
                            <tr>
                                <th>User</th>
                                <th t-foreach="state.companies" t-as="c" t-key="c.id" t-esc="c.name"/>
                            </tr>
                        </thead>
                        <tbody>
                            <tr t-foreach="state.users" t-as="u" t-key="u.id">
                                <td>
                                    <span t-esc="u.login"/>
                                    <span t-if="u.active_company" class="ca-active-tag"
                                          t-esc="' in ' + companyName(u.active_company)"/>
                                </td>
                                <td t-foreach="state.companies" t-as="c" t-key="c.id" class="ca-cell">
                                    <input type="checkbox" t-att-checked="isAllowed(u, c.id)"
                                           t-att-disabled="state.busy"
                                           t-att-aria-label="u.login + ' in ' + c.name"
                                           t-on-change="() => this.toggleAccess(u, c.id)"/>
                                </td>
                            </tr>
                            <tr t-if="!state.users.length"><td colspan="99" class="ca-empty">No users.</td></tr>
                        </tbody>
                    </table>
                </div>
            </div>

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
            companies: [], users: [], accessError: '', busy: false,
            mIdentity: '', mTenant: '', mLogin: '',
            pCode: '', pName: '', pPrice: '',
        });
        this.loadAccess();
        this.reload();
    }

    // docs/094 — in-database company access. Loaded separately from the control
    // plane below: this works on every install, whereas the control plane is off
    // on single-database servers and its failure must not blank this table.
    async loadAccess() {
        try {
            const r = await RpcService.companyAccess('list');
            this.state.companies  = (r && r.companies) || [];
            this.state.users      = (r && r.users) || [];
            this.state.accessError = '';
        } catch (e) { this.state.accessError = (e && e.message) || 'Could not load company access.'; }
    }

    companyName(id) {
        const c = this.state.companies.find(x => x.id === id);
        return c ? c.name : ('#' + id);
    }
    isAllowed(user, companyId) {
        return !!(user.allowed && user.allowed.indexOf(companyId) !== -1);
    }
    async toggleAccess(user, companyId) {
        this.state.busy = true;
        this.state.accessError = '';
        try {
            await RpcService.companyAccess(this.isAllowed(user, companyId) ? 'revoke' : 'grant',
                                           { user_id: user.id, company_id: companyId });
        } catch (e) {
            this.state.accessError = (e && e.message) || 'Change refused.';
        }
        this.state.busy = false;
        // Reload either way: on success to pick up an active-company move the
        // server may have made, on failure to put the checkbox back where the
        // database says it should be.
        await this.loadAccess();
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
