/**
 * drive.mjs — the admin-issued password-reset link, driven through real Chrome.
 *
 * The API journey next to this one proves the /web/reset_password COMPLETION
 * route works. What it cannot see is the panel a real user actually uses: the
 * login page reads the token out of the URL and renders a "set a new password"
 * form. That template is parsed as XML in the CLIENT, so a mistake in it throws
 * in the browser and is completely invisible server-side — every RPC still
 * answers 200 while the panel renders nothing. This is the only check that
 * catches it.
 *
 * The whole loop, end to end:
 *   admin mints a reset link (RPC) → open it as an anonymous visitor →
 *   the reset panel appears → type a new password → submit →
 *   the success state shows → the new password actually signs in.
 *
 * Prints one JSON report; test.sh asserts on it. Skips cleanly (report.skipped)
 * if Chrome is not installed, so the suite still runs on a headless box.
 */
const BASE = process.env.BASE || 'http://127.0.0.1:8069';
const DB   = process.env.DBN  || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOT = process.env.SHOT || '/tmp/account-reset.png';
const LOGIN = process.env.RESET_LOGIN;   // set by test.sh — the user to reset
const NEWPW = process.env.RESET_NEWPW || 'Browser-Set-9';

const report = { ok: false, steps: {}, errors: [] };
const done = (code) => { console.log(JSON.stringify(report)); process.exit(code); };

let puppeteer;
try { puppeteer = await import('puppeteer-core'); }
catch { report.skipped = 'puppeteer-core not installed'; done(0); }

import { existsSync } from 'node:fs';
if (!existsSync(CHROME)) { report.skipped = 'chrome not found at ' + CHROME; done(0); }
if (!LOGIN) { report.errors.push('RESET_LOGIN not provided'); done(1); }

// 1. Admin mints a reset link for the target user (the only way a token exists).
const authRes = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }),
});
const adminSid = (await authRes.json())?.result?.session_id;
if (!adminSid) { report.errors.push('admin auth failed'); done(1); }

const uidRes = await fetch(`${BASE}/web/dataset/call_kw`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'Cookie': `session_id=${adminSid}` },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
        model: 'res.users', method: 'search_read',
        args: [[['login', '=', LOGIN]]], kwargs: { fields: ['id'], context: { session_id: adminSid } } } }),
});
const uid = (await uidRes.json())?.result?.[0]?.id;
if (!uid) { report.errors.push('could not find user ' + LOGIN); done(1); }

const genRes = await fetch(`${BASE}/web/dataset/call_kw`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'Cookie': `session_id=${adminSid}` },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
        model: 'res.users', method: 'action_generate_reset_link',
        args: [[uid]], kwargs: { context: { session_id: adminSid } } } }),
});
const gen = (await genRes.json())?.result;
if (!gen?.reset_url) { report.errors.push('no reset_url from generator'); done(1); }
report.steps.linkMinted = true;

// The generator builds the URL from web.base.url, which may be a non-local
// host; drive the LOCAL server with the same query string.
const q = gen.reset_url.split('?')[1] || '';
const targetUrl = `${BASE}/?${q}`;

// 2. Open the link as an anonymous visitor (no session cookie at all).
const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
try {
    const page = await browser.newPage();
    await page.setViewport({ width: 1200, height: 900 });
    page.on('pageerror', e => report.errors.push('pageerror: ' + e.message));
    page.on('console', m => { if (m.type() === 'error') report.errors.push('console: ' + m.text()); });

    await page.goto(targetUrl, { waitUntil: 'networkidle2' });

    // 3. The reset panel must render. Its heading names the login.
    await page.waitForFunction(
        (lg) => document.body.innerText.includes('Set a new password') &&
                document.body.innerText.includes(lg),
        { timeout: 12000 }, LOGIN);
    report.steps.panelRendered = true;

    // 4. Fill the two password fields and submit.
    const pwInputs = await page.$$('input[type="password"]');
    if (pwInputs.length < 2) { report.errors.push('expected two password inputs, got ' + pwInputs.length); }
    await pwInputs[0].type(NEWPW);
    await pwInputs[1].type(NEWPW);
    await page.screenshot({ path: SHOT });

    // Click the "Set password" button (a div-built button carrying that text).
    const clicked = await page.evaluate(() => {
        const vis = (e) => e.offsetParent !== null;
        const btn = [...document.querySelectorAll('button')].find(
            e => e.textContent.trim() === 'Set password' && vis(e));
        if (!btn) return false;
        btn.click(); return true;
    });
    if (!clicked) { report.errors.push('Set password button not found'); }

    // 5. The success state appears.
    await page.waitForFunction(
        () => document.body.innerText.includes('Password updated'),
        { timeout: 12000 });
    report.steps.successShown = true;
    await page.screenshot({ path: SHOT });
} catch (e) {
    report.errors.push('drive: ' + e.message);
} finally {
    await browser.close();
}

// 6. Confirm the browser-set password actually authenticates.
const verify = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: LOGIN, password: NEWPW } }),
});
report.steps.newPasswordWorks = !!(await verify.json())?.result?.session_id;

report.ok = report.steps.linkMinted && report.steps.panelRendered &&
            report.steps.successShown && report.steps.newPasswordWorks &&
            report.errors.length === 0;
done(report.ok ? 0 : 1);
