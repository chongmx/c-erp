/**
 * render_contract.mjs — start a rental contract THROUGH THE SCREEN.
 *
 *   node tests/lib/render_contract.mjs "ZZLIFE Sunrise Traders Sdn Bhd"
 *
 * Picks an EXISTING customer out of the Customer combobox on a new rental
 * contract, fills the form, presses Create, and prints the id of what was
 * saved. Exits non-zero if any step fails.
 *
 * WHY THIS EXISTS. The functional test creates its contract over the API,
 * which proves the billing arithmetic and proves nothing at all about the
 * screen: a combobox that cannot find the customer, or that loses the value on
 * save, passes an API test every time. It was reported twice as "I cannot
 * select this company in my new rental contract" while every API test was
 * green.
 *
 * Two things make this different from render_pick.mjs:
 *
 *   - the customer ALREADY EXISTS and was not created by this script, so it
 *     exercises finding a record among the others rather than one planted a
 *     second ago;
 *   - it SAVES, and prints what the database ended up with, so a value that
 *     looks right in the box but never arrives is caught.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOT   = process.env.SHOT || '/tmp/render_contract.png';

const CUSTOMER = process.argv[2];
const REF      = process.argv[3] || ('UI-RC-' + String(Date.now()).slice(-6));
if (!CUSTOMER) { console.log('FAIL usage: render_contract.mjs "<customer name>" [ref]'); process.exit(1); }

const puppeteer = await import('puppeteer-core');

const a = await (await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }) })).json();
const sid = a?.session_id || a?.result?.session_id;
if (!sid) { console.log('FAIL could not authenticate'); process.exit(1); }

async function rpc(model, method, args, kwargs = {}) {
    const r = await fetch(`${BASE}/web/dataset/call_kw`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Cookie: `session_id=${sid}` },
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
            model, method, args, kwargs: { ...kwargs, context: { session_id: sid } } } }),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error.data?.message || j.error.message);
    return j.result;
}

let failed = 0;
const ok = m => console.log('    PASS  ' + m);
const no = m => { console.log('    FAIL  ' + m); failed++; };

// The customer must already be there — this script does not create it.
const want = await rpc('res.partner', 'search_read',
    [[['name', '=', CUSTOMER]]], { fields: ['id', 'name'] });
if (!want.length) { console.log(`FAIL no customer named "${CUSTOMER}"`); process.exit(1); }
const wantId = want[0].id;

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 950 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await new Promise(r => setTimeout(r, 1400));
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await new Promise(r => setTimeout(r, 1000));
    ok('the new rental contract form opened');

    const sel = '.m2o[data-model="res.partner"] input.m2o-input';
    if (!await page.$(sel)) { no('there is no Customer picker on this form'); throw new Error('no picker'); }

    // Type a fragment of the name, as a person does.
    const term = CUSTOMER.split(/\s+/).slice(-2, -1)[0] || CUSTOMER.slice(0, 8);
    await page.click(sel);
    await page.type(sel, term, { delay: 35 });
    await page.waitForFunction(
        (name) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                    .some(o => o.textContent.trim() === name),
        { timeout: 8000 }, CUSTOMER)
        .then(() => ok(`typing "${term}" offers the customer`))
        .catch(async () => {
            const got = await page.evaluate(() =>
                [...document.querySelectorAll('.m2o-pop .m2o-opt')].map(o => o.textContent.trim()));
            no(`typing "${term}" did not offer "${CUSTOMER}" — got ${JSON.stringify(got)}`);
        });

    await page.evaluate((name) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === name)
            || document.querySelector('.m2o-pop .m2o-opt');
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, CUSTOMER);
    await new Promise(r => setTimeout(r, 600));

    const shown = await page.$eval(sel, e => e.value);
    if (shown === CUSTOMER) ok('the box shows the chosen customer');
    else no(`after picking, the box shows "${shown}"`);

    // Fill the rest and save.
    await page.evaluate((ref) => {
        const set = (name, value) => {
            const el = document.querySelector(`[data-field="${name}"]`);
            if (!el) return;
            el.value = value;
            el.dispatchEvent(new Event('input',  { bubbles: true }));
            el.dispatchEvent(new Event('change', { bubbles: true }));
        };
        set('name', ref);
        set('date_start', '2026-11-01');
        set('billing_period', 'quarterly');
    }, REF);
    await new Promise(r => setTimeout(r, 400));
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await new Promise(r => setTimeout(r, 2500));
    await page.screenshot({ path: SHOT });

    // The whole point: did the picked customer reach the database?
    const saved = await rpc('rental.contract', 'search_read',
        [[['name', '=', REF]]],
        { fields: ['id', 'name', 'partner_id', 'billing_period', 'date_start'] });
    if (!saved.length) { no(`no contract named ${REF} was saved`); }
    else {
        const c = saved[0];
        const pid = Array.isArray(c.partner_id) ? c.partner_id[0] : c.partner_id;
        if (pid === wantId) ok(`saved against the right customer (partner_id=${pid})`);
        else no(`saved against partner_id=${pid}, expected ${wantId} — the picked value was LOST`);
        if (c.billing_period === 'quarterly') ok('the billing period chosen in the combobox was saved');
        else no(`billing_period saved as "${c.billing_period}", expected quarterly`);
        console.log('    contract: ' + JSON.stringify(c));
    }

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
    try { await page.screenshot({ path: SHOT }); } catch (_) {}
} finally {
    await browser.close();
}

console.log('screenshot: ' + SHOT);
process.exit(failed ? 1 : 0);
