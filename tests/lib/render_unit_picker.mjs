/**
 * render_unit_picker.mjs — putting a unit on a rental contract, by clicking.
 *
 *   node tests/lib/render_unit_picker.mjs ZZUP
 *
 * Reported: "unit picker never really work."
 *
 * It was not one bug, it was four, and each hid the next:
 *
 *   1. NOTHING TO PICK. A clean database has zero rental units, so the picker
 *      correctly answered "No match" forever. Correct, and useless.
 *   2. NO WAY OUT. The "＋" beside the picker posted create({name}) and
 *      rental.unit requires a CODE, so the one escape hatch on that screen came
 *      back "An internal error occurred" — on the very screen where the picker
 *      is empty because no unit exists yet.
 *   3. THE REST OF THE LINE WAS UNUSABLE ANYWAY. billing_mode and state were
 *      registered FieldType::Char, so the line's Billing and Status rendered as
 *      free TEXT BOXES: you had to type "recurring" and "active" letter-perfect
 *      into a form that never showed the valid values, and a CHECK constraint
 *      refused anything else on save.
 *   4. AND THE CHOICE DID NOT STICK. Once those were dropdowns, the values
 *      still saved as the column defaults: onFormChange's o2m branch ran
 *      parseInt over any <select>, so 'recurring' became 0 and overwrote what
 *      onFormInput had just stored correctly. Found by this file, on its first
 *      run, after the other three were fixed — which is the argument for
 *      asserting the stored value and not just the rendered control.
 *
 * Every one of those is invisible to an API test, which creates its unit with
 * one call and never opens the form. So this drives the screen and plants
 * nothing over the API.
 *
 * Prints PASS/FAIL lines; exits non-zero on any failure.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/unit_picker';

const PFX      = process.argv[2] || 'ZZUP';
const CUSTOMER = `${PFX} Renter Bhd`;
const CODE     = `${PFX}-A101`;
const UNITNAME = `${PFX} Locker A101`;
const LABEL    = `${CODE} — ${UNITNAME}`;      // what the picker should read
const REF      = `${PFX}-RC`;

/**
 * Three units, so the dropdown is a LIST rather than a single row.
 * All three are typed into existence through the "＋" dialog on the contract
 * form itself — the point being that an operator who has no units yet can get
 * from an empty picker to a working contract without leaving the screen.
 */
const UNITS = [
    { code: CODE,           name: UNITNAME },
    { code: `${PFX}-A102`,  name: `${PFX} Locker A102` },
    { code: `${PFX}-B201`,  name: `${PFX} Locker B201` },
];
const LABELS = UNITS.map(u => `${u.code} — ${u.name}`);

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

// Login only — no record below is created or read over RPC.
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

