/**
 * render_list_labels.mjs — a list must show names, not ids.
 *
 *   node tests/lib/render_list_labels.mjs ZZLL
 *
 * Reported: "rental -> operation -> contract shows a list of contract. however,
 * the customer column is showing a number, not the customer name."
 *
 * read/search_read project COLUMNS (rowsToJson_), not the [id, name] pairs the
 * reference ERP returns, so a many2one arrives at the browser as a bare
 * integer. The generic ListView rendered it raw, and a customer appeared on
 * screen as "1766" — a number nobody can act on, in the one column that says
 * whose contract it is.
 *
 * The label wanted is "name, company" for a person at a company and the plain
 * name for an individual — which is exactly res_partner.display_name, so the
 * list asks for that and falls back to `name` for models without one.
 *
 * Both cases are built here by clicking, because they are the two the report
 * distinguishes:
 *
 *   a company, a person inside it   -> "Person, Company"
 *   an individual with no company   -> "Individual"
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/list_labels';

const PFX     = process.argv[2] || 'ZZLL';
const COMPANY = `${PFX} Orchard Bhd`;
const PERSON  = `${PFX} Mina`;
const LONE    = `${PFX} Walkin`;
const EXPECT  = `${PERSON}, ${COMPANY}`;

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
async function clickBadge(text) {
    return page.evaluate((t) => {
        const el = [...document.querySelectorAll('.contact-badge')]
            .find(x => x.textContent.trim() === t);
        if (!el) return false;
        el.click();
        return true;
    }, text);
}
async function saveContact() {
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.so-action-btns button')]
            .find(x => x.textContent.trim() === 'Save');
        if (b) b.click();
    });
    await pause(2200);
}
/** Choose a partner in the first res.partner picker on screen. */
async function pickPartner(name) {
    const sel = '.m2o[data-model="res.partner"] input.m2o-input';
    if (!(await page.$(sel))) return false;
    await page.click(sel);
    await pause(250);
    await page.type(sel, name.split(' ')[1] || name, { delay: 25 });
    try {
        await page.waitForFunction((n) =>
            [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                .some(o => o.textContent.trim() === n), { timeout: 8000 }, name);
    } catch (_) { return false; }
    return page.evaluate((n) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === n);
        if (!el) return false;
        el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
        return true;
    }, name);
}
async function makeContract(ref, customerLabel) {
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1600);
    await clickNew();
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(900);
    if (!(await pickPartner(customerLabel))) return `could not pick "${customerLabel}"`;
    await pause(500);
    await setField('name', ref);
    await setField('date_start', '2026-11-01');
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(2600);
    return null;
}
/** The Customer cell of a contract row, by reference. */
async function customerCell(ref) {
    return page.evaluate((r) => {
        const heads = [...document.querySelectorAll('.list-table thead th')]
            .map(t => t.textContent.trim());
        const col = heads.indexOf('Customer');
        if (col < 0) return { err: 'no Customer column: ' + JSON.stringify(heads) };
        const row = [...document.querySelectorAll('.list-table tbody tr')]
            .find(x => x.textContent.includes(r));
        if (!row) return { err: 'no row for ' + r };
        return { text: (row.querySelectorAll('td')[col] || {}).textContent.trim() };
    }, ref);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- the two kinds of customer, both through the contact form --------
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1500);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', COMPANY);
    await clickBadge('Company'); await pause(250);
    await clickBadge('Customer'); await pause(250);
    await saveContact();
    ok('a company customer exists');

    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1500);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', PERSON);
    await clickBadge('Individual'); await pause(250);
    await clickBadge('Customer');   await pause(250);
    if (!(await pickPartner(COMPANY))) no('could not set the person\'s company');
    await pause(500);
    await saveContact();
    ok('a person inside that company exists');

    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1500);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', LONE);
    await clickBadge('Individual'); await pause(250);
    await clickBadge('Customer');   await pause(250);
    await saveContact();
    ok('and an individual with no company');

    // ---- a contract for each ---------------------------------------------
    let err = await makeContract(`${PFX}-C1`, EXPECT);
    if (err) { no(`contract for the person: ${err}`); throw new Error('c1'); }
    err = await makeContract(`${PFX}-C2`, LONE);
    if (err) { no(`contract for the individual: ${err}`); throw new Error('c2'); }
    ok('a contract for each of them');

    // ---- the list ---------------------------------------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(2600);
    await page.screenshot({ path: `${SHOTDIR}/1-contract-list.png` });

    let cell = await customerCell(`${PFX}-C1`);
    if (cell.err) {
        no(cell.err);
    } else if (/^#?\d+$/.test(cell.text)) {
        no(`the Customer column still shows an id: "${cell.text}"`);
    } else if (cell.text === EXPECT) {
        ok(`a person at a company reads "${EXPECT}"`);
    } else {
        no(`expected "${EXPECT}", the cell reads "${cell.text}"`);
    }

    cell = await customerCell(`${PFX}-C2`);
    if (cell.err) {
        no(cell.err);
    } else if (cell.text === LONE) {
        ok(`an individual with no company reads just "${LONE}" — no stray comma`);
    } else {
        no(`expected "${LONE}", the cell reads "${cell.text}"`);
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
