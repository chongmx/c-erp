/**
 * render_unit_edit.mjs — create a unit, click it, change it, by clicking.
 *
 *   node tests/lib/render_unit_edit.mjs ZZUE
 *
 * Reported: "after I create a unit, Rental → Operations → Units has a unit. I
 * want to be able to click and edit the unit again. maybe change its name and
 * zone."
 *
 * Clicking a unit called openUnit(), which called an optional prop nobody
 * supplies and then returned. So the click did nothing at all, on the ONLY
 * screen that lists units — a typo in a code, or a locker moved to another
 * zone, could not be corrected anywhere in the product.
 *
 * The journey, all clicks:
 *
 *   1. create a unit through "+ New unit"
 *   2. click it in the grid — the dialog must open, FILLED with its values
 *   3. change the name and the zone, save
 *   4. the grid reflects both without a reload
 *   5. reopen it: the new values are what comes back, not the old ones
 *
 * Step 2 is the one worth stating: an edit dialog that opens blank is worse
 * than none, because saving it wipes the fields the user did not retype.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/unit_edit';

const PFX   = process.argv[2] || 'ZZUE';
const CODE  = `${PFX}-E1`;
const NAME0 = `${PFX} Before`;
const ZONE0 = `${PFX} Old Wing`;
const NAME1 = `${PFX} After`;
const ZONE1 = `${PFX} New Wing`;

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

/** Type into a [data-nu] control in the unit dialog. */
async function setDlg(field, value) {
    return page.evaluate((f, v) => {
        const el = document.querySelector(`[data-nu="${f}"]`);
        if (!el) return false;
        el.focus(); el.value = v;
        el.dispatchEvent(new Event('input',  { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    }, field, value);
}

/** Read every [data-nu] control back, so "opens filled" is checkable. */
async function readDlg() {
    return page.evaluate(() => {
        const out = {};
        for (const el of document.querySelectorAll('[data-nu]')) out[el.dataset.nu] = el.value;
        const t = document.querySelector('.m2o[data-model="rental.unit.type"] input.m2o-input');
        if (t) out.__type = t.value;
        const h = document.querySelector('.m2o-modal-head span');
        if (h) out.__title = h.textContent.trim();
        return out;
    });
}

async function submitDlg() {
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.m2o-modal-foot button')]
            .find(x => /^(create|save)$/i.test(x.textContent.trim()));
        if (b) b.click();
    });
    await pause(2400);
    return page.evaluate(() => {
        const e = document.querySelector('.ru-dlg .error');
        return e ? e.textContent.trim() : null;
    });
}

/** Click the unit's row in the table view. */
async function clickUnitRow(code) {
    return page.evaluate((c) => {
        const row = [...document.querySelectorAll('.rental-table tbody tr')]
            .find(r => (r.querySelector('td') || {}).textContent === c);
        if (!row) return false;
        row.click();
        return true;
    }, code);
}

/** The row's cells, so the grid's own view of the unit can be checked. */
async function rowCells(code) {
    return page.evaluate((c) => {
        const row = [...document.querySelectorAll('.rental-table tbody tr')]
            .find(r => (r.querySelector('td') || {}).textContent === c);
        return row ? [...row.querySelectorAll('td')].map(td => td.textContent.trim()) : null;
    }, code);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    await page.evaluate(() => window.ErpNav.openRecord('rental.unit', 0));
    await pause(2000);

    // ---- 1. create it -----------------------------------------------------
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button')]
            .find(x => /new unit/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(1100);
    if (await page.$('[data-nu="zone"]'))
        ok('the New unit dialog offers Zone as well as Code and Name');
    else
        no('the dialog has no Zone field — a unit could be filed nowhere');

    await setDlg('code', CODE);
    await setDlg('name', NAME0);
    await setDlg('zone', ZONE0);
    await pause(300);
    let err = await submitDlg();
    if (err) { no(`creating the unit failed: "${err}"`); throw new Error('create'); }
    ok(`created ${CODE} in zone "${ZONE0}"`);

    // ---- 2. click it — the dialog must open, and be FILLED ----------------
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button')]
            .find(x => /table view/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(900);

    if (await clickUnitRow(CODE)) ok('the unit can be clicked in the grid');
    else { no(`${CODE} is not in the table to click`); throw new Error('no row'); }
    await pause(1200);

    if (await page.$('.ru-dlg')) ok('clicking it opens the unit for editing');
    else { no('clicking the unit did nothing — the dialog never opened'); throw new Error('no dialog'); }

    let dlg = await readDlg();
    // An edit dialog that opens blank is worse than none: saving it would wipe
    // every field the user did not retype.
    if (dlg.code === CODE)  ok("it opens filled with the unit's code");
    else no(`the Code box reads "${dlg.code}", expected "${CODE}"`);
    if (dlg.name === NAME0) ok('and its name');
    else no(`the Name box reads "${dlg.name}", expected "${NAME0}"`);
    if (dlg.zone === ZONE0) ok('and its zone');
    else no(`the Zone box reads "${dlg.zone}", expected "${ZONE0}"`);
    if ((dlg.__title || '').includes(CODE)) ok(`the title names the unit ("${dlg.__title}")`);
    else no(`the dialog title reads "${dlg.__title}"`);
    await page.screenshot({ path: `${SHOTDIR}/1-edit-open.png` });

    // ---- 3. change the name and the zone ----------------------------------
    await setDlg('name', NAME1);
    await setDlg('zone', ZONE1);
    await pause(300);
    err = await submitDlg();
    if (err) no(`saving the edit failed: "${err}"`);
    else ok(`changed the name to "${NAME1}" and the zone to "${ZONE1}"`);

    // ---- 4. the grid reflects it, without a reload ------------------------
    await pause(900);
    const cells = await rowCells(CODE);
    if (!cells) {
        no(`${CODE} vanished from the grid after the edit`);
    } else {
        if (cells[1] === NAME1) ok('the grid shows the new name straight away');
        else no(`the grid still shows "${cells[1]}"`);
        if (cells[3] === ZONE1) ok('and the new zone');
        else no(`the grid still shows zone "${cells[3]}"`);
    }
    await page.screenshot({ path: `${SHOTDIR}/2-edited.png` });

    // ---- 5. reopen: the NEW values come back ------------------------------
    // Proves the write landed rather than the grid having been patched locally.
    await clickUnitRow(CODE);
    await pause(1200);
    dlg = await readDlg();
    if (dlg.name === NAME1 && dlg.zone === ZONE1)
        ok('reopening it shows the saved values, not the old ones');
    else
        no(`reopened with name "${dlg.name}" and zone "${dlg.zone}"`);

    // The code must still be editable too — it is the field most likely to
    // carry a typo, and the one a UNIQUE constraint makes scary to change.
    if (dlg.code === CODE) ok('the code is editable on the same dialog');
    else no(`the code box reads "${dlg.code}"`);

    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.m2o-modal-foot button')]
            .find(x => /cancel/i.test(x.textContent));
        if (b) b.click();
    });
    await pause(600);

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors in the whole journey');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
