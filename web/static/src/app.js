/**
 * app.js — root App with authentication gate.
 *
 * Flow:
 *   1. On mount: call restoreSession() to check if a valid session cookie exists.
 *   2. If authenticated → show main shell.
 *   3. If not         → show LoginPage.
 *   4. listen for 'login-success' / 'logout' custom events to switch views.
 */
const { Component, useState, xml, mount, onMounted } = owl;

const PAGES = {
    dashboard: { label: '⬡  Dashboard',        component: Dashboard },
    partners:  { label: '👤  Partners',         component: PartnerList },
    fields:    { label: '🔎  Field Inspector',  component: FieldsInspector },
};

// ----------------------------------------------------------------
// Main authenticated shell
// ----------------------------------------------------------------
class MainApp extends Component {
    static template = xml`
        <div class="shell">
            <nav class="sidebar">
                <div class="sidebar-logo">odoo-cpp</div>
                <t t-foreach="Object.entries(pages)" t-as="entry" t-key="entry[0]">
                    <div t-attf-class="nav-item {{ state.page === entry[0] ? 'active' : '' }}"
                         t-on-click="() => state.page = entry[0]"
                         t-esc="entry[1].label"/>
                </t>
            </nav>
            <div class="main">
                <div class="topbar">
                    <h1 t-esc="pages[state.page].label"/>
                    <UserMenu/>
                </div>
                <div class="content">
                    <t t-component="pages[state.page].component"/>
                </div>
            </div>
        </div>
    `;

    static components = { Dashboard, PartnerList, FieldsInspector, UserMenu };

    pages = PAGES;
    state = useState({ page: 'dashboard' });
}

// ----------------------------------------------------------------
// Root App — authentication gate
// ----------------------------------------------------------------
class App extends Component {
    static template = xml`
        <div>
            <t t-if="state.loading">
                <div class="boot-screen">
                    <span class="spinner" style="width:32px;height:32px;border-width:3px"/>
                </div>
            </t>
            <t t-elif="state.authenticated">
                <MainApp/>
            </t>
            <t t-else="">
                <LoginPage/>
            </t>
        </div>
    `;

    static components = { MainApp, LoginPage };

    state = useState({ loading: true, authenticated: false });

    setup() {
        onMounted(async () => {
            // Try to restore session from cookie
            const ok = await RpcService.restoreSession();
            this.state.authenticated = ok;
            this.state.loading       = false;
        });

        // Listen for login/logout events from child components
        document.addEventListener('login-success', () => {
            this.state.authenticated = true;
        });
        document.addEventListener('logout', () => {
            this.state.authenticated = false;
        });
    }
}

mount(App, document.getElementById('app'));