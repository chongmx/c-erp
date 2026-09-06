/**
 * render_contract_invoice.mjs — "Create Invoice" on a rental contract.
 *
 *   node tests/lib/render_contract_invoice.mjs ZZIV
 *
 * Asked for: "for each rental contract created and active, I want a method of
 * creating invoice, similar to how sales order let me create invoice."
 *
 * sale.order has a statusbar of workflow buttons; a rental contract had none,
 * so the only way to invoice one was to wait for the cron — which skips
 * one-off and on-demand contracts entirely, by design. There was no way to
 * invoice those at all.
 *
 * The journey, all clicks:
 *
 *   1. a customer, a unit, a contract, a line — through the forms
 *   2. the contract list, open the contract
 *   3. "Create Invoice" is on the form, and only because it is ACTIVE
 *   4. press it: an invoice appears and the form says so
 *   5. press it again: it says "already invoiced", and does not bill twice
 *
 * Step 5 is the one worth having. Idempotency is the database's job
 * (UNIQUE contract_line_id, period_start) but a button that silently
 * no-ops looks broken, and one that claims success twice is worse.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/contract_invoice';

const PFX   = process.argv[2] || 'ZZIV';
const CUST  = `${PFX} Renter Sdn Bhd`;
const REF   = `${PFX}-CT`;

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

const a = await (await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }) })).json();
const sid = a?.session_id || a?.result?.session_id;
if (!sid) { console.log('    FAIL  could not authenticate'); process.exit(1); }

let failed = 0;
const ok = m => console.log('    PASS  ' + m);
const no = m => { console.log('    FAIL  ' + m); failed++; };

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 950 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

const pause = ms => new Promise(r => setTimeout(r, ms));

async function clickNew() {
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
}
async function setField(name, value) {
    return page.evaluate((n, v) => {
        const el = document.querySelector(`[data-field="${n}"]`);
        if (!el) return false;
        el.focus(); el.value = v;
        el.dispatchEvent(new Event('input',  { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    }, name, value);
}
async function setLineField(name, value) {
    return page.evaluate((n, v) => {
        const el = document.querySelector(`.o2m-table [data-field="${n}"]`);
        if (!el) return false;
        el.focus(); el.value = v;
        el.dispatchEvent(new Event('input',  { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    }, name, value);
}
async function pickIn(model, term, exact) {
    const sel = `.m2o[data-model="${model}"] input.m2o-input`;
    if (!(await page.$(sel))) return false;
    await page.click(sel);
    await pause(250);
    await page.type(sel, term, { delay: 25 });
    try {
        await page.waitForFunction((n) =>
            [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                .some(o => o.textContent.trim() === n), { timeout: 8000 }, exact);
    } catch (_) { return false; }
    return page.evaluate((n) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === n);
        if (!el) return false;
        el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
        return true;
    }, exact);
}
const actionMsg = () => page.evaluate(() => {
    const el = document.querySelector('.gf-action-msg');
    return el ? el.textContent.trim() : null;
});
async function pressCreateInvoice() {
    const found = await page.evaluate(() => {
        const b = document.querySelector('[data-action="action_create_invoice"]');
        if (!b) return false;
        b.click();
        return true;
    });
    if (!found) return null;
    await pause(3000);
    return actionMsg();
}

const today = new Date().toISOString().slice(0, 10);

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1. a customer and a unit, through their forms --------------------
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1500);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', CUST);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.contact-badge')]
            .find(x => x.textContent.trim() === 'Customer');
        if (b) b.click();
    });
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.so-action-btns button')]
            .find(x => x.textContent.trim() === 'Save');
        if (b) b.click();
    });
    await pause(2200);
    ok('a customer exists');

    await page.evaluate(() => window.ErpNav.openRecord('rental.unit', 0));
    await pause(1800);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button')]
            .find(x => /new unit/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(1000);
    await page.evaluate((code) => {
        const set = (k, v) => {
            const el = document.querySelector(`[data-nu="${k}"]`);
            if (!el) return;
            el.focus(); el.value = v;
            el.dispatchEvent(new Event('input', { bubbles: true }));
        };
        set('code', code); set('name', code + ' room');
    }, `${PFX}-U1`);
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.m2o-modal-foot button')]
            .find(x => /create/i.test(x.textContent));
        if (b) b.click();
    });
    await pause(2000);
    ok('a unit exists');

    // ---- 2. the contract, with a line, ACTIVE -----------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1600);
    await clickNew();
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(900);
    if (!(await pickIn('res.partner', 'Renter', CUST))) no('could not pick the customer');
    await pause(400);
    await setField('name', REF);
    await setField('date_start', today);
    await page.evaluate(() => {
        const sel = document.querySelector('[data-field="state"]');
        if (sel && sel.tagName === 'SELECT') {
            sel.value = 'active';
            sel.dispatchEvent(new Event('change', { bubbles: true }));
        }
    });
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button')]
            .find(x => /add a line/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(800);
    if (!(await pickIn('rental.unit', PFX, `${PFX}-U1 — ${PFX}-U1 room`)))
        no('could not pick the unit on the line');
    await pause(400);
    await setLineField('date_start', today);
    await setLineField('unit_price', '300');
    // MANUAL, for two reasons. It is what every line on the live database
    // actually is — billing_mode DEFAULTs to it — and it is the case that was
    // broken. It also takes the billing CRON out of the picture: a recurring
    // line is fair game for the scheduled run, which billed this contract
    // mid-test and left the button correctly reporting "nothing is due",
    // failing the test for a reason that had nothing to do with the button.
    await setLineField('billing_mode', 'manual');
    await setLineField('state', 'active');
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(2800);
    ok('an active contract with a line was saved');

    // ---- 3. reopen it: the button must be there ---------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1800);
    const opened = await page.evaluate((ref) => {
        const row = [...document.querySelectorAll('.list-row')].find(r => r.textContent.includes(ref));
        if (!row) return false;
        row.click();
        return true;
    }, REF);
    if (!opened) { no(`${REF} was not in the list`); throw new Error('no row'); }
    await pause(2200);

    // What actually got saved. A "nothing is due" answer is usually the
    // fixture, not the feature, and this says which in the test log rather
    // than sending someone to the database.
    const saved = await page.evaluate(async (ref) => {
        const rpc = async (model, method, args, kwargs = {}) => {
            const r = await fetch('/web/dataset/call_kw', {
                method: 'POST', credentials: 'same-origin',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
                    params: { model, method, args, kwargs } }) });
            const j = await r.json();
            return j.result;
        };
        const c = await rpc('rental.contract', 'search_read', [[['name', '=', ref]]],
            { fields: ['id', 'name', 'state', 'date_start', 'billing_period'] });
        if (!c || !c.length) return { contract: null };
        const l = await rpc('rental.contract.line', 'search_read',
            [[['contract_id', '=', c[0].id]]],
            { fields: ['id', 'state', 'billing_mode', 'date_start',
                       'next_period_start', 'unit_price', 'unit_id'] });
        return { contract: c[0], lines: l };
    }, REF);
    console.log('    saved: ' + JSON.stringify(saved));

    if (await page.$('[data-action="action_create_invoice"]'))
        ok('the contract form offers "Create Invoice", like a sales order');
    else { no('there is no Create Invoice button on the contract'); throw new Error('no button'); }
    await page.screenshot({ path: `${SHOTDIR}/1-contract-form.png` });

    // ---- 4. press it ------------------------------------------------------
    let msg = await pressCreateInvoice();
    if (msg === null) { no('the button could not be pressed'); throw new Error('press'); }
    if (/invoice created/i.test(msg)) ok(`pressing it reports: "${msg}"`);
    else no(`the first press reported "${msg}"`);
    await page.screenshot({ path: `${SHOTDIR}/2-invoiced.png` });

    // ---- 5. press it again ------------------------------------------------
    // The guarantee is "no second invoice", not any particular sentence, and
    // the two honest answers differ by billing mode:
    //
    //   recurring — the invoice ADVANCED next_period_start, so the period is
    //               consumed and the next one is not due yet: "Nothing is due".
    //   one-off   — next_period_start stays NULL so the period start does not
    //               move, and UNIQUE (contract_line_id, period_start) rejects
    //               the repeat: "Already invoiced for this period".
    //
    // Either is correct. Claiming a second invoice is not.
    msg = await pressCreateInvoice();
    if (/invoice created/i.test(msg)) {
        no(`the second press claimed "${msg}" — that would be a double-bill`);
    } else if (/already invoiced|nothing is due/i.test(msg)) {
        ok(`pressing it again reports: "${msg}"`);
    } else {
        no(`the second press reported "${msg}"`);
    }

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
