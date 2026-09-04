/**
 * render_lifecycle.mjs — one tenancy end to end, entirely by clicking.
 *
 *   node tests/lib/render_lifecycle.mjs ZZLIFE
 *
 * customer company -> a person who works there -> a unit -> a contract with
 * that unit on it -> run the billing -> the invoice -> pay it -> stop the
 * contract -> nothing bills again.
 *
 * NOTHING here is created over the API. Records the journey is about are made
 * through the screens, because an API test proves the data is right and says
 * nothing about whether the screen works: the reason this file exists is that
 * the old 14-rental-lifecycle created its contract with `call rental.contract
 * create` and never opened the combobox, so "I cannot select this company" was
 * reported three times while the suite stayed green.
 *
 * The RPC helper below is used ONLY to read state back for assertions, never
 * to make something happen.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/lifecycle';

const PFX  = process.argv[2] || 'ZZLIFE';
const CO   = `${PFX} Sunrise Traders Sdn Bhd`;
const WHO  = `${PFX} Siti Rahman`;
const UNIT = `${PFX}-A1`;
const REF  = `${PFX}-RC-001`;

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

const auth = await (await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }) })).json();
const sid = auth?.session_id || auth?.result?.session_id;
if (!sid) { console.log('FAIL could not authenticate'); process.exit(1); }

/** READ-ONLY. Assertions only — never used to create or change anything. */
async function read(model, method, args, kwargs = {}) {
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
const pause = ms => new Promise(r => setTimeout(r, ms));

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 1000 });
const errs = [];
// Keep the first stack frame: "v2 is not a function" from a minified bundle is
// unlocatable without it, and the error is reported at the END of a long
// journey, so there is no telling which step produced it.
page.on('pageerror', e => errs.push('pageerror: ' + e.message +
    (e.stack ? ' @ ' + String(e.stack).split('\n').slice(1, 3).join(' | ').trim() : '')));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
let step = '(start)';
const at = s => { step = s; errs.push('--step: ' + s); };

