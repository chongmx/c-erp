/**
 * render_booking.mjs — the booking calendar, driven by clicking.
 *
 *   node tests/lib/render_booking.mjs ZZBK
 *
 * The journey an operator actually has:
 *
 *   1. create two units (through the New-unit dialog, not the API)
 *   2. open Rental -> Booking
 *   3. pick a unit from the sidebar, click two days, press Book
 *   4. choose a customer in the dialog and confirm
 *   5. the day boxes fill, the occupancy figure moves
 *   6. book the SAME unit again for a later window — this is the capability
 *      migration 820 unlocked; the old one-live-line-per-unit index made it
 *      impossible, so a calendar you could only book once on
 *   7. try to overlap the first booking — refused, naming the dates
 *
 * Step 6 and 7 are the pair that matters. Relaxing a double-let guard is easy
 * to get wrong in the direction that lets two tenants into one locker, so the
 * test proves BOTH halves: sequential lets allowed, overlapping lets refused.
 *
 * Nothing is created over the API.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/booking';

const PFX      = process.argv[2] || 'ZZBK';
const CUSTOMER = `${PFX} Tenant Bhd`;
const UNITS    = [
    { code: `${PFX}-C1`, name: `${PFX} Cabin One` },
    { code: `${PFX}-C2`, name: `${PFX} Cabin Two` },
];

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
await page.setViewport({ width: 1600, height: 1000 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

const pause = ms => new Promise(r => setTimeout(r, ms));

/** The month the calendar opens on, and two safe windows inside it. */
function windows(monthIso) {          // monthIso = "YYYY-MM"
    return {
        aFrom: `${monthIso}-10`, aTo: `${monthIso}-14`,
        bFrom: `${monthIso}-20`, bTo: `${monthIso}-23`,
        clash: `${monthIso}-12`,
    };
}

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

