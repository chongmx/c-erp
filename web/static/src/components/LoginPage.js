/**
 * LoginPage.js — authentication form with a multi-company chooser (docs/072).
 *
 * When the typed login/email maps to more than one company (via the control
 * plane), a Company dropdown replaces the free-text Database field so the user
 * picks which company to sign into; picking one fills in the local login for
 * that company. Single-company / no-control-plane servers see the plain
 * Database field, exactly as before.
 *
 * There is no "Sign up" and no "Forgot password?" here, by design: accounts are
 * created by an administrator, and a password reset only happens through a link
 * an administrator generates and sends. When the user opens such a link
 * (…/?reset_login=…&reset_token=…) this same page detects the token in the URL
 * and shows a "set a new password" panel instead of the login form — that is
 * the only self-service moment, and it needs a token the user could only have
 * received from an admin.
 *
 * Emits a "login-success" custom event on the document when auth succeeds.
 */
class LoginPage extends owl.Component {
    static template = owl.xml`
        <div class="login-shell">
            <div class="login-card">
                <div class="login-logo">c-erp</div>
                <p class="login-sub">C++ ERP · fast by construction</p>

                <!-- ============ password-reset panel (admin-issued link) ============ -->
                <t t-if="state.resetMode">
                    <t t-if="state.resetDone">
                        <div class="login-ok">Password updated. You can sign in now.</div>
                        <button class="login-btn" t-on-click="leaveReset">Back to sign in</button>
                    </t>
                    <t t-else="">
                        <p class="login-sub" style="margin-top:0">
                            Set a new password for <strong t-esc="state.resetLogin"/>
                        </p>
                        <t t-if="state.error">
                            <div class="login-error" t-esc="state.error"/>
                        </t>
                        <div class="form-group">
                            <label>New password</label>
                            <input type="password" t-model="state.resetPw"
                                   placeholder="At least 8 characters"
                                   autocomplete="new-password" t-on-keydown="onResetKey"/>
                        </div>
                        <div class="form-group">
                            <label>Confirm password</label>
                            <input type="password" t-model="state.resetPw2"
                                   placeholder="Repeat password"
                                   autocomplete="new-password" t-on-keydown="onResetKey"/>
                        </div>
                        <button class="login-btn" t-on-click="submitReset"
                                t-att-disabled="state.loading">
                            <t t-if="state.loading"><span class="spinner"/></t>
                            <t t-else="">Set password</t>
                        </button>
                        <button class="login-link" t-on-click="leaveReset">Cancel</button>
                    </t>
                </t>

                <!-- ============ normal sign-in ============ -->
                <t t-else="">
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
                </t>
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
        // reset-via-link panel
        resetMode:  false,
        resetLogin: '',
        resetToken: '',
        resetPw:    '',
        resetPw2:   '',
        resetDone:  false,
    });

    setup() {
        // An admin-issued reset link carries the login and a one-time token in
        // the query string. Their presence — and only their presence — flips
        // this page into "set a new password" mode.
        const p = new URLSearchParams(window.location.search);
        const token = p.get('reset_token');
        const login = p.get('reset_login');
        if (token && login) {
            this.state.resetMode  = true;
            this.state.resetLogin = login;
            this.state.resetToken = token;
        }
    }

    onKey(ev) { if (ev.key === 'Enter') this.submit(); }
    onResetKey(ev) { if (ev.key === 'Enter') this.submitReset(); }

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

    // Complete an admin-issued reset by POSTing the token + new password to the
    // completion route. The token is the credential here — the server validates
    // it, enforces the 8-char floor again, and spends it single-use.
    async submitReset() {
        const pw  = this.state.resetPw  || '';
        const pw2 = this.state.resetPw2 || '';
        if (pw.length < 8) { this.state.error = 'Password must be at least 8 characters.'; return; }
        if (pw !== pw2)    { this.state.error = 'Passwords do not match.'; return; }
        this.state.loading = true;
        this.state.error   = null;
        try {
            const resp = await fetch('/web/reset_password', {
                method:  'POST',
                headers: { 'Content-Type': 'application/json' },
                body:    JSON.stringify({
                    login:    this.state.resetLogin,
                    token:    this.state.resetToken,
                    password: pw,
                }),
            });
            const data = await resp.json().catch(() => ({}));
            if (!resp.ok || (data && data.error)) {
                this.state.error = (data && data.error) ||
                    'This reset link is invalid or has expired. Ask your administrator for a new one.';
            } else {
                this.state.resetDone = true;
                // Scrub the token from the address bar so it is not left in
                // history or copied by accident.
                try { window.history.replaceState({}, document.title, window.location.pathname); }
                catch (e) { /* non-fatal */ }
            }
        } catch (e) {
            this.state.error = 'Could not reach the server. Please try again.';
        }
        this.state.loading = false;
    }

    // Leave the reset panel and return to the ordinary sign-in form.
    leaveReset() {
        this.state.resetMode = false;
        this.state.resetDone = false;
        this.state.error     = null;
        this.state.resetPw   = '';
        this.state.resetPw2  = '';
        try { window.history.replaceState({}, document.title, window.location.pathname); }
        catch (e) { /* non-fatal */ }
    }
}
