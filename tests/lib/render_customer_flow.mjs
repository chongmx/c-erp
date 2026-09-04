/**
 * render_customer_flow.mjs — the reported journey, driven entirely by clicking.
 *
 *   node tests/lib/render_customer_flow.mjs ZZCPK
 *
 * Reported three times, always the same sentence: "I created the contact, made
 * it a customer, and it still is not a valid option on a new rental contract."
 * Every API-level test was green throughout, because none of them created a
 * contact the way a person does — through the Contacts form, with the type
 * badges, saving in between.
 *
 * So this does exactly that, with NO records planted over the API:
 *
 *   A. new contact, NO labels at all      -> Save
 *   B. reopen it, click Customer          -> Save
 *   C. new rental contract: it must be findable in the Customer picker
 *   D. a second contact, Individual + Customer in one go
 *   E. that one must be findable too, and the contract must SAVE with it
 *
 * The prefix argument namespaces every record so the shell test can clean up.
 * Prints PASS/FAIL lines; exits non-zero on any failure.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/customer_flow';

const PFX = process.argv[2] || 'ZZCPK';
const CO_NAME  = `${PFX} Big Carrot Bhd`;
const IND_NAME = `${PFX} Ind Customer`;

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

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

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 950 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

const pause = ms => new Promise(r => setTimeout(r, ms));

/** Click the first element matching `sel` whose trimmed text equals `text`. */
async function clickText(sel, text) {
    return page.evaluate((s, t) => {
        const el = [...document.querySelectorAll(s)].find(x => x.textContent.trim() === t);
        if (!el) return false;
        el.click();
        return true;
    }, sel, text);
}

