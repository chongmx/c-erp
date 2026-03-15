/**
 * rpc.js — JSON-RPC 2.0 + session management for odoo-cpp.
 */
const RpcService = (() => {
    let _id = 1;

    // --------------------------------------------------------
    // Session state (mirrors server-side Session struct)
    // --------------------------------------------------------
    const _session = {
        uid:        0,
        login:      '',
        sessionId:  '',
        db:         '',
        context:    {},
    };

    function isAuthenticated() { return _session.uid > 0; }
    function getSession()      { return { ..._session }; }

    // --------------------------------------------------------
    // Core JSON-RPC call
    // --------------------------------------------------------
    async function call(model, method, args = [], kwargs = {}) {
        const res = await fetch('/web/dataset/call_kw', {
            method:      'POST',
            credentials: 'include',   // send session_id cookie
            headers:     { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                jsonrpc: '2.0', method: 'call', id: _id++,
                params:  { model, method, args, kwargs },
            }),
        });
        const data = await res.json();
        if (data.error) throw new Error(data.error.data?.message || data.error.message);
        return data.result;
    }

    // --------------------------------------------------------
    // authenticate — POST /web/session/authenticate
    // --------------------------------------------------------
    async function authenticate(login, password, db = 'odoo') {
        const res = await fetch('/web/session/authenticate', {
            method:      'POST',
            credentials: 'include',
            headers:     { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                jsonrpc: '2.0', method: 'call', id: _id++,
                params:  { db, login, password },
            }),
        });
        const data = await res.json();
        if (data.error) throw new Error(data.error.data?.message || data.error.message);
        if (!data.result || !data.result.uid)
            throw new Error('Invalid credentials');

        // Populate local session state
        Object.assign(_session, {
            uid:       data.result.uid,
            login:     data.result.login,
            sessionId: data.result.session_id || '',
            db:        data.result.db || db,
            context:   data.result.context || {},
        });
        return _session;
    }

    // --------------------------------------------------------
    // logout
    // --------------------------------------------------------
    async function logout() {
        await call('res.users', 'logout', [], {}).catch(() => {});
        Object.assign(_session, { uid: 0, login: '', sessionId: '', context: {} });
    }

    // --------------------------------------------------------
    // Restore session from server (called on page load)
    // --------------------------------------------------------
    async function restoreSession() {
        try {
            const res  = await fetch('/web/session/get_session_info', { credentials: 'include' });
            const data = await res.json();
            const info = data.result ?? data;
            if (info && info.uid > 0) {
                Object.assign(_session, {
                    uid:       info.uid,
                    login:     info.login || '',
                    sessionId: info.session_id || '',
                    db:        info.db || '',
                    context:   info.context || {},
                });
            }
        } catch (_) { /* not fatal */ }
        return isAuthenticated();
    }

    // --------------------------------------------------------
    // Convenience helpers
    // --------------------------------------------------------
    async function health() {
        const res = await fetch('/healthz');
        return res.json();
    }

    async function sessionInfo() {
        const res  = await fetch('/web/session/get_session_info', { credentials: 'include' });
        const data = await res.json();
        return data.result ?? data;
    }

    return { call, authenticate, logout, restoreSession,
             isAuthenticated, getSession, health, sessionInfo };
})();
