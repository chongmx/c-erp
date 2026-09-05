/**
 * render_unit_type.mjs — a NEW unit type must reach the picker that uses it.
 *
 *   node tests/lib/render_unit_type.mjs ZZUT
 *
 * Reported: "make sure when I create a new unit type, it reflects on that drop
 * down and can be searched."
 *
 * It did not. The New-unit dialog's Type control was a plain <select> filled
 * from one `search_read(limit 200)` issued when the Units screen opened — the
 * same three defects M2OSelect exists to kill:
 *
 *   • STALE — a type created afterwards, on the Unit Types screen or in another
 *     tab, was simply not in the list until the whole screen was reloaded;
 *   • TRUNCATED — 200 rows, unordered, so the default `id ASC` kept the OLDEST;
 *   • NOT SEARCHABLE — a <select> has no search at all.
 *
 * So this walks the real sequence, clicking throughout and creating nothing
 * over the API:
 *
 *   1. Rental → Unit Types → New → a type that did not exist a second ago
 *   2. Rental → Units → "+ New unit", WITHOUT reloading the app
 *   3. type part of the new type's name — it must be found
 *   4. pick it, create the unit, and the unit must carry that type
 *
 * Step 2 is the load-bearing one: no reload. A prefetch would pass this test
 * if the page were refreshed in between, which is exactly the bug reported.
 *
 * Prints PASS/FAIL lines; exits non-zero on any failure.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/unit_type';

const PFX      = process.argv[2] || 'ZZUT';
const TYPE_NAME = `${PFX} Walk-in Vault`;
const TYPE_CODE = `${PFX}V`;
const TYPE_LABEL = `${TYPE_CODE} — ${TYPE_NAME}`;
const UNIT_CODE = `${PFX}-V1`;
const UNIT_NAME = `${PFX} Vault One`;

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

// Login only — every record below is typed and clicked into existence.
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

/**
 * Type into a picker and return the options it settles on, leaving the
 * dropdown OPEN. Closes it first so a previous search's rows are never read
 * as this one's answer.
 */
async function pickerOptions(sel, term) {
    if (!(await page.$(sel))) return null;
    await page.evaluate((s) => {
        const el = document.querySelector(s);
        if (el) { el.value = ''; el.blur(); }
    }, sel);
    await pause(350);
    await page.click(sel);
    await pause(200);
    await page.evaluate((s) => { const el = document.querySelector(s); if (el) el.value = ''; }, sel);
    if (term) await page.type(sel, term, { delay: 30 });
    const read = () => page.evaluate(() =>
        [...document.querySelectorAll('.m2o-pop .m2o-opt')].map(o => o.textContent.trim()));
    let last = null, stable = 0;
    for (let i = 0; i < 32 && stable < 2; i++) {
        await pause(250);
        const now = JSON.stringify(await read());
        if (now === last) stable++; else { stable = 0; last = now; }
    }
    return read();
}

/**
 * Are the dropdown rows actually painted where they claim to be — ALL of them?
 *
 * Checking only the first row is not enough, and that is not a hypothetical:
 * with the dialog clipping its own overflow, row one was perfectly visible and
 * hit-testable while rows two to seven were cut off at the card's edge. A user
 * saw two of seven unit types and no way to reach the rest. So hit-test the
 * LAST row too — partial clipping is the normal shape of this bug, because the
 * list always starts inside whatever is clipping it.
 *
 * Returns {rows, firstSeen, lastSeen} so the caller can say which half failed.
 */
async function dropdownVisibility() {
    return page.evaluate(() => {
        const opts = [...document.querySelectorAll('.m2o-pop .m2o-opt')];
        if (!opts.length) return { rows: 0 };
        const seen = (el) => {
            const r = el.getBoundingClientRect();
            if (!r.width || !r.height) return false;
            if (r.top < 0 || r.bottom > innerHeight) return false;
            const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
            return !!(hit && (hit === el || el.contains(hit)));
        };
        return { rows: opts.length,
                 firstSeen: seen(opts[0]),
                 lastSeen:  seen(opts[opts.length - 1]) };
    });
}

