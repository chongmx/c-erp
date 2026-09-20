/**
 * render_company_settings.mjs — Settings → ERP Settings edits the COMPANY.
 *
 *   node tests/lib/render_company_settings.mjs ZZCS "<company name>" MYR USD
 *
 * Reported: "how to configure the company currency? I should be able to
 * configure the company currency without any manual sql editing".
 *
 * The home currency gets a combo box on General. On the way there it turned
 * out General and Banking were dead: they read and wrote ir_config_parameter,
 * which nothing has read since identity moved onto res_company (docs/094) and
 * which startup deletes — so the screen opened with every box blank, and what
 * was typed never reached a document.
 *
 * The journey, all clicks and typing:
 *   1. open it from the menu; the boxes show the company as it IS;
 *   2. the combo box offers the active currencies, with the home one chosen;
 *   3. type a registration number, pick another currency, Save;
 *   4. Banking: type an account number, Save;
 *   5. reload and open it again — both edits are still there;
 *   6. Precision & Currency marks the NEW home currency as the base.
 *
 * argv: prefix, the company name expected on screen, the home currency code it
 * starts on, and the code to switch to. The shell test reads those from the
 * database and re-checks the database afterwards.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/company_settings';

const PFX     = process.argv[2] || 'ZZCS';
const CO_NAME = process.argv[3] || '';
const FROM    = process.argv[4] || 'MYR';
const TO      = process.argv[5] || 'USD';
const REG     = `${PFX}-REG-1`;
const ACCT    = `${PFX}-ACCT-1`;
const NEWCUR  = process.argv[6] || 'ZZK';   // a currency the test adds through ＋

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

/** Click the first visible element that owns `text` (polls — see render.mjs). */
async function clickByText(text, timeoutMs = 10000) {
    const started = Date.now();
    for (;;) {
        const done = await page.evaluate((t) => {
            const own = (e) => [...e.childNodes]
                .filter(n => n.nodeType === 3).map(n => n.textContent).join('').trim();
            const visible = (e) => e.offsetParent !== null;
            let label = [...document.querySelectorAll('*')].find(e => own(e) === t && visible(e));
            if (!label) {
                const all = [...document.querySelectorAll('*')]
                    .filter(e => e.textContent.trim() === t && visible(e));
                label = all[all.length - 1];
            }
            if (!label) return false;
            const target = label.closest('a,button,[role="button"]')
                        || label.closest('[class*="btn"],[class*="item"],[class*="menu"],[class*="tab"]')
                        || label.parentElement || label;
            target.click();
            return true;
        }, text);
        if (done) break;
        if (Date.now() - started > timeoutMs)
            throw new Error(`not found or not visible after ${timeoutMs}ms: "${text}"`);
        await pause(200);
    }
    await pause(300);
}

