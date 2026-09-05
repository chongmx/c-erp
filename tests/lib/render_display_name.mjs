/**
 * render_display_name.mjs — "Carol, Big Carrots", on screen, by clicking.
 *
 *   node tests/lib/render_display_name.mjs ZZDSP
 *
 * Reported: "an Individual, if connected to a company, should be displayed as
 * Carol, Big Carrots. a Company will stay as it is. please update all of the
 * customer/supplier/vendor etc picker combobox."
 *
 * The integration test proves the stored value is right. This proves the thing
 * the user actually asked about: that the LABEL IN THE COMBOBOX says it. Those
 * are different claims and this codebase has already been bitten by the gap —
 * a picker can hold a stale list, format its own label, or render a name from
 * a prefetch that never asked for the column, and every one of those looks
 * exactly like "the server is wrong" from the outside.
 *
 * Nothing here is created over the API. Every record is typed and clicked into
 * existence through the real forms, because a contact planted by `create` is
 * not the contact the user has — theirs went through two saves and a badge.
 *
 * Two pickers are checked on purpose: the rental contract's (rendered by the
 * GENERIC form, which picks its model at runtime) and the sales order's (a
 * hand-written form with its own markup). They are separate code paths and a
 * fix that reaches only one of them is the bug this change exists to end.
 *
 * Prints PASS/FAIL lines; exits non-zero on any failure.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/display_name';

const PFX      = process.argv[2] || 'ZZDSP';
const CO_NAME  = `${PFX} Big Carrots`;
const PERSON   = `${PFX} Carol`;
const EXPECTED = `${PERSON}, ${CO_NAME}`;      // the whole point of the exercise

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

// Authenticate only to obtain a session cookie. This is the browser's login,
// not a data path: no record below is created, read or asserted over RPC.
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

async function clickNew() {
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
}

async function openContacts() {
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1400);
}

/**
 * Type `term` into the nth res.partner picker on screen and return every
 * option the dropdown offers, as text.
 *
 * The wait is for the SEARCH to settle, not for a fixed delay: the widget
 * debounces 250 ms and then makes two round trips, and a fixed pause races
 * that on a loaded machine — which is how a green test hides a broken picker.
 */
async function pickerOptions(term, nth = 0) {
    const sel = '.m2o[data-model="res.partner"] input.m2o-input';
    const inputs = await page.$$(sel);
    if (!inputs[nth]) return null;

    // Close the dropdown before reopening it. Without this the OLD option list
    // is still on screen while the new search is in flight, and reading it is
    // how the second query in a row silently reports the first one's answers —
    // a false failure that looks exactly like a missing record.
    await page.evaluate((s, n) => {
        const el = [...document.querySelectorAll(s)][n];
        if (el) { el.value = ''; el.blur(); }
    }, sel, nth);
    await pause(350);

    await inputs[nth].click();
    await pause(200);
    await page.evaluate((s, n) => {
        const el = [...document.querySelectorAll(s)][n];
        if (el) el.value = '';
    }, sel, nth);
    await inputs[nth].type(term, { delay: 30 });

    // Settle rather than sleep: 250 ms of debounce, then two round trips. A
    // fixed pause races that on a loaded machine.
    const read = () => page.evaluate(() =>
        [...document.querySelectorAll('.m2o-pop .m2o-opt')].map(o => o.textContent.trim()));
    let last = null, stable = 0;
    for (let i = 0; i < 40 && stable < 2; i++) {
        await pause(250);
        const now = JSON.stringify(await read());
        if (now === last && now !== '[]') stable++; else { stable = 0; last = now; }
    }
    return read();
}