/** Type into a [data-field] input the way a user does. */
async function setField(name, value) {
    return page.evaluate((n, v) => {
        const el = document.querySelector(`[data-field="${n}"]`);
        if (!el) return false;
        el.focus();
        el.value = v;
        el.dispatchEvent(new Event('input',  { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    }, name, value);
}

async function openContacts() {
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1400);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // The check the user can run themselves in DevTools.
    const hasWidget = await page.evaluate(() => typeof M2OSelect);
    if (hasWidget === 'function') ok('the page has loaded the M2OSelect widget');
    else no(`typeof M2OSelect is "${hasWidget}" — this page is running old code`);

    // ---- A. a contact with NO labels, created through the form ------------
    await openContacts();
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    ok('the contact form opened');

    await setField('name', CO_NAME);
    await clickText('.contact-badge', 'Company');     // a company, but NOT a customer yet
    await pause(300);
    await clickText('.so-action-btns button', 'Save');
    await pause(2000);
    await page.screenshot({ path: `${SHOTDIR}/a-unlabelled.png` });

    let rows = await rpc('res.partner', 'search_read',
        [['|', ['active', '=', true], ['active', '=', false], ['name', '=', CO_NAME]]],
        { fields: ['id', 'name', 'is_company', 'customer_rank', 'active'] });
    if (rows.length === 1) ok('it saved with no customer label');
    else { no(`expected 1 contact named "${CO_NAME}", found ${rows.length}`); throw new Error('setup'); }
    const CO_ID = rows[0].id;
    if (rows[0].customer_rank === 0) ok('and it is not a customer yet (customer_rank 0)');
    else no(`customer_rank is ${rows[0].customer_rank}, expected 0 at this point`);

    // ---- B. reopen it and label it a Customer -----------------------------
    await page.evaluate((id) => window.ErpNav.openRecord('res.partner', id), CO_ID);
    await page.waitForSelector('.contact-badge', { timeout: 12000 });
    await pause(1000);
    if (await clickText('.contact-badge', 'Customer')) ok('the Customer badge is clickable');
    else no('no Customer badge on the contact form');
    await pause(300);
    await clickText('.so-action-btns button', 'Save');
    await pause(2000);
    await page.screenshot({ path: `${SHOTDIR}/b-labelled.png` });

    rows = await rpc('res.partner', 'read', [[CO_ID], ['customer_rank', 'active', 'is_company']]);
    if (rows[0].customer_rank > 0) ok('the Customer label persisted through Save');
    else no(`customer_rank is still ${rows[0].customer_rank} after clicking Customer and saving`);
    if (rows[0].active !== false) ok('and the contact is still active');
    else no('saving the Customer label ARCHIVED the contact');

    // ---- C. it must be selectable on a new rental contract ----------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1400);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(1000);

    const sel = '.m2o[data-model="res.partner"] input.m2o-input';
    if (await page.$(sel)) ok('the new contract has a Customer picker');
    else { no('the new contract form has NO Customer picker'); throw new Error('no picker'); }

    await page.click(sel);
    await page.type(sel, 'Big Carrot', { delay: 35 });
    await page.waitForFunction((n) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .some(o => o.textContent.trim() === n), { timeout: 8000 }, CO_NAME)
        .then(() => ok('the newly-labelled customer IS offered in the picker'))
        .catch(async () => {
            const got = await page.evaluate(() =>
                [...document.querySelectorAll('.m2o-pop .m2o-opt')].map(o => o.textContent.trim()));
            no(`the customer was NOT offered — picker showed ${JSON.stringify(got)}`);
        });
    await page.screenshot({ path: `${SHOTDIR}/c-picker.png` });

    // ---- D. a second contact: Individual AND Customer together ------------
    await openContacts();
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', IND_NAME);
    await clickText('.contact-badge', 'Save');   // no-op; keeps the shape obvious
    await clickText('.so-action-btns button', 'Save');
    await pause(2000);

    let ind = await rpc('res.partner', 'search_read', [[['name', '=', IND_NAME]]],
        { fields: ['id', 'name', 'is_individual', 'customer_rank'] });
    if (ind.length === 1) ok('the second contact saved with no labels');
    else { no(`expected 1 contact named "${IND_NAME}", found ${ind.length}`); throw new Error('setup2'); }
    const IND_ID = ind[0].id;

    await page.evaluate((id) => window.ErpNav.openRecord('res.partner', id), IND_ID);
    await page.waitForSelector('.contact-badge', { timeout: 12000 });
    await pause(1000);
    await clickText('.contact-badge', 'Individual');
    await pause(200);
    await clickText('.contact-badge', 'Customer');
    await pause(200);
    await clickText('.so-action-btns button', 'Save');
    await pause(2000);
    await page.screenshot({ path: `${SHOTDIR}/d-individual.png` });

    ind = await rpc('res.partner', 'read', [[IND_ID], ['is_individual', 'customer_rank', 'active']]);
    if (ind[0].is_individual) ok('the Individual label persisted');
    else no('the Individual label did NOT persist through Save');
    if (ind[0].customer_rank > 0) ok('and the Customer label persisted with it');
    else no('the Customer label did not persist when set together with Individual');

    // ---- E. pick that one, and SAVE the contract --------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1400);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(900);

    await page.click(sel);
    await page.type(sel, 'Ind Customer', { delay: 35 });
    await page.waitForFunction((n) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .some(o => o.textContent.trim() === n), { timeout: 8000 }, IND_NAME)
        .then(() => ok('the individual customer is offered too'))
        .catch(async () => {
            const got = await page.evaluate(() =>
                [...document.querySelectorAll('.m2o-pop .m2o-opt')].map(o => o.textContent.trim()));
            no(`the individual was NOT offered — picker showed ${JSON.stringify(got)}`);
        });

    await page.evaluate((n) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === n) || document.querySelector('.m2o-pop .m2o-opt');
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, IND_NAME);
    await pause(600);

    const REF = `${PFX}-RC-UI`;
    await setField('name', REF);
    await setField('date_start', '2026-12-01');
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(2500);
    await page.screenshot({ path: `${SHOTDIR}/e-saved.png` });

    const saved = await rpc('rental.contract', 'search_read', [[['name', '=', REF]]],
        { fields: ['id', 'name', 'partner_id'] });
    if (!saved.length) no(`no contract named ${REF} was saved from the screen`);
    else {
        const pid = Array.isArray(saved[0].partner_id) ? saved[0].partner_id[0] : saved[0].partner_id;
        if (pid === IND_ID) ok(`the contract saved against the picked customer (partner_id=${pid})`);
        else no(`contract saved against partner_id=${pid}, expected ${IND_ID}`);
    }

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors in the whole journey');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
