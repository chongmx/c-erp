/**
 * render_forms.mjs — open a FORM for each model and report what the browser says.
 *
 *   node tests/lib/render_forms.mjs rental.contract res.partner sale.order
 *   node tests/lib/render_forms.mjs --all
 *
 * WHY THIS EXISTS, separately from render.mjs: render.mjs reaches a screen by
 * clicking menus, which lands on a LIST. An OWL template error in a FORM only
 * fires when that form first renders, so a list-level check walks straight past
 * it. Every m2o picker in this app lives in a form.
 *
 * OWL compiles a template the first time it is rendered, not when the file
 * loads. So "app.js parsed fine" proves nothing about `<M2OSelect/>` inside
 * SaleOrderFormView — only opening a sale order does.
 *
 * For each model it: finds a record (or opens a NEW form when the table is
 * empty), calls window.ErpNav.openRecord, waits for a form root, and reports
 * every console error and failed request. Exits non-zero if any model failed.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/render_forms';

// Every model whose form holds at least one converted m2o picker.
const ALL = [
    'rental.contract',   // generic FormView — the reported bug
    'res.partner',       // ContactFormView — the company picker
    'sale.order', 'purchase.order', 'account.move', 'stock.picking',
    'product.product', 'mrp.bom', 'stock.location', 'stock.warehouse',
    'account.asset', 'account.budget', 'res.partner.bank', 'hr.expense.sheet',
];

const args = process.argv.slice(2);
const models = (args.length === 0 || args[0] === '--all') ? ALL : args;

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

// ---- a session, the ordinary way ------------------------------------------
const authRes = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
                           params: { db: DB, login: 'admin', password: 'admin' } }),
});
const authJson = await authRes.json();
const sid = authJson?.session_id || authJson?.result?.session_id;
if (!sid) { console.log('FAIL could not authenticate'); process.exit(1); }

async function rpc(model, method, rpcArgs, kwargs = {}) {
    const r = await fetch(`${BASE}/web/dataset/call_kw`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Cookie': `session_id=${sid}` },
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
            model, method, args: rpcArgs,
            kwargs: { ...kwargs, context: { session_id: sid } } } }),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error.message || 'rpc error');
    return j.result;
}

const browser = await puppeteer.launch({
    executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'],
});
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 950 });

let errors = [];
page.on('pageerror',     e => errors.push('pageerror: ' + e.message));
page.on('console',       m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
page.on('requestfailed', r => errors.push('requestfailed: ' + r.url()));

await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

// A form root, whichever flavour of form this model uses.
const FORM_ROOT = '.gf-shell, .so-shell, .prd-shell, .ct-shell, .trn-shell, .form-card, .inv-shell';

/** Click the first visible button whose text is New/Create. */
async function clickNew() {
    return page.evaluate(() => {
        const btns = [...document.querySelectorAll('button, .btn')];
        const b = btns.find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (!b) return false;
        b.click();
        return true;
    });
}

const results = [];
for (const model of models) {
    // Reload for every model. One OWL exception tears down the root component
    // and takes window.ErpNav with it — without this, the first failure makes
    // every later model report a misleading "openRecord of undefined".
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });
    errors = [];

    let recId = 0, hasAction = true;
    try {
        const acts = await rpc('ir.actions.act_window', 'search_read',
            [[['res_model', '=', model]]], { fields: ['id'], limit: 1 });
        hasAction = Array.isArray(acts) && acts.length > 0;
        const ids = await rpc(model, 'search_read', [[]], { fields: ['id'], limit: 1 });
        if (Array.isArray(ids) && ids.length) recId = ids[0].id;
    } catch (_) { /* fall through; the browser will tell us */ }

    if (!hasAction) {
        results.push({ model, skipped: true, ok: true });
        console.log('SKIP ' + model.padEnd(22) + 'no act_window for this model');
        continue;
    }

    let found = false, pickers = 0, note = '', mode = recId ? 'existing' : 'new';
    try {
        await page.evaluate((m, id) => window.ErpNav.openRecord(m, id), model, recId);
        if (!recId) {
            // openRecord(model, 0) lands on the LIST — pendingRecordId is
            // `recordId || null`. Clicking New is how the form gets rendered
            // at all for a model whose table is empty on a clean baseline.
            await new Promise(r => setTimeout(r, 1200));
            if (!await clickNew()) throw new Error('no New button on the list');
        }
        await page.waitForSelector(FORM_ROOT, { timeout: 12000 });
        found = true;
        // Give the pickers their onWillStart read + first render.
        await new Promise(r => setTimeout(r, 900));
        pickers = await page.evaluate(() => document.querySelectorAll('.m2o').length);
    } catch (e) {
        note = e.message.split('\n')[0];
    }

    await page.screenshot({ path: `${SHOTDIR}/${model.replace(/\./g, '_')}.png` });
    // A blank screen with no exception is still a failure: OWL swallows some
    // template problems into an empty render.
    const blank = found && await page.evaluate(sel => {
        const el = document.querySelector(sel);
        return !el || el.textContent.trim().length < 20;
    }, FORM_ROOT);

    const ok = found && errors.length === 0 && !blank;
    results.push({ model, recId, ok, found, blank, pickers, errors: [...errors], note });
    console.log(
        (ok ? 'PASS ' : 'FAIL ') + model.padEnd(22) +
        mode.padEnd(10) +
        'pickers=' + String(pickers).padEnd(4) +
        (blank ? 'BLANK ' : '') + (note ? 'note=' + note : '') +
        (errors.length ? '\n       ' + errors.slice(0, 4).join('\n       ') : ''));
}

await browser.close();
const failed = results.filter(r => !r.ok);
const skipped = results.filter(r => r.skipped).length;
console.log(`\n${results.length - failed.length - skipped}/${results.length - skipped} forms rendered clean` +
            (skipped ? ` (${skipped} skipped).` : '.'));
console.log('screenshots: ' + SHOTDIR);
process.exit(failed.length ? 1 : 0);
