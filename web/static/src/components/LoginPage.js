/**
 * LoginPage.js — authentication form with a multi-company chooser (docs/072).
 *
 * When the typed login/email maps to more than one company (via the control
 * plane), a Company dropdown replaces the free-text Database field so the user
 * picks which company to sign into; picking one fills in the local login for
 * that company. Single-company / no-control-plane servers see the plain
 * Database field, exactly as before.
 *
 * Emits a "login-success" custom event on the document when auth succeeds.
 */
class LoginPage extends owl.Component {
    static template = owl.xml`
        <div class="login-shell">
            <div class="login-card">
                <div class="login-logo">odoo-cpp</div>
                <p class="login-sub">C++ Backend · Odoo 19 Compatible</p>

                <t t-if="state.error">
                    <div class="login-error" t-esc="state.error"/>
                </t>

                <div class="form-group">
                    <label>Login</label>
                    <input t-att-value="state.login" placeholder="you@company.com"
                           autocomplete="username" t-on-keydown="onKey" t-on-input="onLoginInput"/>
                </div>

                <t t-if="state.companies.length > 1">
                    <div class="form-group">
                        <label>Company</label>
                        <select t-model="state.db" t-on-change="onPickCompany">
                            <option t-foreach="state.companies" t-as="co" t-key="co.db"
                                    t-att-value="co.db" t-esc="co.name"/>
                        </select>
                    </div>
                </t>
                <t t-else="">
                    <div class="form-group">
                        <label>Database</label>
                        <input t-model="state.db" placeholder="odoo" autocomplete="off"/>
                    </div>
                </t>

                <div class="form-group">
                    <label>Password</label>
                    <input type="password" t-model="state.password"
                           placeholder="••••••••"
                           autocomplete="current-password" t-on-keydown="onKey"/>
                </div>

                <button class="login-btn"
                        t-on-click="submit"
                        t-att-disabled="state.loading">
                    <t t-if="state.loading"><span class="spinner"/></t>
                    <t t-else="">Sign In</t>
                </button>
            </div>
        </div>
    `;

    state = owl.useState({
        db:        'odoo',
        login:     'admin',
        password:  '',
        loading:   false,
        error:     null,
        companies: [],
    });

    onKey(ev) { if (ev.key === 'Enter') this.submit(); }

    // Resolve companies a short moment after the user stops typing their login.
    // Uses t-on-input (which bubbles) rather than blur (which does not).
    onLoginInput(ev) {
        this.state.login = ev.target.value;
        clearTimeout(this._resolveTimer);
        this._resolveTimer = setTimeout(() => this.resolveCompanies(), 450);
    }

    async resolveCompanies() {
        const login = (this.state.login || '').trim();
        if (!login) { this.state.companies = []; return; }
        const companies = await RpcService.lookupCompanies(login);
        this.state.companies = companies || [];
        if (this.state.companies.length === 1) {
            this.state.db = this.state.companies[0].db;
            if (this.state.companies[0].login) this.state.login = this.state.companies[0].login;
        } else if (this.state.companies.length > 1) {
            this.state.db = this.state.companies[0].db;   // default to the first
        }
    }

    onPickCompany() {
        const co = this.state.companies.find(c => c.db === this.state.db);
        if (co && co.login) this.state.login = co.login;
    }

    async submit() {
        if (!this.state.login || !this.state.password) {
            this.state.error = 'Login and password are required.';
            return;
        }
        this.state.loading = true;
        this.state.error   = null;
        try {
            await RpcService.authenticate(
                this.state.login, this.state.password, this.state.db);
            document.dispatchEvent(new CustomEvent('login-success'));
        } catch (e) {
            this.state.error = e.message;
        }
        this.state.loading = false;
    }
}
