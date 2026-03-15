/**
 * LoginPage.js — authentication form.
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
                    <label>Database</label>
                    <input t-model="state.db" placeholder="odoo" autocomplete="off"/>
                </div>
                <div class="form-group">
                    <label>Login</label>
                    <input t-model="state.login" placeholder="admin"
                           autocomplete="username" t-on-keydown="onKey"/>
                </div>
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
        db:       'odoo',
        login:    'admin',
        password: '',
        loading:  false,
        error:    null,
    });

    onKey(ev) { if (ev.key === 'Enter') this.submit(); }

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
            // Signal the root App that auth succeeded
            document.dispatchEvent(new CustomEvent('login-success'));
        } catch (e) {
            this.state.error = e.message;
        }
        this.state.loading = false;
    }
}
