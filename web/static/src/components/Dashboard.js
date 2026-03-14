/**
 * Dashboard.js — server status panel.
 * owl globals (Component, useState, etc.) are destructured in app.js.
 */
class Dashboard extends owl.Component {
    static template = owl.xml`
        <div>
            <div class="card">
                <h2>Server Status</h2>
                <t t-if="state.loading"><div class="spinner"/></t>
                <t t-else="">
                    <div class="status-row">
                        <span class="status-label">HTTP Server</span>
                        <span class="badge ok">online</span>
                    </div>
                    <div class="status-row">
                        <span class="status-label">Database</span>
                        <span t-attf-class="badge {{ state.health.status === 'ok' ? 'ok' : 'err' }}"
                              t-esc="state.health.status ?? 'unknown'"/>
                    </div>
                    <div class="status-row">
                        <span class="status-label">Session UID</span>
                        <span class="badge ok" t-esc="state.session.uid ?? 0"/>
                    </div>
                </t>
            </div>
            <div class="card">
                <button t-on-click="refresh">Refresh</button>
            </div>
        </div>
    `;

    state = owl.useState({ loading: true, health: {}, session: {} });

    setup() { owl.onMounted(() => this.refresh()); }

    async refresh() {
        this.state.loading = true;
        try {
            const [health, session] = await Promise.all([
                RpcService.health(),
                RpcService.sessionInfo(),
            ]);
            this.state.health  = health;
            this.state.session = session;
        } catch (e) {
            this.state.health = { status: 'error: ' + e.message };
        }
        this.state.loading = false;
    }
}