/**
 * app.js — root App component and mount point.
 *
 * This file is loaded last by index.html, after all components
 * and rpc.js. It is the only file that destructures from owl —
 * all other files reference owl.* directly to avoid re-declaration
 * errors in the shared global scope.
 */
const { Component, useState, xml, mount } = owl;

const PAGES = {
    dashboard: { label: '⬡  Dashboard',        component: Dashboard },
    partners:  { label: '👤  Partners',         component: PartnerList },
    fields:    { label: '🔎  Field Inspector',  component: FieldsInspector },
};

class App extends Component {
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
                </div>
                <div class="content">
                    <t t-component="pages[state.page].component"/>
                </div>
            </div>
        </div>
    `;

    static components = { Dashboard, PartnerList, FieldsInspector };

    pages = PAGES;
    state = useState({ page: 'dashboard' });
}

mount(App, document.getElementById('app'));