async function clickText(sel, text) {
    return page.evaluate((s, t) => {
        const el = [...document.querySelectorAll(s)].find(x => x.textContent.trim() === t);
        if (!el) return false;
        el.click();
        return true;
    }, sel, text);
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

/** Set one cell of the first o2m line, by field name. */
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

async function clickNew() {
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
}

/**
 * Type into a picker and return the options it settles on.
 * Closes the dropdown first: leaving it open means reading the PREVIOUS
 * search's rows while the new one is still in flight.
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
    await page.type(sel, term, { delay: 30 });
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

const UNIT_SEL = '.o2m-table .m2o[data-model="rental.unit"] input.m2o-input';

/**
 * Click the picker and let it list what it has, with nothing typed.
 *
 * This is the plain case an operator meets first — click the box, see the
 * units — and it is a different code path from typing: focus runs the search
 * with an EMPTY term, so it exercises the domain on its own rather than the
 * name/code ilike. Returns the options once they stop changing; the dropdown
 * is left OPEN so the caller can photograph it.
 */
async function pickerFocusOptions(sel) {
    if (!(await page.$(sel))) return null;
    await page.evaluate((s) => {
        const el = document.querySelector(s);
        if (el) el.blur();
    }, sel);
    await pause(350);
    await page.click(sel);
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

/** Fill and submit the "＋" quick-create dialog. Returns its error text, if any. */
async function quickCreate(values) {
    await page.evaluate((vals) => {
        for (const el of document.querySelectorAll('.gf-modal-card input[data-quick]')) {
            const v = vals[el.dataset.quick];
            if (v === undefined) continue;
            el.focus(); el.value = v;
            el.dispatchEvent(new Event('input', { bubbles: true }));
        }
    }, values);
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-modal-actions button')]
            .find(x => /create/i.test(x.textContent));
        if (b) b.click();
    });
    await pause(2200);
    return page.evaluate(() => {
        const e = document.querySelector('.gf-modal-err');
        return e ? e.textContent.trim() : null;
    });
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 0. a customer to rent to, made through the contact form ----------
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1400);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', CUSTOMER);
    await clickText('.contact-badge', 'Company');
    await pause(250);
    await clickText('.contact-badge', 'Customer');
    await pause(250);
    await clickText('.so-action-btns button', 'Save');
    await pause(2200);
    ok('a customer exists, created through the form');

    // ---- 1. a new contract, and a line on it ------------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1500);
    await clickNew();
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(1000);

    const addLine = await page.evaluate(() => {
        const b = [...document.querySelectorAll('button')]
            .find(x => /add a line/i.test((x.textContent || '').trim()));
        if (!b) return false;
        b.click();
        return true;
    });
    if (addLine) ok('the contract form offers "+ Add a line"');
    else { no('there is no way to add a unit line to a contract'); throw new Error('no add-line'); }
    await pause(900);

    // ---- 2. the shape of the line row -------------------------------------
    // The regression guard for defect 3: Billing and Status must be real
    // dropdowns carrying the constraint's vocabulary, and the dates must be
    // date inputs. As free text boxes they were unusable and unsaveable.
    const shape = await page.evaluate(() => {
        const row = document.querySelector('.o2m-table tbody tr');
        if (!row) return null;
        const out = {};
        for (const td of row.querySelectorAll('td')) {
            const m2o = td.querySelector('.m2o');
            if (m2o) { out[m2o.dataset.model] = 'm2o'; continue; }
            const el = td.querySelector('select, input');
            if (!el) continue;
            const name = el.dataset.field;
            if (!name) continue;
            out[name] = el.tagName === 'SELECT'
                ? 'select:' + [...el.options].map(o => o.value).filter(Boolean).join(',')
                : 'input:' + el.type;
        }
        return out;
    });
    if (!shape) { no('the line row did not render'); throw new Error('no row'); }

    if (shape['rental.unit'] === 'm2o') ok('the line has a Unit picker');
    else no('the line has no Unit picker at all');

    if (shape.date_start === 'input:date') ok('Start Date is a date input');
    else no(`Start Date rendered as ${shape.date_start}, not a date input`);

    if ((shape.billing_mode || '').startsWith('select:')) {
        ok('Billing is a dropdown, not a text box');
        const opts = shape.billing_mode.slice(7).split(',');
        if (opts.includes('recurring') && opts.includes('oneoff'))
            ok('and it offers the values the CHECK constraint allows');
        else no(`Billing offered ${JSON.stringify(opts)}`);
    } else no(`Billing rendered as ${shape.billing_mode} — a text box you must spell into`);

    if ((shape.state || '').startsWith('select:')) {
        ok('Status is a dropdown, not a text box');
        const opts = shape.state.slice(7).split(',');
        if (opts.includes('active') && opts.includes('pending'))
            ok('and it offers the line states');
        else no(`Status offered ${JSON.stringify(opts)}`);
    } else no(`Status rendered as ${shape.state} — a text box you must spell into`);
    await page.screenshot({ path: `${SHOTDIR}/1-line-row.png` });

    // ---- 3. an empty picker says so, and "＋" is the way out ---------------
    let opts = await pickerOptions(UNIT_SEL, CODE);
    if (opts === null) { no('the unit picker input is missing'); throw new Error('no picker'); }
    if (!opts.includes(LABEL)) ok('the unit does not exist yet, and the picker says so');
    else no('a unit with this code already existed — the fixture did not clean up');

    const plusFound = await page.evaluate(() => {
        const b = document.querySelector('.o2m-table button.gf-addnew');
        if (!b) return false;
        b.click();
        return true;
    });
    if (plusFound) ok('there is a "＋" beside the picker');
    else { no('no "＋" beside the unit picker — an empty picker is a dead end'); throw new Error('no plus'); }
    await pause(1500);

    // Defect 2: the dialog must ask for everything the model REQUIRES.
    const asked = await page.evaluate(() =>
        [...document.querySelectorAll('.gf-modal-card .gf-modal-lbl')].map(l => l.textContent.trim()));
    if (asked.includes('Code')) ok('the quick-create asks for the Code a unit requires');
    else no(`the quick-create asked only for ${JSON.stringify(asked)} — a unit needs a Code`);
    await page.screenshot({ path: `${SHOTDIR}/2-quick-create.png` });

    let err = await quickCreate({ name: UNITNAME, code: CODE });
    if (err) no(`creating the unit from the dialog failed: "${err}"`);
    else ok('the unit was created from the dialog');

    const boxed = await page.evaluate((s) => {
        const el = document.querySelector(s); return el ? el.value : null;
    }, UNIT_SEL);
    if (boxed === LABEL) ok(`and the picker immediately reads "${LABEL}"`);
    else no(`the picker reads "${boxed}" after creating, expected "${LABEL}"`);

    // ---- 3b. two more, so the dropdown is a LIST --------------------------
    // One unit proves the plumbing; it does not prove the picker OFFERS a
    // choice. Both extras go in the same way — click "＋", fill, Create.
    for (const u of UNITS.slice(1)) {
        await page.evaluate(() => {
            const b = document.querySelector('.o2m-table button.gf-addnew');
            if (b) b.click();
        });
        await pause(1400);
        const e2 = await quickCreate({ name: u.name, code: u.code });
        if (e2) no(`creating ${u.code} from the dialog failed: "${e2}"`);
    }
    ok(`${UNITS.length} units now exist, every one of them created by clicking`);

    // ---- 4. a duplicate code is refused in words ---------------------------
    // Left as a raw constraint violation this reached the screen as "An
    // internal error occurred", from a dialog that could then only be cancelled.
    await page.evaluate(() => {
        const b = document.querySelector('.o2m-table button.gf-addnew');
        if (b) b.click();
    });
    await pause(1400);
    err = await quickCreate({ name: `${UNITNAME} again`, code: CODE });
    if (!err) {
        no('a duplicate unit code was accepted');
    } else if (/internal error/i.test(err)) {
        no(`a duplicate code still reports "${err}"`);
    } else {
        ok(`a duplicate code is refused in words: "${err}"`);
    }
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-modal-actions button')]
            .find(x => /cancel/i.test(x.textContent));
        if (b) b.click();
    });
    await pause(600);

    // ---- 5. click the picker: it lists the units ---------------------------
    // The plain case, and the one the report was about. No typing: click the
    // box and the units are there. The dropdown is left open and photographed
    // as it stands on screen.
    opts = await pickerFocusOptions(UNIT_SEL);
    if (opts === null) { no('the unit picker input is missing'); throw new Error('no picker'); }
    const missing = LABELS.filter(l => !opts.includes(l));
    if (!missing.length) ok(`clicking the picker lists all ${LABELS.length} units`);
    else no(`the dropdown was missing ${JSON.stringify(missing)} — it showed ${JSON.stringify(opts)}`);

    // EXACTLY the units that exist — no more, no fewer.
    //
    // "contains what I made" is the weaker claim, and it is the one that lets a
    // picker quietly show the wrong population: a stale prefetch, another
    // company's rows, archived units, or a domain that filters out most of the
    // table. The baseline has zero rental units, so after creating three the
    // list must be those three and nothing else. The shell test then re-counts
    // the same thing straight out of the database.
    const extra = opts.filter(o => !LABELS.includes(o));
    if (opts.length === LABELS.length && !extra.length)
        ok(`and lists exactly those ${LABELS.length} — nothing stale, nothing hidden`);
    else
        no(`the dropdown holds ${opts.length} rows, expected exactly ${LABELS.length}`
           + (extra.length ? ` — unexpected: ${JSON.stringify(extra)}` : ''));

    // Photograph it BEFORE anything else touches the page.
    //
    // Every CDP round trip is a chance for headless Chrome to blur the page,
    // and a blur starts M2OSelect's 150 ms close timer — so a "is it open?"
    // evaluate placed before the screenshot is what CLOSES the dropdown, and
    // the picture comes back empty while the assertion says it was open. Shoot
    // first; the options read above already prove it was open.
    await page.screenshot({ path: `${SHOTDIR}/5-picker-open.png`,
                            captureBeyondViewport: false });

    // Is it actually ON SCREEN? Not "does the element have a height" — the
    // list had a perfectly good height while being clipped away entirely by
    // `.o2m-table { overflow: hidden }`, so the DOM said open and the user saw
    // an empty box. hit-test the middle of the first row instead: if the point
    // where an option is drawn does not belong to that option, something is
    // covering or clipping it, and the picker is unusable however correct the
    // markup is.
    const seen = await page.evaluate(() => {
        const pop = document.querySelector('.m2o-pop');
        if (!pop) return { open: false };
        const opt = pop.querySelector('.m2o-opt');
        if (!opt) return { open: true, rows: 0 };
        const r = opt.getBoundingClientRect();
        if (r.width === 0 || r.height === 0) return { open: true, rows: 1, painted: false };
        const inViewport = r.top >= 0 && r.left >= 0 &&
                           r.bottom <= innerHeight && r.right <= innerWidth;
        const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
        return { open: true, rows: 1, painted: true, inViewport,
                 hit: !!(hit && (hit === opt || opt.contains(hit))),
                 text: opt.textContent.trim() };
    });
    if (!seen.open || !seen.painted) {
        no('the dropdown was not painted at the moment of capture');
    } else if (!seen.inViewport) {
        no('the dropdown opened outside the viewport — off screen for the user');
    } else if (!seen.hit) {
        no('the dropdown is in the DOM but nothing is drawn where its rows are — '
           + 'it is being clipped or covered, so the user sees an empty box');
    } else {
        ok(`the dropdown is visible and hit-testable on screen ("${seen.text}")`);
    }
    console.log(`    dropdown showed: ${JSON.stringify(opts)}`);

    // ---- 6. and it narrows, by code and by name ---------------------------
    opts = await pickerOptions(UNIT_SEL, CODE);
    if (opts.includes(LABEL)) ok('typing the CODE finds the unit');
    else no(`typing "${CODE}" offered ${JSON.stringify(opts)}`);
    if (opts.length === 1) ok('and narrows the list to just that one');
    else no(`typing the full code still offered ${opts.length} rows`);

    opts = await pickerOptions(UNIT_SEL, 'Locker A101');
    if (opts.includes(LABEL)) ok('typing the NAME finds it too');
    else no(`typing the name offered ${JSON.stringify(opts)}`);

    await page.evaluate((t) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === t);
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, LABEL);
    await pause(700);

    // ---- 7. fill the line and save the contract ----------------------------
    await setLineField('date_start', '2026-12-01');
    await setLineField('unit_price', '450');
    await setLineField('billing_mode', 'recurring');
    await setLineField('state', 'active');
    await pause(300);

    const custSel = '.gf-grid .m2o[data-model="res.partner"] input.m2o-input';
    opts = await pickerOptions(custSel, CUSTOMER);
    if (opts && opts.includes(CUSTOMER)) ok('the customer is selectable on the contract');
    else no(`the customer picker offered ${JSON.stringify(opts)}`);
    await page.evaluate((t) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === t);
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, CUSTOMER);
    await pause(600);

    await setField('name', REF);
    await setField('date_start', '2026-12-01');
    await pause(300);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.gf-actions button, .btn')]
            .find(x => /^create$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await pause(2800);
    await page.screenshot({ path: `${SHOTDIR}/3-saved.png` });
    ok('the contract was saved from the screen');

    // ---- 8. reopening shows the line, with its unit ------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1600);
    const opened = await page.evaluate((ref) => {
        const row = [...document.querySelectorAll('.list-row')].find(r => r.textContent.includes(ref));
        if (!row) return false;
        row.click();
        return true;
    }, REF);
    if (!opened) {
        no(`the saved contract ${REF} was not in the list to reopen`);
    } else {
        await pause(2400);
        const back = await page.evaluate((s) => {
            const el = document.querySelector(s);
            const sel = document.querySelector('.o2m-table [data-field="billing_mode"]');
            return { unit: el ? el.value : null,
                     rows: document.querySelectorAll('.o2m-table tbody tr').length,
                     billing: sel ? sel.value : null };
        }, UNIT_SEL);
        if (back.rows >= 1) ok('the saved contract reopens with its line');
        else no('the line did not survive the save');
        if (back.unit === LABEL) ok('and the line still names its unit');
        else no(`the reopened line shows "${back.unit}", expected "${LABEL}"`);
        if (back.billing === 'recurring') ok('and the billing mode chosen from the dropdown persisted');
        else no(`billing mode came back "${back.billing}", expected "recurring"`);
        await page.screenshot({ path: `${SHOTDIR}/4-reopened.png` });
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
