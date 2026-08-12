/**
 * UserMenu.js — topbar company switcher + user badge + logout button.
 *
 * Multi-company (docs/072 Phase 2): when the logged-in identity can reach more
 * than one company, a switcher dropdown appears. Picking another company calls
 * the cross-tenant SSO endpoint and reloads against that tenant. On a
 * single-company / no-control-plane server the list is empty, so the switcher
 * stays hidden and the bar looks exactly as before.
 */
class UserMenu extends owl.Component {
    static template = owl.xml`
        <div class="user-menu">
            <div t-if="state.companies.length > 1" class="company-switcher">
                <button class="ghost company-current" t-on-click="toggle">
                    <span class="company-icon">🏢</span>
                    <span t-esc="currentName()"/>
                    <span class="caret">▾</span>
                </button>
                <div t-if="state.open" class="company-dropdown">
                    <div class="company-dropdown-head">Switch company</div>
                    <div t-foreach="state.companies" t-as="co" t-key="co.db"
                         class="company-item" t-att-class="{ active: co.current }"
                         t-on-click="() => this.switchTo(co)">
                        <span t-esc="co.name"/>
                        <span t-if="co.current" class="check">✓</span>
                    </div>
                </div>
            </div>
            <span class="user-badge">
                <span class="user-avatar" t-esc="initials()"/>
                <span class="user-login" t-esc="session.login"/>
            </span>
            <button class="ghost" t-on-click="logout">Sign out</button>
        </div>
    `;

    setup() {
        this.state = owl.useState({ companies: [], open: false });
        this.loadCompanies();
    }

    async loadCompanies() {
        try { this.state.companies = (await RpcService.listCompanies()) || []; }
        catch (_) { this.state.companies = []; }
    }

    currentName() {
        const c = this.state.companies.find(x => x.current);
        return c ? c.name : (this.session.db || 'Company');
    }

    toggle() { this.state.open = !this.state.open; }

    async switchTo(co) {
        this.state.open = false;
        if (co.current) return;
        try {
            await RpcService.switchCompany(co.db);
            window.location.reload();   // re-init the whole app against the new tenant
        } catch (e) {
            alert('Could not switch company: ' + (e.message || e));
        }
    }

    get session() { return RpcService.getSession(); }

    initials() {
        const login = this.session.login || '?';
        return login.substring(0, 2).toUpperCase();
    }

    async logout() {
        await RpcService.logout();
        document.dispatchEvent(new CustomEvent('logout'));
    }
}
