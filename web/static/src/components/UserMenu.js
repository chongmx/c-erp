/**
 * UserMenu.js — topbar company switcher + user badge + logout button.
 *
 * There are two different things called "switching company", and this menu
 * shows both, because from the user's side they are one question — "which
 * company am I working in?" — even though the machinery differs:
 *
 *   • docs/094, in this database: several companies share one database and one
 *     login. Switching changes which company's records are visible; the page
 *     reloads but the session and the database stay put.
 *   • docs/072, another database: a separate tenant entirely, reached by
 *     cross-tenant SSO. Switching re-authenticates against that database.
 *
 * They are listed under separate headings rather than merged, because the
 * blast radius is different and the user should be able to tell which one they
 * are about to do. On a single-company, single-tenant server both lists have at
 * most one entry, the switcher stays hidden, and the bar looks as it always did.
 */
class UserMenu extends owl.Component {
    static template = owl.xml`
        <div class="user-menu">
            <!-- The active company is ALWAYS shown, switcher or not.
                 It used to appear only when there was somewhere to switch TO,
                 so on a single-company database nothing anywhere told you which
                 company you were working in — and every record silently carries
                 that company. With one company this is a plain label; with more
                 it becomes the switcher. -->
            <div class="company-switcher">
                <span t-if="!hasSwitcher" class="company-current company-static">
                    <span class="company-icon">🏢</span>
                    <span t-esc="currentName()"/>
                </span>
                <button t-if="hasSwitcher" class="ghost company-current" t-on-click="toggle"
                        t-att-aria-expanded="state.open ? 'true' : 'false'">
                    <span class="company-icon">🏢</span>
                    <span t-esc="currentName()"/>
                    <span class="caret">▾</span>
                </button>
                <div t-if="state.open" class="company-dropdown">
                    <t t-if="state.dbCompanies.length > 1">
                        <div class="company-dropdown-head">Companies in this database</div>
                        <div t-foreach="state.dbCompanies" t-as="co" t-key="'db' + co.id"
                             class="company-item" t-att-class="{ active: co.id === state.activeId }"
                             t-on-click="() => this.switchInDb(co)">
                            <span t-esc="co.name"/>
                            <span t-if="co.id === state.activeId" class="check">✓</span>
                        </div>
                    </t>
                    <t t-if="state.tenants.length > 1">
                        <div class="company-dropdown-head">Other databases</div>
                        <div t-foreach="state.tenants" t-as="co" t-key="'t' + co.db"
                             class="company-item" t-att-class="{ active: co.current }"
                             t-on-click="() => this.switchTenant(co)">
                            <span t-esc="co.name"/>
                            <span t-if="co.current" class="check">✓</span>
                        </div>
                    </t>
                    <div t-if="state.error" class="company-dropdown-err" t-esc="state.error"/>
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
        this.state = owl.useState({
            tenants: [], dbCompanies: [], activeId: 0, open: false, error: '',
        });
        this.load();
    }

    async load() {
        // Independent and both optional: a server with no control plane has no
        // tenants, and a fresh install has one company. Neither failing should
        // take the other down, nor the topbar with it.
        try { this.state.tenants = (await RpcService.listCompanies()) || []; }
        catch (_) { this.state.tenants = []; }
        try {
            const r = await RpcService.myCompanies();
            this.state.dbCompanies = (r && r.companies) || [];
            this.state.activeId    = (r && r.active) || 0;
        } catch (_) { this.state.dbCompanies = []; }
    }

    get hasSwitcher() {
        return this.state.dbCompanies.length > 1 || this.state.tenants.length > 1;
    }

    currentName() {
        const inDb = this.state.dbCompanies.find(x => x.id === this.state.activeId);
        if (inDb) return inDb.name;
        const t = this.state.tenants.find(x => x.current);
        return t ? t.name : (this.session.db || 'Company');
    }

    toggle() { this.state.open = !this.state.open; this.state.error = ''; }

    async switchInDb(co) {
        if (co.id === this.state.activeId) { this.state.open = false; return; }
        try {
            await RpcService.setActiveCompany(co.id);
            window.location.reload();   // every list on screen is now scoped elsewhere
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not switch company.';
        }
    }

    async switchTenant(co) {
        this.state.open = false;
        if (co.current) return;
        try {
            await RpcService.switchCompany(co.db);
            window.location.reload();   // re-init the whole app against the new tenant
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not switch database.';
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