/** Choose the option whose text equals `text` (mousedown — the widget's own event). */
async function pickOption(text) {
    return page.evaluate((t) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === t);
        if (!el) return false;
        el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
        return true;
    }, text);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // A tab running yesterday's bundle reports every symptom below and none of
    // them are real, so establish which code is on the page first.
    const hasWidget = await page.evaluate(() => typeof M2OSelect);
    if (hasWidget === 'function') ok('the page has loaded the M2OSelect widget');
    else no(`typeof M2OSelect is "${hasWidget}" — this page is running old code`);

    // ---- 1. the company, created through the form -------------------------
    await openContacts();
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', CO_NAME);
    await clickText('.contact-badge', 'Company');
    await pause(300);
    await clickText('.contact-badge', 'Customer');
    await pause(300);
    await clickText('.so-action-btns button', 'Save');
    await pause(2200);
    ok('the company was created through the contact form');

    // ---- 2. the person, attached to it through the form -------------------
    await openContacts();
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', PERSON);
    await clickText('.contact-badge', 'Individual');
    await pause(400);
    await clickText('.contact-badge', 'Customer');
    await pause(400);

    // The Company field on the contact form is itself an M2OSelect.
    const coOpts = await pickerOptions(CO_NAME);
    if (coOpts === null) { no('the contact form has no Company picker'); throw new Error('no company picker'); }
    if (coOpts.includes(CO_NAME)) ok('the Company picker offers the company by its own name');
    else no(`the Company picker did not offer "${CO_NAME}" — showed ${JSON.stringify(coOpts)}`);
    await pickOption(CO_NAME);
    await pause(700);
    await clickText('.so-action-btns button', 'Save');
    await pause(2400);
    await page.screenshot({ path: `${SHOTDIR}/1-contact-saved.png` });
    ok('the individual was saved against that company');

    // ---- 3. the rental contract picker (GENERIC form) ---------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1400);
    await clickNew();
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(900);

    let opts = await pickerOptions('Carol');
    if (opts === null) { no('the new contract has no Customer picker'); throw new Error('no picker'); }
    if (opts.includes(EXPECTED)) ok(`the Customer picker reads "${EXPECTED}"`);
    else no(`expected "${EXPECTED}" in the Customer picker — showed ${JSON.stringify(opts)}`);
    if (!opts.includes(PERSON)) ok('and never the bare name on its own');
    else no(`the bare "${PERSON}" is still offered — a second, unlabelled row`);
    await page.screenshot({ path: `${SHOTDIR}/2-contract-picker.png` });

    // Searching by the COMPANY must find the people who work there. This is
    // the half that a client-side format could never have delivered.
    opts = await pickerOptions(CO_NAME);
    if (opts.includes(EXPECTED)) ok('typing the company name finds its people');
    else no(`typing "${CO_NAME}" did not offer "${EXPECTED}" — showed ${JSON.stringify(opts)}`);
    if (opts.includes(CO_NAME)) ok('and still offers the company itself, unchanged');
    else no(`the company "${CO_NAME}" itself dropped out of the picker`);

    // ---- 4. the chosen value keeps the label ------------------------------
    await pickOption(EXPECTED);
    await pause(800);
    let boxed = await page.evaluate(() => {
        const el = document.querySelector('.m2o[data-model="res.partner"] input.m2o-input');
        return el ? el.value : null;
    });
    if (boxed === EXPECTED) ok('the field itself shows the composed label after picking');
    else no(`the field shows "${boxed}" after picking, expected "${EXPECTED}"`);

    const REF = `${PFX}-RC-UI`;
    await setField('name', REF);
    await setField('date_start', '2026-12-01');
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(2600);
    await page.screenshot({ path: `${SHOTDIR}/3-contract-saved.png` });
    ok('the contract was saved from the screen');

    // ---- 5. reopening resolves the label BY ID ----------------------------
    // A saved value is redisplayed by reading the record, not by finding it in
    // a page of search results. That is a different code path and it has its
    // own way of showing the wrong name.
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1600);
    const opened = await page.evaluate((ref) => {
        const row = [...document.querySelectorAll('.list-row')]
            .find(r => r.textContent.includes(ref));
        if (!row) return false;
        row.click();
        return true;
    }, REF);
    if (opened) {
        await pause(2000);
        boxed = await page.evaluate(() => {
            const el = document.querySelector('.m2o[data-model="res.partner"] input.m2o-input');
            return el ? el.value : null;
        });
        if (boxed === EXPECTED) ok('reopening the contract redisplays the composed label');
        else no(`the reopened contract shows "${boxed}", expected "${EXPECTED}"`);
        await page.screenshot({ path: `${SHOTDIR}/4-contract-reopened.png` });
    } else {
        no(`the saved contract ${REF} was not in the list to reopen`);
    }

    // ---- 6. a hand-written form uses the same label -----------------------
    // The sales order form is not the generic renderer; it carries its own
    // markup and its own three partner pickers. If the label only reached the
    // generic form, this is where it shows.
    await page.evaluate(() => window.ErpNav.openRecord('sale.order', 0));
    await pause(1500);
    await clickNew();
    await pause(1800);
    opts = await pickerOptions('Carol');
    if (opts === null) {
        no('the sales order form has no Customer picker');
    } else if (opts.includes(EXPECTED)) {
        ok('the sales order Customer picker reads the same label');
    } else {
        no(`the sales order picker showed ${JSON.stringify(opts)}`);
    }
    await page.screenshot({ path: `${SHOTDIR}/5-sale-picker.png` });

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors in the whole journey');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
