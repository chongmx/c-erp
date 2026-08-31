/**
 * drive.mjs — Part Lookup, driven through real Chrome.
 *
 * The API tests either side of this one pass happily while the screen renders
 * nothing: an OWL template is parsed as XML in the CLIENT, so a bad template
 * throws in the browser and is completely invisible server-side. This is the
 * only check that would catch it.
 *
 * It also covers the things that only exist in the browser — that the editor's
 * inputs are really editable, that Save round-trips, that the Apply button
 * reflects the state machine — none of which an RPC can observe.
 *
 * Uses the `mock` provider, so it never touches the network. Prints one JSON
 * report; test.sh asserts on it.
 */
const BASE = process.env.BASE || 'http://127.0.0.1:8069';
const DB = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOT = process.env.SHOT || '/tmp/part-lookup.png';

const puppeteer = await import('puppeteer-core');

const authRes = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }),
});
const sid = (await authRes.json())?.result?.session_id;
if (!sid) { console.log(JSON.stringify({ failure: 'could not authenticate' })); process.exit(1); }

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1400, height: 1000 });

const errors = [];
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
page.on('requestfailed', r => errors.push('requestfailed: ' + r.url()));

await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
await page.goto(BASE + '/', { waitUntil: 'networkidle2' });

// Click the element that OWNS the text, then walk out to whatever handles the
// click — this UI is built from divs, so closest('button') alone finds nothing.
// See tests/docs/browser-render-checks.md.
async function clickByText(text, timeoutMs = 12000) {
    const started = Date.now();
    for (;;) {
        const ok = await page.evaluate((t) => {
            const own = (e) => [...e.childNodes].filter(n => n.nodeType === 3)
                .map(n => n.textContent).join('').trim();
            const vis = (e) => e.offsetParent !== null;
            let l = [...document.querySelectorAll('*')].find(e => own(e) === t && vis(e));
            if (!l) {
                const all = [...document.querySelectorAll('*')]
                    .filter(e => e.textContent.trim() === t && vis(e));
                l = all[all.length - 1];
            }
            if (!l) return false;
            (l.closest('a,button,[role="button"]')
             || l.closest('[class*="btn"],[class*="item"],[class*="menu"],[class*="tab"]')
             || l.parentElement || l).click();
            return true;
        }, text);
        if (ok) break;
        if (Date.now() - started > timeoutMs)
            throw new Error(`not found or not visible after ${timeoutMs}ms: "${text}"`);
        await new Promise(r => setTimeout(r, 200));
    }
    await new Promise(r => setTimeout(r, 350));
}

/**
 * Replace an input's value, firing the event OWL actually listens for.
 *
 * Triple-click-then-type does NOT work here: OWL re-renders on each keystroke
 * and re-assigns `value`, which collapses the selection, so the select-all is
 * gone by the time Backspace lands. That produced "Mock ManufactureQA-UI…" —
 * one character deleted and the rest appended. Setting the value through the
 * native setter and dispatching `input` is what a real edit looks like to the
 * framework, without depending on selection surviving a render.
 */
async function retype(handle, value) {
    await page.evaluate((el, v) => {
        const setter = Object.getOwnPropertyDescriptor(
            window.HTMLInputElement.prototype, 'value').set;
        setter.call(el, v);
        el.dispatchEvent(new Event('input', { bubbles: true }));
    }, handle, value);
    await new Promise(r => setTimeout(r, 200));
}

const out = { errors };
try {
    await clickByText('Products');
    await clickByText('Part Lookup');
    await page.waitForSelector('.pl-ask-in', { timeout: 15000 });
    out.reached = true;

    // ---- ask ------------------------------------------------------------
    await page.type('.pl-ask-in', 'QA-UI-DRIVE');
    await clickByText('Ask the agent');
    await page.waitForSelector('.pl-cand', { timeout: 60000 });
    out.candidates = await page.$$eval('.pl-cand', els => els.length);
    out.badge = await page.$eval('.pl-badge', e => e.textContent.trim()).catch(() => null);
    out.notes = !!(await page.$('.pl-notes'));
    // The mock deliberately answers "4k7" + "kΩ" — the double-multiplier the
    // server corrects. The warning must reach the reviewer, not just the log.
    out.adjustedShown = await page.$eval('.pl-cand-adj', e => e.textContent.trim()).catch(() => null);
    await page.screenshot({ path: SHOT });

    // ---- stage ----------------------------------------------------------
    await clickByText('Stage for review');
    await page.waitForSelector('.pl-edit input', { timeout: 20000 });
    out.editorFields = await page.$$eval('.pl-edit > label', els => els.map(e => e.textContent.trim()));
    out.confBadge = await page.$eval('.pl-d-head .pl-cbadge',
        e => ({ text: e.textContent.trim(), cls: e.className })).catch(() => null);
    out.paramRows = await page.$$eval('.pl-table tbody tr', rows => rows.length);

    // ---- edit and save --------------------------------------------------
    const fields = await page.$$('.pl-edit input');
    await retype(fields[1], 'QA-UI Edited Mfr');          // Manufacturer
    await clickByText('Save changes');
    await new Promise(r => setTimeout(r, 900));
    out.savedNotice = await page.$eval('.pl-notice', e => e.textContent.trim()).catch(() => null);
    out.mfrAfterSave = await page.$$eval('.pl-edit input', els => els[1].value);

    // ---- adding and removing a parameter --------------------------------
    await clickByText('Add a parameter');
    out.rowsAfterAdd = await page.$$eval('.pl-table tbody tr', rows => rows.length);

    // ---- make it invalid, and watch Apply refuse to offer ----------------
    const pinputs = await page.$$('.pl-table input');
    await retype(pinputs[2], 'furlongs');                  // first row's unit
    await clickByText('Save changes');
    await new Promise(r => setTimeout(r, 900));
    out.applyAfterInvalid = await page.$eval('.pl-actions .pl-btn.primary',
        e => ({ label: e.textContent.trim(), disabled: e.disabled, title: e.title }));
    out.issuesShown = await page.$$eval('.pl-issue', els => els.length);
    await page.screenshot({ path: SHOT.replace(/\.png$/, '-invalid.png') });
} catch (e) {
    out.failure = String(e.message || e);
    await page.screenshot({ path: SHOT }).catch(() => {});
}
console.log(JSON.stringify(out, null, 2));
await browser.close();
process.exit(out.failure || errors.length ? 1 : 0);