async function clickText(sel, text) {
    return page.evaluate((s, t) => {
        const el = [...document.querySelectorAll(s)].find(x => x.textContent.trim() === t);
        if (!el) return false;
        el.click(); return true;
    }, sel, text);
}
async function setField(name, value, root = '') {
    return page.evaluate((n, v, r) => {
        const el = document.querySelector(`${r}[data-field="${n}"]`);
        if (!el) return false;
        el.focus(); el.value = v;
        el.dispatchEvent(new Event('input',  { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    }, name, value, root);
}
async function clickNew() {
    return page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) { b.click(); return true; } return false;
    });
}
/** Type into an M2OSelect and choose the row whose text equals `want`. */
async function pickM2O(model, term, want) {
    const sel = `.m2o[data-model="${model}"] input.m2o-input`;
    await page.click(sel);
    await page.type(sel, term, { delay: 30 });
    try {
        await page.waitForFunction(
            (w) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                     .some(o => o.textContent.trim() === w), { timeout: 8000 }, want);
    } catch (_) {
        const got = await page.evaluate(() =>
            [...document.querySelectorAll('.m2o-pop .m2o-opt')].map(o => o.textContent.trim()));
        no(`picker for ${model} did not offer "${want}" — showed ${JSON.stringify(got)}`);
        return false;
    }
    await page.evaluate((w) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === w);
        el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, want);
    await pause(500);
    return true;
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1. the customer company ------------------------------------------
    at('company');
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1300);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', CO);
    await clickText('.contact-badge', 'Company');
    await clickText('.contact-badge', 'Customer');
    await setField('city', 'Penang');
    await clickText('.so-action-btns button', 'Save');
    await pause(2000);
    let rows = await read('res.partner', 'search_read', [[['name', '=', CO]]],
        { fields: ['id', 'is_company', 'customer_rank', 'city'] });
    if (rows.length === 1) ok('the customer company was created by clicking');
    else { no(`expected 1 company "${CO}", found ${rows.length}`); throw new Error('setup'); }
    const CO_ID = rows[0].id;
    if (rows[0].is_company && rows[0].customer_rank > 0) ok('labelled Company and Customer');
    else no(`is_company=${rows[0].is_company} customer_rank=${rows[0].customer_rank}`);

    // ---- 2. a person who works there --------------------------------------
    at('contact');
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1300);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', WHO);
    await pause(300);
    if (await pickM2O('res.partner', 'Sunrise', CO)) ok('her company is chosen from the picker');
    await setField('job_position', 'Office Manager');
    await clickText('.so-action-btns button', 'Save');
    await pause(2000);
    const who = await read('res.partner', 'search_read', [[['name', '=', WHO]]],
        { fields: ['id', 'parent_id', 'commercial_company_name', 'city'] });
    if (who.length === 1) ok('the contact was created under the company');
    else { no(`expected 1 contact "${WHO}", found ${who.length}`); throw new Error('setup'); }
    const pid = Array.isArray(who[0].parent_id) ? who[0].parent_id[0] : who[0].parent_id;
    if (pid === CO_ID) ok('and is linked to it');
    else no(`parent_id=${pid}, expected ${CO_ID}`);
    if (who[0].commercial_company_name === CO) ok('her row shows the company name');
    else no(`Company cell reads "${who[0].commercial_company_name}"`);
    if (who[0].city === 'Penang') ok('and she inherited the company address');
    else no(`city is "${who[0].city}", expected the inherited Penang`);

    // ---- 3. a unit to let --------------------------------------------------
    at('unit');
    await page.evaluate(() => window.ErpNav.openRecord('rental.unit', 0));
    await pause(1600);
    if (await clickText('.rental-filters button', '+ New unit')) ok('the Units screen offers New unit');
    else no('no "New unit" button on the Units screen');
    await page.waitForSelector('[data-nu="code"]', { timeout: 8000 });
    await page.evaluate((code) => {
        const set = (k, v) => {
            const el = document.querySelector(`[data-nu="${k}"]`);
            el.value = v; el.dispatchEvent(new Event('input', { bubbles: true }));
        };
        set('code', code); set('name', 'Unit A1');
    }, UNIT);
    await pause(200);
    await clickText('.m2o-modal-foot button', 'Create');
    await pause(2000);
    const units = await read('rental.unit', 'search_read', [[['code', '=', UNIT]]],
        { fields: ['id', 'code', 'state'] });
    if (units.length === 1) ok('the unit was created by clicking');
    else { no(`expected 1 unit "${UNIT}", found ${units.length}`); throw new Error('setup'); }
    const UNIT_ID = units[0].id;

    // ---- 4. the contract, with that unit on it -----------------------------
    at('contract');
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1400);
    await clickNew();
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(900);
    if (await pickM2O('res.partner', 'Sunrise', CO)) ok('the customer is chosen on the contract');
    await setField('name', REF);
    await setField('date_start', '2026-03-01');
    await setField('billing_period', 'monthly');
    await setField('billing_lead_days', '7');
    await setField('state', 'active');

    // The lines grid — the thing that did not exist before this change.
    if (await page.$('[data-add-o2m]')) ok('the contract form has a Units section');
    else no('the contract form still has no Units section');
    await page.evaluate(() => document.querySelector('[data-add-o2m]').click());
    await pause(700);
    // The unit picker labels a unit "CODE — Name", because a unit is known by
    // its code; that label is what the dropdown row reads.
    if (await pickM2O('rental.unit', PFX, `${UNIT} — Unit A1`)) ok('the unit is chosen on the line');
    await page.evaluate(() => {
        const setLine = (field, v) => {
            const el = document.querySelector(`.o2m-table [data-field="${field}"]`);
            if (!el) return;
            el.focus(); el.value = v;
            el.dispatchEvent(new Event('input',  { bubbles: true }));
            el.dispatchEvent(new Event('change', { bubbles: true }));
        };
        setLine('date_start', '2026-03-01');
        setLine('unit_price', '1200');
        setLine('billing_mode', 'recurring');
        setLine('state', 'active');
    });
    await pause(400);
    await page.screenshot({ path: `${SHOTDIR}/contract-filled.png` });
    await clickText('.gf-actions button', 'Create');
    await pause(2600);

    const rc = await read('rental.contract', 'search_read', [[['name', '=', REF]]],
        { fields: ['id', 'partner_id', 'billing_period', 'state'] });
    if (rc.length === 1) ok('the contract was saved from the screen');
    else { no(`expected 1 contract "${REF}", found ${rc.length}`); throw new Error('contract'); }
    const RC_ID = rc[0].id;
    const rcp = Array.isArray(rc[0].partner_id) ? rc[0].partner_id[0] : rc[0].partner_id;
    if (rcp === CO_ID) ok('against the company, not the individual');
    else no(`contract partner_id=${rcp}, expected ${CO_ID}`);

    const lines = await read('rental.contract.line', 'search_read',
        [[['contract_id', '=', RC_ID]]],
        { fields: ['id', 'unit_id', 'partner_id', 'unit_price', 'state', 'billing_mode'] });
    if (lines.length === 1) ok('the unit is on the contract as a line');
    else { no(`expected 1 line on the contract, found ${lines.length}`); throw new Error('line'); }
    const lup = Array.isArray(lines[0].unit_id) ? lines[0].unit_id[0] : lines[0].unit_id;
    if (lup === UNIT_ID) ok('and it is the right unit');
    else no(`line unit_id=${lup}, expected ${UNIT_ID}`);
    const lpp = Array.isArray(lines[0].partner_id) ? lines[0].partner_id[0] : lines[0].partner_id;
    if (lpp === CO_ID) ok('the line inherited the contract customer');
    else no(`line partner_id=${lpp}, expected ${CO_ID} — the trigger did not fire`);

    // ---- 5. run the billing, from the dashboard ----------------------------
    at('billing');
    await page.evaluate(() => window.ErpNav.openRecord('rental.dashboard', 0));
    await pause(2200);
    const dateInput = await page.$('.rental-filters input[type="date"]');
    if (dateInput) ok('the dashboard offers a billing date');
    else no('no billing date on the dashboard');
    await page.evaluate(() => {
        const el = document.querySelector('.rental-filters input[type="date"]');
        el.value = '2026-02-22';
        el.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await pause(300);
    if (await clickText('.rental-filters button', 'Run billing now')) ok('billing is run from the screen');
    else no('no "Run billing now" button');
    await page.waitForFunction(
        () => { const m = document.querySelector('.dash-bill-msg');
                return m && m.textContent.trim().length > 0; }, { timeout: 30000 })
        .catch(() => no('the billing run never reported a result'));
    console.log('    billing said: ' + await page.evaluate(() =>
        (document.querySelector('.dash-bill-msg') || {}).textContent || '(nothing)'));
    await page.screenshot({ path: `${SHOTDIR}/billing-run.png` });

    let invs = await read('account.move', 'search_read',
        [[['partner_id', '=', CO_ID], ['move_type', '=', 'out_invoice']]],
        { fields: ['id', 'amount_total', 'amount_residual', 'payment_state', 'invoice_date'] });
    if (invs.length === 1) ok('one invoice was raised for the company');
    else { no(`expected 1 invoice, found ${invs.length}`); throw new Error('invoice'); }
    const INV = invs[0].id;
    if (String(invs[0].invoice_date || '').startsWith('2026-02')) ok('billed in advance of the March period');
    else no(`invoice dated ${invs[0].invoice_date}, expected February`);

    // ---- 6. pay it, from the invoice screen --------------------------------
    at('payment');
    await page.evaluate((id) => window.ErpNav.openRecord('account.move', id), INV);
    await pause(2200);
    await page.screenshot({ path: `${SHOTDIR}/invoice.png` });
    const paid = await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /register payment|pay/i.test((x.textContent || '').trim()));
        if (!b) return false;
        b.click(); return true;
    });
    if (paid) ok('the invoice screen offers Register Payment');
    else no('no Register Payment button on the invoice');
    await page.waitForSelector('.pay-dialog', { timeout: 8000 })
        .then(() => ok('the Register Payment dialog opens'))
        .catch(() => no('the payment dialog never opened'));
    // The dialog opens FIRST and fetches its bank/cash journals after, so wait
    // for them rather than for a fixed moment — a fixed pause raced the fetch
    // and the dialog then refused with "Please select a journal.", which is
    // correct of it and looked like a product bug.
    await page.waitForFunction(
        () => [...document.querySelectorAll('.pay-dialog select option')]
                .some(o => /^\d+$/.test(o.value)), { timeout: 10000 })
        .catch(() => no('the payment dialog never loaded a bank or cash journal'));
    const gotJournal = await page.evaluate(() => {
        const sel = [...document.querySelectorAll('.pay-dialog select')]
            .find(s => [...s.options].some(o => /^\d+$/.test(o.value)));
        if (!sel) return null;
        const opt = [...sel.options].find(o => /^\d+$/.test(o.value));
        sel.value = opt.value;
        sel.dispatchEvent(new Event('change', { bubbles: true }));
        return opt.textContent.trim();
    });
    if (gotJournal) ok(`a payment journal is chosen (${gotJournal})`);
    else no('the payment dialog offered no journal to choose');
    await pause(400);

    if (await clickText('.pay-dialog-actions button', 'Validate')) ok('the payment is validated');
    else no('no Validate button in the payment dialog');
    await pause(3000);
    const payErr = await page.evaluate(() =>
        (document.querySelector('.pay-dialog-error') || {}).textContent || '');
    if (payErr.trim()) no('the payment dialog reported: ' + payErr.trim());
    await page.screenshot({ path: `${SHOTDIR}/paid.png` });

    invs = await read('account.move', 'read', [[INV],
        ['payment_state', 'amount_residual']]);
    if (['paid', 'in_payment'].includes(invs[0].payment_state)) ok(`the invoice reads as paid (${invs[0].payment_state})`);
    else no(`payment_state is "${invs[0].payment_state}" after paying from the screen`);
    if (Number(invs[0].amount_residual) === 0) ok('nothing is left outstanding');
    else no(`amount_residual is ${invs[0].amount_residual}`);

    // ---- 7. stop the contract ---------------------------------------------
    at('closing');
    await page.evaluate((id) => window.ErpNav.openRecord('rental.contract', id), RC_ID);
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(1200);
    await page.evaluate(() => {
        const setLine = (field, v) => {
            const el = document.querySelector(`.o2m-table [data-field="${field}"]`);
            if (!el) return;
            el.focus(); el.value = v;
            el.dispatchEvent(new Event('input',  { bubbles: true }));
            el.dispatchEvent(new Event('change', { bubbles: true }));
        };
        setLine('date_end', '2026-03-31');
        setLine('state', 'ended');
    });
    await setField('state', 'closed');
    await pause(300);
    await clickText('.gf-actions button', 'Save');
    await pause(2400);
    await page.screenshot({ path: `${SHOTDIR}/closed.png` });

    const after = await read('rental.contract', 'read', [[RC_ID], ['state']]);
    if (after[0].state === 'closed') ok('the contract is closed');
    else no(`contract state is "${after[0].state}"`);
    const lafter = await read('rental.contract.line', 'read', [[lines[0].id], ['state', 'date_end']]);
    if (lafter[0].state === 'ended') ok('the line is ended');
    else no(`line state is "${lafter[0].state}"`);
    const uafter = await read('rental.unit', 'read', [[UNIT_ID], ['state']]);
    if (uafter[0].state === 'available') ok('the unit is released back to available');
    else no(`unit state is "${uafter[0].state}", expected available`);

    // ---- 8. a stopped contract never bills again ---------------------------
    at('recheck');
    await page.evaluate(() => window.ErpNav.openRecord('rental.dashboard', 0));
    await pause(2200);
    for (const d of ['2026-04-15', '2026-05-15', '2026-06-15']) {
        await page.evaluate((v) => {
            const el = document.querySelector('.rental-filters input[type="date"]');
            el.value = v; el.dispatchEvent(new Event('change', { bubbles: true }));
        }, d);
        await pause(300);
        await clickText('.rental-filters button', 'Run billing now');
        await pause(2500);
    }
    const finalInvs = await read('account.move', 'search_count',
        [[['partner_id', '=', CO_ID], ['move_type', '=', 'out_invoice']]]);
    if (finalInvs === 1) ok('three more billing runs raised NO further invoice');
    else no(`${finalInvs} invoices after closing — a stopped contract kept billing`);

    const real = errs.filter(e => !e.startsWith('--step:'));
    if (real.length) {
        // Print the step markers around the first error so it can be placed.
        const i = errs.indexOf(real[0]);
        const near = errs.slice(Math.max(0, i - 1), i + 1).filter(e => e.startsWith('--step:'));
        no('browser errors' + (near.length ? ' (after ' + near.join(', ') + ')' : '') +
           ': ' + real.slice(0, 2).join(' | '));
    } else ok('no browser console errors in the whole journey');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
    try { await page.screenshot({ path: `${SHOTDIR}/failure.png` }); } catch (_) {}
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