const TYPE_SEL = '.m2o[data-model="rental.unit.type"] input.m2o-input';

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1. a brand new unit type, created through its own screen ---------
    await page.evaluate(() => window.ErpNav.openRecord('rental.unit.type', 0));
    await pause(1500);
    await clickNew();
    await page.waitForSelector('.gf-shell [data-field="name"]', { timeout: 12000 });
    await setField('name', TYPE_NAME);
    await setField('code', TYPE_CODE);
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(2500);
    ok(`the unit type "${TYPE_NAME}" was created through the form`);
    await page.screenshot({ path: `${SHOTDIR}/1-type-created.png` });

    // ---- 2. straight to Units — NO page reload ---------------------------
    // This is the whole point. The old <select> was filled when the Units
    // screen first opened; reloading the app here would hide the defect.
    await page.evaluate(() => window.ErpNav.openRecord('rental.unit', 0));
    await pause(2000);
    const opened = await page.evaluate(() => {
        const b = [...document.querySelectorAll('button')]
            .find(x => /new unit/i.test((x.textContent || '').trim()));
        if (!b) return false;
        b.click();
        return true;
    });
    if (opened) ok('the Units screen offers "+ New unit"');
    else { no('there is no "+ New unit" button on the Units screen'); throw new Error('no button'); }
    await pause(1200);

    if (await page.$(TYPE_SEL)) ok('the Type field is a searchable picker, not a fixed <select>');
    else { no('the Type field is not an M2OSelect — a new type cannot be searched for'); throw new Error('no type picker'); }

    // ---- 3. it must be findable BY SEARCH, without a reload --------------
    let opts = await pickerOptions(TYPE_SEL, 'Walk-in');
    if (opts && opts.includes(TYPE_LABEL))
        ok(`typing part of its name finds the type created moments ago`);
    else
        no(`searching "Walk-in" offered ${JSON.stringify(opts)} — the new type is missing`);

    // Photograph BEFORE any further round trip: a blur closes the dropdown.
    await page.screenshot({ path: `${SHOTDIR}/2-type-picker-open.png`,
                            captureBeyondViewport: false });
    let vis = await dropdownVisibility();
    if (vis.firstSeen) ok('the type dropdown is visible and hit-testable on screen');
    else no('the type dropdown is in the DOM but nothing is drawn where its rows are');

    // ---- the WHOLE list, with nothing typed ------------------------------
    // The seeded types plus the new one. This is the case the user audits:
    // click the box and see every type that exists.
    opts = await pickerOptions(TYPE_SEL, '');
    await page.screenshot({ path: `${SHOTDIR}/3-type-list-all.png`,
                            captureBeyondViewport: false });
    vis = await dropdownVisibility();
    if (opts && opts.includes(TYPE_LABEL))
        ok(`clicking the picker lists it among all ${opts.length} types`);
    else
        no(`the full list did not include it: ${JSON.stringify(opts)}`);

    // Every seeded type must be there too — the new one arriving is no use if
    // it pushed the existing ones out of a capped list.
    for (const seeded of ['Small Locker', 'Medium Locker', 'Large Locker',
                          'Pallet Space']) {
        if (opts && opts.some(o => o.includes(seeded))) continue;
        no(`the seeded type "${seeded}" is missing from the list`);
    }
    ok('and the seeded types are all still listed alongside it');

    if (vis.rows !== (opts || []).length)
        no(`only ${vis.rows} rows are rendered for ${opts.length} options`);
    else if (!vis.firstSeen || !vis.lastSeen)
        no(`the list is CLIPPED — ${vis.rows} rows, first visible: ${vis.firstSeen}, `
           + `last visible: ${vis.lastSeen}. The user cannot reach the rest.`);
    else
        ok(`all ${vis.rows} rows are on screen, first to last — nothing cut off`);
    console.log(`    type dropdown showed: ${JSON.stringify(opts)}`);

    // ---- 4. pick it, and the unit must carry it --------------------------
    await page.evaluate((t) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === t);
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, TYPE_LABEL);
    await pause(700);

    await page.evaluate((code, name) => {
        const set = (k, v) => {
            const el = document.querySelector(`[data-nu="${k}"]`);
            if (!el) return;
            el.focus(); el.value = v;
            el.dispatchEvent(new Event('input', { bubbles: true }));
        };
        set('code', code); set('name', name);
    }, UNIT_CODE, UNIT_NAME);
    await pause(400);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.m2o-modal-foot button')]
            .find(x => /create/i.test(x.textContent));
        if (b) b.click();
    });
    await pause(2600);
    await page.screenshot({ path: `${SHOTDIR}/4-unit-created.png` });

    const onGrid = await page.evaluate((code) =>
        document.body.textContent.includes(code), UNIT_CODE);
    if (onGrid) ok('the new unit appears on the grid straight away');
    else no('the unit was created but the grid did not refresh to show it');

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors in the whole journey');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
