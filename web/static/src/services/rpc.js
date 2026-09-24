/**
 * rpc.js — JSON-RPC 2.0 + session management for c-erp.
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
    /**
     * How long any one call may take before it is given up on.
     *
     * A request that never answers — a stalled connection, a CDN holding it
     * open — leaves the screen that made it waiting for ever: the Save button
     * on Settings sat on "Saving…" with nothing to show, because the promise
     * simply never settled. Nothing is retried; the caller gets an error it
     * can put on screen, and the person can press the button again.
     */
    function rpcTimeoutMs() {
        const t = (typeof window !== 'undefined' && window.UI_TIMING) ? window.UI_TIMING.rpcTimeout : 0;
        return Number.isFinite(t) && t > 0 ? t : 45000;   // read per call, so a test can shorten it
    }

    /**
     * @param opts.timeoutMs  how long THIS call may take, when the default is
     *   wrong for it. Asking a model a question is the case: the server is
     *   allowed up to its configured ceiling — minutes, for a browsing
     *   search — and a browser that gives up at 45 s reports a timeout for a
     *   call that was still running and would have answered (CERP-10).
     *   Callers pass the server's own limit plus a margin.
     */
    async function call(model, method, args = [], kwargs = {}, opts = {}) {
        // Inject session_id into context so the server can resolve the session
        // from the request body (fallback when cookies aren't transmitted).
        const ctx = Object.assign({ session_id: _session.sessionId }, kwargs.context || {});
        const fullKwargs = Object.assign({}, kwargs, { context: ctx });
        const asked = opts && Number(opts.timeoutMs);
        const limit = Number.isFinite(asked) && asked > 0 ? asked : rpcTimeoutMs();
        const ctrl = typeof AbortController === 'function' ? new AbortController() : null;
        const timer = ctrl ? setTimeout(() => ctrl.abort(), limit) : 0;
        let res;
        try {
            res = await fetch('/web/dataset/call_kw', {
                method:      'POST',
                credentials: 'include',
                headers:     { 'Content-Type': 'application/json' },
                signal:      ctrl ? ctrl.signal : undefined,
                body: JSON.stringify({
                    jsonrpc: '2.0', method: 'call', id: _id++,
                    params:  { model, method, args, kwargs: fullKwargs },
                }),
            });
        } catch (e) {
            if (e && e.name === 'AbortError')
                // "Nothing was saved" is only true of the default case — a
                // call given its own longer budget is usually a question, not
                // a save, and telling someone their work was lost when it was
                // never a write is worse than saying nothing about it.
                throw new Error('The server did not answer within ' +
                                Math.round(limit / 1000) + ' s. ' +
                                (asked > 0 ? 'Try again, or allow it longer in Settings → AI agent.'
                                           : 'Nothing was saved — try again.'));
            throw e;
        } finally {
            if (timer) clearTimeout(timer);
        }
        // A reply that is not JSON is a proxy or CDN page, not the server.
        let data;
        try {
            data = await res.json();
        } catch (_) {
            throw new Error('The server sent an unreadable reply (HTTP ' + res.status + ').');
        }
        if (data.error) {
            const err = new Error(data.error.data?.message || data.error.message);
            err.code  = data.error.code;
            err.type  = data.error.data?.name || '';
            throw err;
        }
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
        // Views can differ by user — a group the next login lacks may hide a
        // field — so the cache must not outlive the session that filled it.
        _viewCache.clear();
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
    // IR helpers
    // --------------------------------------------------------
    async function loadMenus() {
        return call('ir.ui.menu', 'load_menus', [false], {});
    }

    async function loadAction(actionId) {
        const res = await fetch('/web/action/load', {
            method:      'POST',
            credentials: 'include',
            headers:     { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                jsonrpc: '2.0', method: 'call', id: _id++,
                params:  { action_id: actionId, session_id: _session.sessionId },
            }),
        });
        const data = await res.json();
        if (data.error) throw new Error(data.error.data?.message || data.error.message);
        return data.result;
    }

    // A view definition is static for the life of a session: it comes from the
    // views registered at boot, not from data. Fetching it on every screen
    // change cost a whole round trip before anything could paint — which is
    // the "Loading views…" you see, and on a remote server it is the slowest
    // part of opening a list. Cached per model+shape, so the second visit to a
    // screen paints from memory.
    //
    // Cleared on logout with the rest of the session state; a server restart
    // that changed a view needs a page reload, which is already true of every
    // other file the browser holds.
    const _viewCache = new Map();

    async function getViews(model, views) {
        // views = [[false, 'list'], [false, 'form']]
        const key = model + '|' + JSON.stringify(views);
        if (_viewCache.has(key)) return _viewCache.get(key);
        const result = await call(model, 'get_views', [views], {});
        _viewCache.set(key, result);
        return result;
    }

    function clearViewCache() { _viewCache.clear(); }

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

    // --------------------------------------------------------
    // Multi-company (docs/072): the companies this identity can reach, and
    // cross-tenant SSO switching between them (top-bar switcher).
    // --------------------------------------------------------
    async function listCompanies() {
        try {
            const res = await fetch('/web/session/companies', {
                method: 'POST', credentials: 'include',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ jsonrpc: '2.0', method: 'call', id: _id++,
                    params: { context: { session_id: _session.sessionId } } }),
            });
            const data = await res.json();
            return (data.result ?? data) || [];
        } catch (_) { return []; }
    }

    async function switchCompany(db) {
        const res = await fetch('/web/session/switch_company', {
            method: 'POST', credentials: 'include',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', method: 'call', id: _id++,
                params: { company: db, context: { session_id: _session.sessionId } } }),
        });
        const data = await res.json();
        const info = data.result ?? data;
        if (!info || info.error) throw new Error((info && info.error) || 'switch failed');
        Object.assign(_session, {
            uid:       info.uid,
            login:     info.login,
            sessionId: info.session_id || '',
            db:        info.db || db,
            context:   info.context || {},
        });
        return info;
    }

    // Pre-login chooser: the companies an email/login can reach.
    async function lookupCompanies(login) {
        try {
            const res = await fetch('/web/session/lookup_companies', {
                method: 'POST', credentials: 'include',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ jsonrpc: '2.0', method: 'call', id: _id++,
                    params: { login } }),
            });
            const data = await res.json();
            return (data.result ?? data) || [];
        } catch (_) { return []; }
    }

    // Control-plane admin (identity memberships + shared catalogue). Admin only.
    async function controlAdmin(op, args = {}) {
        const res = await fetch('/web/control/admin', {
            method: 'POST', credentials: 'include',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', method: 'call', id: _id++,
                params: Object.assign({ op, context: { session_id: _session.sessionId } }, args) }),
        });
        const data = await res.json();
        const info = data.result ?? data;
        if (info && info.error) throw new Error(info.error);
        return info;
    }

    // Database section (docs/075) — per-tenant snapshots, admin + password gated.
    async function _dbPost(path, extra = {}) {
        const res = await fetch(path, {
            method: 'POST', credentials: 'include',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', method: 'call', id: _id++,
                params: Object.assign({ context: { session_id: _session.sessionId } }, extra) }),
        });
        const data = await res.json();
        return data.result ?? data;
    }
    const dbList    = ()               => _dbPost('/web/db/list');
    const dbBackup  = (label = '')     => _dbPost('/web/db/backup', { label });
    const dbRestore = (file, password) => _dbPost('/web/db/restore', { file, password });
    const dbDelete  = (file)           => _dbPost('/web/db/delete', { file });
    async function dbUpload(file) {
        const buf = await file.arrayBuffer();
        const res = await fetch('/web/db/upload?name=' + encodeURIComponent(file.name), {
            method: 'POST', credentials: 'include',
            headers: { 'Content-Type': 'application/octet-stream' }, body: buf,
        });
        return res.json();
    }
    const dbDownloadUrl = (file) =>
        '/web/db/download?file=' + encodeURIComponent(file) +
        '&session_id=' + encodeURIComponent(_session.sessionId);

    // In-database multi-company (docs/094). Distinct from switchCompany() above,
    // which moves the session to a different tenant DATABASE (docs/072); these
    // stay in the same database and change which company's records are visible.
    async function myCompanies() {
        const info = await _dbPost('/web/session/my_companies');
        if (info && info.error) throw new Error(info.error);
        return info || { companies: [], active: 0 };
    }
    async function setActiveCompany(companyId) {
        const info = await _dbPost('/web/session/set_active_company', { company_id: companyId });
        if (info && info.error) throw new Error(info.error);
        return info;
    }
    async function companyAccess(op, args = {}) {
        const info = await _dbPost('/web/company/access', Object.assign({ op }, args));
        if (info && info.error) throw new Error(info.error);
        return info;
    }

    // Database Tools (docs/093) — read-only browser / console / schema map.
    // Every op is admin-gated and runs inside a READ ONLY transaction server-side.
    // Errors come back as {error}; throwing here keeps the callers to try/catch.
    // The payload arrives under `data`, not `result`. _dbPost above ends with
    // `data.result ?? data` to peel a JSON-RPC envelope; when this endpoint also
    // used `result` that line peeled it, and the unwrap below peeled it a second
    // time, so every call returned {} and the whole screen rendered blank.
    async function dbTool(op, args = {}) {
        const info = await _dbPost('/web/dbtool', Object.assign({ op }, args));
        if (info && info.error) {
            const err = new Error(info.error);
            err.isSqlError = !!info.sql_error;   // console shows these verbatim
            throw err;
        }
        return (info && info.data) || {};
    }

    return { call, authenticate, logout, restoreSession,
             isAuthenticated, getSession,
             loadMenus, loadAction, getViews, clearViewCache,
             health, sessionInfo,
             listCompanies, switchCompany, lookupCompanies, controlAdmin,
             dbList, dbBackup, dbRestore, dbDelete, dbUpload, dbDownloadUrl,
             dbTool, myCompanies, setActiveCompany, companyAccess };
})();