/** The input in the settings row labelled `label`. */
async function fieldHandle(label) {
    const h = await page.evaluateHandle((l) => {
        const row = [...document.querySelectorAll('.erp-field-row')]
            .find(r => (r.querySelector('.erp-field-label')?.textContent || '').trim() === l);
        return row ? row.querySelector('input, select') : null;
    }, label);
    return h.asElement();
}
async function fieldValue(label) {
    const el = await fieldHandle(label);
    return el ? el.evaluate(e => e.value) : null;
}
/** Select what is there, then TYPE — the way a person replaces a value. */
async function typeInto(label, text) {
    const el = await fieldHandle(label);
    if (!el) return false;
    // Select all and delete: a triple-click selects only a word in some
    // inputs, which left the old value in front of the new one.
    await el.click();
    await page.keyboard.down('Control');
    await page.keyboard.press('KeyA');
    await page.keyboard.up('Control');
    await page.keyboard.press('Backspace');
    if (text) await el.type(text, { delay: 15 });
    return true;
}
const PICKER = '.erp-currency-pick input.m2o-input';
/** What the Home Currency picker shows, and what it offers when opened. */
async function combo(open) {
    if (open) {
        await page.click(PICKER);
        await pause(900);          // it searches the server on focus
    }
    return page.evaluate((sel) => {
        const el = document.querySelector(sel);
        if (!el) return null;
        return { shown: el.value.trim(),
                 options: [...document.querySelectorAll('.erp-currency-pick .m2o-opt')]
                            .map(o => o.textContent.trim()) };
    }, PICKER);
}
/** Choose a currency by typing its code and clicking the row — as a person does. */
async function pickCurrency(code) {
    await page.click(PICKER);
    await pause(400);
    await page.type(PICKER, code, { delay: 40 });
    try {
        await page.waitForFunction((c) =>
            [...document.querySelectorAll('.erp-currency-pick .m2o-opt')]
                .some(o => o.textContent.trim().startsWith(c)), { timeout: 8000 }, code);
    } catch (_) { return false; }
    return page.evaluate((c) => {
        const o = [...document.querySelectorAll('.erp-currency-pick .m2o-opt')]
            .find(x => x.textContent.trim().startsWith(c));
        if (!o) return false;
        o.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
        return true;
    }, code);
}
/** Is the element at its own centre actually the element? (not covered/clipped) */
async function hitTest(sel) {
    return page.evaluate((s) => {
        const el = document.querySelector(s);
        if (!el) return false;
        const r = el.getBoundingClientRect();
        if (!r.width || !r.height) return false;
        const at = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
        return at === el || el.contains(at);
    }, sel);
}
/** Press Save Changes; report "Saved!" or the error the screen shows. */
async function save() {
    await clickByText('Save Changes');
    try {
        await page.waitForFunction(() =>
            document.querySelector('.erp-save-row .erp-saved-ok, .erp-save-row .erp-save-err'),
            { timeout: 10000 });
    } catch (_) { return { err: 'neither "Saved!" nor an error appeared' }; }
    return page.evaluate(() => {
        const e = document.querySelector('.erp-save-row .erp-save-err');
        return e ? { err: e.textContent.trim() } : { saved: true };
    });
}
async function openSettings() {
    await clickByText('Settings');
    await clickByText('ERP Settings');
    await page.waitForSelector(PICKER, { timeout: 15000 });
    await pause(400);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await openSettings();
    ok('Settings → ERP Settings opens by clicking the menu');

    // ---- 1. the company as it is ------------------------------------------
    const nm = await fieldValue('Company Name');
    if (CO_NAME && nm === CO_NAME) ok(`Company Name shows the company ("${nm}")`);
    else no(`Company Name shows "${nm}", expected "${CO_NAME}" — the screen is not reading the company`);

    // ---- 2. the picker ------------------------------------------------------
    let c = await combo(false);
    if (!c) { no('there is no Home Currency picker'); throw new Error('no picker'); }
    if (c.shown.startsWith(FROM)) ok(`Home Currency shows ${c.shown}`);
    else no(`Home Currency shows "${c.shown}", expected ${FROM}`);
    if (await hitTest(PICKER)) ok('the picker is on screen, not covered');
    else no('the picker is hidden or covered');
    c = await combo(true);
    if (c.options.some(o => o.startsWith(TO))) ok(`opening it offers ${TO} (${c.options.join(' | ')})`);
    else no(`${TO} is not offered: ${c.options.join(' | ')}`);
    // The ones in use come first — that is the "frequently used" list.
    const inUse = c.options.findIndex(o => o.includes('not in use yet'));
    if (inUse !== 0) ok('currencies already in use are listed first');
    else no(`the first entry is one not in use: ${c.options.join(' | ')}`);
    await page.screenshot({ path: `${SHOTDIR}/1-general.png` });

    // ---- 3. type, pick, save ------------------------------------------------
    if (await typeInto('Registration No.', REG)) ok(`typed Registration No. "${REG}"`);
    else no('there is no Registration No. box');
    if (await pickCurrency(TO)) ok(`typed "${TO}" and picked it from the list`);
    else no(`typing "${TO}" offered nothing to pick`);
    await pause(300);
    c = await combo(false);
    if (c.shown.startsWith(TO)) ok(`the picker now shows ${TO}`);
    else no(`after picking, the picker shows "${c.shown}"`);
    let r = await save();
    if (r.saved) ok('Save Changes → "Saved!"');
    else no(`saving General failed: "${r.err}"`);
    await page.screenshot({ path: `${SHOTDIR}/2-saved.png` });

    // ---- 4. Banking ---------------------------------------------------------
    await clickByText('Banking');
    await page.waitForFunction(() =>
        [...document.querySelectorAll('.erp-field-label')].some(l => l.textContent.trim() === 'Account Number'),
        { timeout: 8000 });
    if (await typeInto('Account Number', ACCT)) ok(`typed Account Number "${ACCT}"`);
    else no('there is no Account Number box');
    r = await save();
    if (r.saved) ok('Banking saves');
    else no(`saving Banking failed: "${r.err}"`);

    // ---- 4b. the Save button cannot stick on "Saving…" -------------------
    // Reported: "Settings -> ALL: After clicking 'Save Changes' the button got
    // stuck at 'Saving'." The page wrote one call per setting and nothing
    // bounded how long a call could take. Now: one call for the page, and a
    // request that does not answer fails with something readable.
    await clickByText('General');
    await pause(400);
    const calls = [];
    page.on('request', r => {
        const b = r.postData() || '';
        if (r.url().includes('call_kw') && b.includes('ir.config.parameter')) calls.push(b.slice(0, 120));
    });
    await typeInto('Address Line 3', `${PFX} floor 9`);
    let r2 = await save();
    if (r2.saved) ok('saving General still works');
    else no(`saving General failed: "${r2.err}"`);
    if (calls.length === 1) ok('and the whole page went in ONE call, not one per setting');
    else no(`the page took ${calls.length} ir.config.parameter calls`);

    await page.evaluate(() => { window.UI_TIMING = window.UI_TIMING || {}; window.UI_TIMING.rpcTimeout = 1; });
    await typeInto('Address Line 3', `${PFX} floor 10`);
    r2 = await save();
    if (r2.err && /did not answer/i.test(r2.err)) ok('a request that never answers ends as an error, not a stuck button');
    else no(`with a 1 ms limit the save reported: ${JSON.stringify(r2)}`);
    const stuck = await page.$$eval('.erp-save-row button', els => els.some(b => /Saving/.test(b.textContent)));
    if (!stuck) ok('and the button says "Save Changes" again, ready to retry');
    else no('the button is still on "Saving…"');
    await page.evaluate(() => { window.UI_TIMING.rpcTimeout = 45000; });
    await typeInto('Address Line 3', '');
    r2 = await save();
    if (r2.saved) ok('the retry saves');
    else no(`the retry failed: "${r2.err}"`);

    // ---- 5. reload: the edits are the company's now ----------------------
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await openSettings();
    c = await combo(false);
    if (c.shown.startsWith(TO)) ok(`after a reload the Home Currency is still ${TO}`);
    else no(`after a reload the Home Currency shows "${c.shown}" — the pick was not kept`);
    const reg = await fieldValue('Registration No.');
    if (reg === REG) ok('and the Registration No. is still there');
    else no(`after a reload Registration No. reads "${reg}"`);
    await clickByText('Banking');
    await pause(400);
    const acct = await fieldValue('Account Number');
    if (acct === ACCT) ok('and so is the bank Account Number');
    else no(`after a reload Account Number reads "${acct}"`);

    // ---- 6. the base currency follows -------------------------------------
    await clickByText('Precision & Currency');
    try {
        await page.waitForFunction(() => document.querySelectorAll('.erp-country-code').length > 0,
            { timeout: 10000 });
    } catch (_) {}
    const base = await page.evaluate(() => {
        const rows = [...document.querySelectorAll('.erp-field-row')]
            .filter(r => r.querySelector('.erp-country-code'));
        const b = rows.find(r => /\bbase\b/.test(r.querySelector('.erp-field-label').textContent));
        if (!b) return null;
        return { code: b.querySelector('.erp-field-label span').textContent.trim(),
                 locked: !!b.querySelector('input')?.disabled };
    });
    if (base && base.code === TO) ok(`Precision & Currency marks ${TO} as the base`);
    else no(`the base currency shown is ${JSON.stringify(base)}, expected ${TO}`);
    if (base && base.locked) ok('and its rate is locked at 1.0');
    else no('the base rate is editable');
    await page.screenshot({ path: `${SHOTDIR}/3-currencies.png` });
    // ---- 7. a currency c-erp does not know yet --------------------------
    // "allow user to create new or search" — the ＋ beside the picker.
    await clickByText('General');
    await pause(400);
    await page.click('[data-erp="new-currency"]');
    await page.waitForSelector('[data-erp="cur-code"]', { timeout: 5000 });
    await page.type('[data-erp="cur-code"]', NEWCUR.toLowerCase(), { delay: 20 });
    await page.type('[data-erp="cur-symbol"]', 'Z$', { delay: 20 });
    await page.click('[data-erp="cur-rate"]');
    await page.keyboard.down('Control'); await page.keyboard.press('KeyA'); await page.keyboard.up('Control');
    await page.type('[data-erp="cur-rate"]', '2.5', { delay: 20 });
    await page.click('[data-erp="cur-save"]');
    try {
        await page.waitForFunction((sel, c) => {
            const el = document.querySelector(sel);
            return el && el.value.trim().startsWith(c);
        }, { timeout: 8000 }, PICKER, NEWCUR);
        ok(`＋ adds ${NEWCUR} and selects it, typed in lower case`);
    } catch (_) {
        const err = await page.$$eval('.erp-save-err', els => els.map(e => e.textContent.trim()));
        no(`adding a currency did not take: ${JSON.stringify(err)}`);
    }
    // A code that is not three letters is refused, on screen.
    await page.click('[data-erp="new-currency"]');
    await page.waitForSelector('[data-erp="cur-code"]', { timeout: 5000 });
    await page.type('[data-erp="cur-code"]', 'zz', { delay: 20 });
    await page.click('[data-erp="cur-save"]');
    await pause(500);
    const curErr = await page.$$eval('.erp-save-err', els => els.map(e => e.textContent.trim()).join(' '));
    if (/three letters/i.test(curErr)) ok('a code that is not three letters is refused, with a reason');
    else no(`a two-letter code was accepted or unexplained: "${curErr}"`);
    await page.screenshot({ path: `${SHOTDIR}/4-currency.png` });
} catch (e) {
    no('the journey stopped: ' + e.message);
    try { await page.screenshot({ path: `${SHOTDIR}/error.png` }); } catch (_) {}
}

if (errs.length) no('console errors: ' + errs.slice(0, 5).join(' || '));
else ok('no console errors');

await browser.close();
process.exit(failed ? 1 : 0);
