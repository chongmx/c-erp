/**
 * render_api_keys.mjs — Settings → Users & Access → API Keys, by clicking.
 *
 *   node tests/lib/render_api_keys.mjs ZZAK <token-file>
 *
 * The journey:
 *   1. open the page from the menu;
 *   2. Create key… → name it, tick "Comment" (which must tick and lock
 *      "Read tickets" with it), limit it to one project, 30 days → Create;
 *   3. the token is on screen, once, with Copy; it is written to <token-file>
 *      so the shell test can prove it WORKS against /api/v1;
 *   4. "I have stored it" → the token is gone from the page for good;
 *   5. the key is listed: name, prefix, permissions, project, active;
 *   6. Revoke → confirm → it reads "revoked" and has no Revoke button.
 *
 * argv: prefix, the file to write the token to, and (optional) "revoke" to do
 * step 6 — the shell test runs the driver twice so it can call the API in
 * between.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/api_keys';

const PFX     = process.argv[2] || 'ZZAK';
const OUTFILE = process.argv[3] || '/tmp/api_key_token';
const MODE    = process.argv[4] || 'create';
const NAME    = `${PFX} key`;
const PROJECT = `${PFX}P`;          // the project prefix the shell test created

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
await page.setViewport({ width: 1400, height: 1000 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
page.on('dialog',    d => d.accept());
const pause = ms => new Promise(r => setTimeout(r, ms));

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
/** The row of the key named NAME: its cells, as text. */
async function keyRow() {
    return page.evaluate((n) => {
        const tr = [...document.querySelectorAll('.ak-card:nth-of-type(n) .ak-table tbody tr, .ak-table tbody tr')]
            .find(r => r.cells[0] && r.cells[0].textContent.trim() === n);
        if (!tr) return null;
        return { cells: [...tr.cells].map(c => c.textContent.replace(/\s+/g, ' ').trim()),
                 revoke: !!tr.querySelector('[data-ak="revoke"]') };
    }, NAME);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await clickByText('Settings');
    await clickByText('Users & Access');
    await clickByText('API Keys');
    await page.waitForSelector('.ak-shell', { timeout: 10000 });
    await pause(500);
    ok('Settings → Users & Access → API Keys opens');

    if (MODE === 'create') {
        await page.click('[data-ak="open-form"]');
        await page.waitForSelector('[data-ak="name"]', { timeout: 5000 });
        await page.type('[data-ak="name"]', NAME, { delay: 10 });

        await page.click('input[data-scope="comments:write"]');
        await pause(200);
        const read = await page.$eval('input[data-scope="tickets:read"]', e => ({ on: e.checked, locked: e.disabled }));
        if (read.on && read.locked) ok('ticking Comment ticks and locks Read tickets');
        else no(`Read tickets after ticking Comment: ${JSON.stringify(read)}`);

        await page.click('[data-ak="some-projects"]');
        await page.waitForSelector(`input[data-project="${PROJECT}"]`, { timeout: 5000 });
        await page.click(`input[data-project="${PROJECT}"]`);
        await page.focus('[data-ak="expires"]');
        await page.select('[data-ak="expires"]', '30');
        await page.screenshot({ path: `${SHOTDIR}/1-form.png` });
        await page.click('[data-ak="create"]');

        try {
            await page.waitForSelector('[data-ak="token"]', { timeout: 8000 });
        } catch (_) { no('no token appeared after Create'); throw new Error('create'); }
        const token = await page.$eval('[data-ak="token"]', e => e.textContent.trim());
        if (/^cerp_[0-9a-f]{48}$/.test(token)) ok('the new key is shown once, in full');
        else no(`the shown key looks wrong: "${token}"`);
        const warns = await page.$eval('.ak-new-token', e => e.textContent);
        if (/not.*be shown again/i.test(warns)) ok('with a warning that it will not be shown again');
        else no('no "not shown again" warning');
        fs.writeFileSync(OUTFILE, token);
        await page.screenshot({ path: `${SHOTDIR}/2-token.png` });

        await clickByText('I have stored it');
        await pause(300);
        const still = await page.evaluate((t) => document.body.textContent.includes(t), token);
        if (!still) ok('dismissed, the token is gone from the page');
        else no('the full token is still on the page');

        const row = await keyRow();
        if (!row) { no('the new key is not listed'); throw new Error('row'); }
        const text = row.cells.join(' | ');
        if (text.includes(token.slice(0, 13) + '…')) ok(`listed by its prefix (${token.slice(0, 13)}…), never in full`);
        else no(`row does not show the prefix: ${text}`);
        if (text.includes('comments:write') && text.includes('tickets:read')) ok('with its permissions');
        else no(`permissions missing: ${text}`);
        if (text.includes(PROJECT)) ok(`limited to ${PROJECT}`);
        else no(`project missing: ${text}`);
        if (/active/i.test(text) && row.revoke) ok('active, with a Revoke button');
        else no(`status/revoke: ${text}`);
    } else {
        const before = await keyRow();
        if (before && before.cells.join(' ').match(/\d+ calls/)) ok(`it shows it was used (${before.cells.join(' ').match(/\d+ calls/)[0]})`);
        else no(`no use recorded on screen: ${before && before.cells.join(' | ')}`);
        await page.evaluate((n) => {
            const tr = [...document.querySelectorAll('.ak-table tbody tr')]
                .find(r => r.cells[0] && r.cells[0].textContent.trim() === n);
            tr.querySelector('[data-ak="revoke"]').click();
        }, NAME);
        try {
            await page.waitForFunction((n) => {
                const tr = [...document.querySelectorAll('.ak-table tbody tr')]
                    .find(r => r.cells[0] && r.cells[0].textContent.trim() === n);
                return tr && /revoked/i.test(tr.textContent) && !tr.querySelector('[data-ak="revoke"]');
            }, { timeout: 8000 }, NAME);
            ok('Revoke → confirm: the key reads "revoked" and cannot be revoked again');
        } catch (_) { no('the key did not become revoked on screen'); }
        await page.screenshot({ path: `${SHOTDIR}/3-revoked.png` });
    }
} catch (e) {
    no('the journey stopped: ' + e.message);
    try { await page.screenshot({ path: `${SHOTDIR}/error.png` }); } catch (_) {}
}

const real = errs.filter(e => !/favicon/.test(e));
if (real.length) no('console errors: ' + real.slice(0, 5).join(' || '));
else ok('no console errors');
await browser.close();
process.exit(failed ? 1 : 0);
