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
    await el.click({ clickCount: 3 });
    await page.keyboard.press('Backspace');
    await el.type(text, { delay: 15 });
    return true;
}
/** What the Home Currency combo box shows, and what it offers. */
async function combo() {
    return page.evaluate(() => {
        const s = document.querySelector('select.erp-home-currency');
        if (!s) return null;
        const o = s.options[s.selectedIndex];
        return { shown: o ? o.textContent.trim() : '',
                 options: [...s.options].map(x => x.textContent.trim()) };
    });
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
    await page.waitForSelector('select.erp-home-currency', { timeout: 15000 });
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

    // ---- 2. the combo box ---------------------------------------------------
    let c = await combo();
    if (!c) { no('there is no Home Currency combo box'); throw new Error('no combo'); }
    if (c.shown.startsWith(FROM)) ok(`Home Currency shows ${c.shown}`);
    else no(`Home Currency shows "${c.shown}", expected ${FROM}`);
    if (c.options.some(o => o.startsWith(TO))) ok(`it offers ${TO} (options: ${c.options.join(' | ')})`);
    else no(`${TO} is not offered: ${c.options.join(' | ')}`);
    if (await hitTest('select.erp-home-currency')) ok('the combo box is on screen, not covered');
    else no('the combo box is hidden or covered');
    await page.screenshot({ path: `${SHOTDIR}/1-general.png` });

    // ---- 3. type, pick, save ------------------------------------------------
    if (await typeInto('Registration No.', REG)) ok(`typed Registration No. "${REG}"`);
    else no('there is no Registration No. box');
    const toVal = await page.evaluate((code) => {
        const s = document.querySelector('select.erp-home-currency');
        const o = [...s.options].find(x => x.textContent.trim().startsWith(code));
        return o ? o.value : null;
    }, TO);
    await page.focus('select.erp-home-currency');
    await page.select('select.erp-home-currency', toVal);
    await pause(200);
    c = await combo();
    if (c.shown.startsWith(TO)) ok(`picked ${TO} in the combo box`);
    else no(`after picking, the combo box shows "${c.shown}"`);
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

    // ---- 5. reload: the edits are the company's now ----------------------
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await openSettings();
    c = await combo();
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
} catch (e) {
    no('the journey stopped: ' + e.message);
    try { await page.screenshot({ path: `${SHOTDIR}/error.png` }); } catch (_) {}
}

if (errs.length) no('console errors: ' + errs.slice(0, 5).join(' || '));
else ok('no console errors');

await browser.close();
process.exit(failed ? 1 : 0);