/** Choose a partner in whichever M2OSelect is on screen. */
async function pickPartner(name) {
    const sel = '.m2o[data-model="res.partner"] input.m2o-input';
    if (!(await page.$(sel))) return false;
    await page.click(sel);
    await pause(250);
    await page.type(sel, name, { delay: 25 });
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

/** Fill the booking dialog and confirm. Returns its error text, or null. */
async function bookDates(from, to) {
    await page.evaluate(() => {
        const b = document.querySelector('[data-book="open"]');
        if (b) b.click();
    });
    await pause(900);
    if (!(await page.$('.rbk-dlg'))) return 'the booking dialog did not open';
    if (!(await pickPartner(CUSTOMER))) return 'the customer could not be chosen';
    await pause(500);
    await page.evaluate((f, t) => {
        const set = (k, v) => {
            const el = document.querySelector(`[data-bk="${k}"]`);
            if (!el) return;
            el.focus(); el.value = v;
            el.dispatchEvent(new Event('input', { bubbles: true }));
        };
        set('date_start', f); set('date_end', t);
    }, from, to);
    await pause(300);
    await page.evaluate(() => {
        const b = document.querySelector('[data-book="confirm"]');
        if (b) b.click();
    });
    await pause(2600);
    return page.evaluate(() => {
        const e = document.querySelector('.rbk-dlg-err');
        return e ? e.textContent.trim() : null;
    });
}

/** Click a day cell in the unit calendar by its ISO date. */
async function clickDay(iso) {
    return page.evaluate((d) => {
        const el = document.querySelector(`[data-day="${d}"]`);
        if (!el) return false;
        el.click();
        return true;
    }, iso);
}

async function letDays(unitCode) {
    return page.evaluate((code) => {
        const row = [...document.querySelectorAll('.rbk-strip-row')]
            .find(r => (r.querySelector('.rbk-strip-code') || {}).textContent === code);
        if (!row) return -1;
        return row.querySelectorAll('.rbk-box.let').length;
    }, unitCode);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    const hasScreen = await page.evaluate(() => typeof RentalBooking);
    if (hasScreen === 'function') ok('the page has loaded the Booking screen');
    else { no(`typeof RentalBooking is "${hasScreen}" — this page is running old code`);
           throw new Error('old bundle'); }

    // ---- 0. a customer, through the contact form ---------------------------
    await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
    await pause(1400);
    await clickNew();
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await setField('name', CUSTOMER);
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
    ok('a customer exists, created through the form');

    // ---- 1. two units, through the New-unit dialog -------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.unit', 0));
    await pause(1800);
    for (const u of UNITS) {
        await page.evaluate(() => {
            const b = [...document.querySelectorAll('button')]
                .find(x => /new unit/i.test((x.textContent || '').trim()));
            if (b) b.click();
        });
        await pause(1000);
        await page.evaluate((code, name) => {
            const set = (k, v) => {
                const el = document.querySelector(`[data-nu="${k}"]`);
                if (!el) return;
                el.focus(); el.value = v;
                el.dispatchEvent(new Event('input', { bubbles: true }));
            };
            set('code', code); set('name', name);
        }, u.code, u.name);
        await pause(300);
        await page.evaluate(() => {
            const b = [...document.querySelectorAll('.m2o-modal-foot button')]
                .find(x => /create/i.test(x.textContent));
            if (b) b.click();
        });
        await pause(2000);
    }
    ok(`${UNITS.length} units created by clicking`);

    // ---- 2. the Booking screen --------------------------------------------
    await page.evaluate(() => window.ErpNav.openRecord('rental.booking', 0));
    await pause(2600);
    if (await page.$('.rbk-wrap')) ok('the Booking screen opens');
    else { no('the Booking screen did not render'); throw new Error('no screen'); }

    const month = await page.evaluate(() => {
        const el = document.querySelector('.rbk-month-label');
        return el ? el.textContent.trim() : '';
    });
    if (month) ok(`it opens on the current month (${month})`);
    else no('no month is shown');

    const monthIso = await page.evaluate(() => window.__rbkMonth || '');
    // Read the month from the first day box rather than parsing the label.
    const iso = await page.evaluate(() => {
        const b = document.querySelector('[data-unit-day]');
        return b ? b.dataset.unitDay.split(':')[1].slice(0, 7) : '';
    });
    const W = windows(iso || monthIso);

    const strips = await page.evaluate(() =>
        document.querySelectorAll('.rbk-strip-row').length);
    if (strips >= UNITS.length) ok(`the sidebar view shows a day-strip per unit (${strips})`);
    else no(`expected at least ${UNITS.length} strips, found ${strips}`);

    const boxesPerRow = await page.evaluate(() => {
        const r = document.querySelector('.rbk-strip-row');
        return r ? r.querySelectorAll('.rbk-box').length : 0;
    });
    if (boxesPerRow >= 28 && boxesPerRow <= 31)
        ok(`each strip has one box per day of the month (${boxesPerRow})`);
    else no(`a strip has ${boxesPerRow} boxes — expected 28-31`);
    await page.screenshot({ path: `${SHOTDIR}/1-strips-empty.png` });

    // ---- 3. open a unit and book it ---------------------------------------
    const opened = await page.evaluate((code) => {
        const row = [...document.querySelectorAll('.rbk-side-unit')]
            .find(r => (r.querySelector('.rbk-side-name') || {}).textContent === code);
        if (!row) return false;
        row.click();
        return true;
    }, UNITS[0].code);
    if (opened) ok(`${UNITS[0].code} opens from the sidebar`);
    else { no(`${UNITS[0].code} is not in the sidebar`); throw new Error('no unit row'); }
    await pause(1200);

    if (await page.$('.rbk-cal')) ok('its month calendar is drawn');
    else no('the unit calendar did not render');

    if (!(await clickDay(W.aFrom))) { no(`day ${W.aFrom} is not clickable`); throw new Error('no day'); }
    await clickDay(W.aTo);
    await pause(500);
    const picked = await page.evaluate(() => document.querySelectorAll('.rbk-cal-day.picked').length);
    if (picked === 5) ok(`clicking two days selects the range between them (${picked} days)`);
    else no(`selecting ${W.aFrom}..${W.aTo} highlighted ${picked} days, expected 5`);
    await page.screenshot({ path: `${SHOTDIR}/2-range-picked.png` });

    let err = await bookDates(W.aFrom, W.aTo);
    if (err) no(`the first booking was refused: "${err}"`);
    else ok(`booked ${W.aFrom} → ${W.aTo}`);
    await pause(1200);
    await page.screenshot({ path: `${SHOTDIR}/3-unit-booked.png` });

    const filled = await page.evaluate(() =>
        document.querySelectorAll('.rbk-cal-day.let').length);
    if (filled === 5) ok('the calendar shows those five days as let');
    else no(`${filled} days show as let after booking five`);

    // ---- 4. a SECOND, non-overlapping booking on the same unit ------------
    // Impossible before migration 820: the partial unique index allowed one
    // live line per unit, so a calendar could only ever be booked once.
    await clickDay(W.bFrom);
    await clickDay(W.bTo);
    await pause(400);
    err = await bookDates(W.bFrom, W.bTo);
    if (err) no(`a later, non-overlapping booking was refused: "${err}"`);
    else ok(`a second, non-overlapping booking on the same unit is allowed`);
    await pause(1200);

    // ---- 5. an OVERLAPPING booking must be refused, in words --------------
    await clickDay(W.clash);
    await clickDay(W.clash);
    await pause(400);
    err = await bookDates(W.clash, W.clash);
    if (!err) {
        no('an overlapping booking was ACCEPTED — the double-let guard is gone');
    } else if (/already let/i.test(err)) {
        ok(`an overlapping booking is refused, naming the clash: "${err.slice(0, 70)}…"`);
    } else {
        no(`overlap refused, but unhelpfully: "${err}"`);
    }
    await page.screenshot({ path: `${SHOTDIR}/4-overlap-refused.png` });
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.m2o-modal-foot button')]
            .find(x => /cancel/i.test(x.textContent));
        if (b) b.click();
    });
    await pause(600);

    // ---- 6. back to the strips: the usage is visible at a glance ----------
    await page.evaluate(() => {
        const b = document.querySelector('[data-pick="all"]');
        if (b) b.click();
    });
    await pause(1500);
    const let1 = await letDays(UNITS[0].code);
    const let2 = await letDays(UNITS[1].code);
    if (let1 === 9) ok(`${UNITS[0].code}'s strip shows 9 let days (5 + 4)`);
    else no(`${UNITS[0].code}'s strip shows ${let1} let days, expected 9`);
    if (let2 === 0) ok(`${UNITS[1].code}, never booked, shows an empty strip`);
    else no(`${UNITS[1].code} shows ${let2} let days but was never booked`);

    const headline = await page.evaluate(() => {
        const el = document.querySelector('.rbk-pct');
        return el ? el.textContent.trim() : '';
    });
    if (headline && headline !== '0%') ok(`the month's occupancy rate reads ${headline}`);
    else no(`the occupancy headline reads "${headline}" after booking nine days`);
    await page.screenshot({ path: `${SHOTDIR}/5-strips-booked.png` });

    // A refused booking IS a 400, by design, and Chrome logs every non-2xx
    // fetch as a console error. Counting those would make this test fail for
    // doing exactly what it set out to prove — so the HTTP-status noise is
    // dropped and real faults are not: a pageerror is an uncaught exception,
    // and any other console error is still a failure.
    const real = errs.filter(e => !/Failed to load resource/i.test(e));
    if (real.length) no('browser errors: ' + real.slice(0, 3).join(' | '));
    else ok('no uncaught errors in the whole journey (the 400 on the refused booking is by design)');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
