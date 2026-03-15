/**
 * UserMenu.js — topbar user badge + logout button.
 */
class UserMenu extends owl.Component {
    static template = owl.xml`
        <div class="user-menu">
            <span class="user-badge">
                <span class="user-avatar" t-esc="initials()"/>
                <span class="user-login" t-esc="session.login"/>
            </span>
            <button class="ghost" t-on-click="logout">Sign out</button>
        </div>
    `;

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
