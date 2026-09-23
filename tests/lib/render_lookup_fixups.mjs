/**
 * render_lookup_fixups.mjs — clearing a lookup's errors from the review desk.
 *
 *   node tests/lib/render_lookup_fixups.mjs ZZLF
 *
 * Reported with a screenshot (CERP-8): a 32-bit MCU proposal sitting at
 * INVALID on two errors —
 *
 *     parameters[0].value  Cannot read value 'MIPS32 M4K'
 *     parameters[4].unit   Unknown unit 'Mbps' — see describe.units
 *
 * — and no way to act on either. "See describe.units" is advice to an agent,
 * not something a person can click, and a core name is not a number and never
 * will be. Both are the ordinary consequence of a catalogue that is still
 * growing, so both now carry a button.
 *
 * The journey, all clicks:
 *
 *   1. Products ▸ Part Lookup, "Needs fixing" — where an invalid proposal is
 *   2. the two errors each offer a way out
 *   3. KEEP IT AS TEXT on the value: the error becomes a note saying it is
 *      searchable by name and not by range
 *   4. ADD Mbps AS A UNIT: a form that asks the only two things that make a
 *      unit usable — what it measures, and how it converts to the base of
 *      that quantity
 *   5. the proposal re-validates itself and leaves "Needs fixing"
 *
 * Step 5 is the point. Fixing the cause without the proposal noticing would
 * leave the reviewer to work out that it was safe to re-submit.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/lookup_fixups';

const PFX  = process.argv[2] || 'ZZLF';
const MPN  = `${PFX}-PIC32MX795F512L`;
const KIND = `${PFX}_rate`;

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

// The proposal itself is a FIXTURE — it is what an agent left behind. The
// journey is what a person does with it, and that is all clicks.
const rpc = async (model, method, args) => {
    const r = await fetch(`${BASE}/web/dataset/call_kw`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Cookie': 'session_id=' + sid },
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
            params: { model, method, args, kwargs: {} } }) });
    const j = await r.json();
    if (j.error) throw new Error(j.error.data?.message || j.error.message);
    return j.result;
};

const staged = await rpc('part.lookup', 'submit', [{
    query: `${PFX} microcontroller 32bit with ethernet`,
    mpn: MPN,
    manufacturer: 'Microchip Technology',
    name: '32-bit MIPS MCU with 10/100 Ethernet MAC',
    parameters: [
        { name: 'core',          value: 'MIPS32 M4K' },
        { name: 'frequency_max', value: '80',  unit: 'MHz' },
        { name: 'ethernet',      value: '10/100', unit: 'Mbps' },
    ],
}]);
if (staged.state === 'invalid') ok('the proposal from the screenshot lands as INVALID, as reported');
else no(`the fixture staged as "${staged.state}" — this test proves nothing`);

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 950 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

const pause = ms => new Promise(r => setTimeout(r, ms));
const issues = () => page.evaluate(() =>
    [...document.querySelectorAll('.pl-issue')].map(i => ({
        level: (i.className.match(/lvl-(\w+)/) || [])[1] || '',
        text:  i.textContent.replace(/\s+/g, ' ').trim(),
    })));

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1. find it under "Needs fixing" ---------------------------------
    await page.evaluate(() => window.ErpNav.openRecord('part.lookup', 0));
    await pause(2500);
    const filtered = await page.evaluate(() => {
        const f = [...document.querySelectorAll('button, .pl-chip, .pl-filter')]
            .find(x => /needs fixing/i.test((x.textContent || '').trim()));
        if (!f) return false;
        f.click(); return true;
    });
    if (filtered) ok('"Needs fixing" is where an invalid proposal waits');
    else { no('there is no "Needs fixing" filter'); throw new Error('filter'); }
    await pause(2000);

    const opened = await page.evaluate((mpn) => {
        const row = [...document.querySelectorAll('.pl-row')]
            .find(r => (r.textContent || '').includes(mpn));
        if (!row) return false;
        row.click(); return true;
    }, MPN);
    if (!opened) { no(`${MPN} was not in the list`); throw new Error('row'); }
    await pause(2200);
    await page.screenshot({ path: `${SHOTDIR}/1-invalid.png` });

    // ---- 2. both errors offer a way out ----------------------------------
    const before = await issues();
    const errorsBefore = before.filter(i => i.level === 'error');
    if (errorsBefore.length === 2) ok('both errors are on screen');
    else no(`expected 2 errors, found ${JSON.stringify(before.map(i => i.level))}`);

    const buttons = await page.evaluate(() =>
        [...document.querySelectorAll('.pl-fix')].map(b => b.dataset.pl));
    if (buttons.includes('keep-text')) ok('the unreadable value offers "Keep it as text"');
    else no(`the value error offers ${JSON.stringify(buttons)}`);
    if (buttons.includes('add-unit')) ok('the unknown unit offers to add it');
    else no(`the unit error offers ${JSON.stringify(buttons)}`);

    // ---- 3. keep the core name as text ------------------------------------
    await page.evaluate(() => document.querySelector('[data-pl="keep-text"]').click());
    await pause(3200);
    const afterText = await issues();
    if (!afterText.some(i => i.level === 'error' && /MIPS32/.test(i.text)))
        ok('the value is no longer an error');
    else no('"MIPS32 M4K" is still an error after keeping it as text');
    if (afterText.some(i => i.level === 'info' && /kept as text/.test(i.text)))
        ok('and says so as a note, so the decision is visible to the next reader');
    else no(`the issues now read ${JSON.stringify(afterText.map(i => i.level))}`);

    // ---- 4. add the unit --------------------------------------------------
    await page.evaluate(() => document.querySelector('[data-pl="add-unit"]').click());
    await page.waitForSelector('.pl-unit-form', { timeout: 8000 })
        .then(() => ok('the unit form opens, with the symbol already filled in'))
        .catch(() => no('the unit form never opened'));
    const prefilled = await page.evaluate(() =>
        (document.querySelector('[data-pl="u-symbol"]') || {}).value);
    if (prefilled === 'Mbps') ok('prefilled with the symbol the datasheet used');
    else no(`the symbol box reads "${prefilled}"`);

    await page.evaluate(() => {
        const el = document.querySelector('[data-pl="u-name"]');
        el.value = 'Megabit per second';
        el.dispatchEvent(new Event('input', { bubbles: true }));
        const kind = document.querySelector('[data-pl="u-kind"]');
        kind.value = '__new';
        kind.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await pause(700);
    await page.evaluate((k) => {
        const el = document.querySelector('[data-pl="u-newkind"]');
        el.value = k;
        el.dispatchEvent(new Event('input', { bubbles: true }));
    }, KIND);
    await pause(400);
    await page.screenshot({ path: `${SHOTDIR}/2-unit-form.png` });
    await page.evaluate(() => document.querySelector('[data-pl="u-save"]').click());
    await pause(3800);

    const notice = await page.evaluate(() => {
        const n = document.querySelector('.pl-notice, .pl-ok, .pl-msg');
        return n ? n.textContent.replace(/\s+/g, ' ').trim() : '';
    });
    if (/base unit/.test(notice)) ok(`the screen says what it did: "${notice}"`);
    else no(`after adding the unit the screen said "${notice}"`);

    // ---- 5. the proposal re-validates itself ------------------------------
    const after = await issues();
    if (!after.some(i => i.level === 'error')) ok('no errors are left on the proposal');
    else no(`still showing ${JSON.stringify(after.filter(i => i.level === 'error').map(i => i.text))}`);
    await page.screenshot({ path: `${SHOTDIR}/3-cleared.png` });

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
