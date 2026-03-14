/**
 * rpc.js — JSON-RPC 2.0 helpers for the odoo-cpp backend.
 *
 * All functions are on the global `RpcService` object so every
 * component can call them without imports (no bundler needed).
 */
const RpcService = (() => {
    let _id = 1;

    /**
     * Call an Odoo model method via JSON-RPC.
     *
     * @param {string}   model   - e.g. "res.partner"
     * @param {string}   method  - e.g. "search_read"
     * @param {Array}    args    - positional args
     * @param {Object}   kwargs  - keyword args
     * @returns {Promise<any>}   - unwrapped result
     */
    async function call(model, method, args = [], kwargs = {}) {
        const res = await fetch('/web/dataset/call_kw', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                jsonrpc: '2.0',
                method:  'call',
                id:      _id++,
                params:  { model, method, args, kwargs },
            }),
        });
        const data = await res.json();
        if (data.error) {
            throw new Error(data.error.data?.message || data.error.message);
        }
        return data.result;
    }

    /** GET /healthz */
    async function health() {
        const res = await fetch('/healthz');
        return res.json();
    }

    /** GET /web/session/get_session_info */
    async function sessionInfo() {
        const res = await fetch('/web/session/get_session_info');
        const data = await res.json();
        return data.result ?? data;
    }

    return { call, health, sessionInfo };
})();
