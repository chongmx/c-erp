/**
 * attack.mjs — drive the editor's OWN JavaScript as an unauthorised caller.
 *
 * website-editor.js is a public static file. Anyone can fetch it, define the
 * `window.__WSITE_EDIT` object it looks for, and run it. So the interesting
 * question is not "can a visitor run the editor" — they can, and this proves
 * it — but "does the server care".
 *
 * That distinction is the whole point of the client not being a security
 * boundary. Hiding the toolbar is presentation; the save endpoint is the
 * control. This forces the toolbar to appear for people who must not have it
 * and then checks that pressing Save changes nothing.
 *
 * Two attackers:
 *   * a visitor with no session at all         → expect 401
 *   * an employee with a real staff login and  → expect 403
 *     no website permission
 */
const BASE = process.env.BASE || 'http://127.0.0.1:8069';
const DB   = process.env.DBN  || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SLUG = process.env.PEN_SLUG || 'pen-public';
const PAGE_ID = parseInt(process.env.PEN_PAGE || '0', 10);
const EMP_LOGIN = process.env.EMP_LOGIN || '';
const EMP_PASS  = process.env.EMP_PASS  || '';

const report = { ok: false, steps: {}, errors: [] };
const done = (c) => { console.log(JSON.stringify(report)); process.exit(c); };

let puppeteer;
try { puppeteer = await import('puppeteer-core'); }
catch { report.skipped = 'puppeteer-core not installed'; done(0); }
const { existsSync } = await import('node:fs');
if (!existsSync(CHROME)) { report.skipped = 'chrome not found'; done(0); }
if (!PAGE_ID) { report.errors.push('PEN_PAGE not supplied'); done(1); }

const before = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });

// Force the editor to load and run on a page the server did NOT arm it for.
async function forceEditor(sid, marker) {
    const ctx = await browser.createBrowserContext();
    const page = await ctx.newPage();
    const errs = [];
    page.on('pageerror', e => errs.push(e.message));
    if (sid) await page.setCookie({ name: 'session_id', value: sid,
                                    domain: '127.0.0.1', path: '/' });
    await page.goto(`${BASE}/site/${SLUG}`, { waitUntil: 'networkidle2' });

    const armedByServer = await page.evaluate(() => typeof window.__WSITE_EDIT !== 'undefined');

    // Define what the server refused to, then inject the real script.
    const ran = await page.evaluate(async (pid) => {
        window.__WSITE_EDIT = { page_id: pid, admin: true };   // claim to be admin
        const r = await fetch('/website-editor.js');
        if (!r.ok) return { loadable: false };
        const src = await r.text();
        const s = document.createElement('script');
        s.textContent = src;
        document.body.appendChild(s);
        return { loadable: true, barAppeared: !!document.querySelector('.wse-bar') };
    }, PAGE_ID);

    // Now drive it exactly as a real editor would: enter edit mode, change the
    // text, press Save.
    let saveStatus = 0;
    if (ran.barAppeared) {
        await page.evaluate(() => document.getElementById('wse-edit').click());
        await page.waitForFunction(
            () => document.querySelector('[contenteditable="true"]'), { timeout: 8000 }
        ).catch(() => {});
        await page.evaluate((m) => {
            const el = document.querySelector('[contenteditable="true"]');
            if (el) { el.textContent = m; el.dispatchEvent(new Event('input', { bubbles: true })); }
        }, marker);
        await page.evaluate(() => document.getElementById('wse-save').click());
        await new Promise(r => setTimeout(r, 1500));
    }

    // Regardless of the UI, ask the endpoint directly too — the UI might not
    // even have got that far, and the endpoint is what actually matters.
    saveStatus = await page.evaluate(async (pid, m) => {
        const r = await fetch(`/site/api/page/${pid}/blocks`, {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ blocks: [{ type: 'text', text: m }] }),
        });
        return r.status;
    }, PAGE_ID, marker);

    await ctx.close();
    return { armedByServer, ...ran, saveStatus, errs };
}

try {
    // --- attacker 1: a visitor, no session ---
    const anon = await forceEditor('', 'DEFACED-BY-ANON');
    report.steps.editorLoadable  = anon.loadable === true;
    report.steps.editorRanAsAnon = anon.barAppeared === true;
    report.steps.serverDidNotArm = anon.armedByServer === false;
    report.steps.anonSaveRefused = anon.saveStatus === 401;
    const afterAnon = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
    report.steps.anonPageUnchanged = !afterAnon.includes('DEFACED-BY-ANON');

    // --- attacker 2: a real employee with no website permission ---
    if (EMP_LOGIN) {
        const sid = await fetch(`${BASE}/web/session/authenticate`, {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
                params: { db: DB, login: EMP_LOGIN, password: EMP_PASS } }),
        }).then(r => r.json()).then(j => j?.result?.session_id);
        if (sid) {
            const emp = await forceEditor(sid, 'DEFACED-BY-EMPLOYEE');
            report.steps.empSaveRefused = emp.saveStatus === 403;
            const afterEmp = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
            report.steps.empPageUnchanged = !afterEmp.includes('DEFACED-BY-EMPLOYEE');
        } else {
            report.errors.push('could not sign in the employee');
        }
    }

    // Nothing the client can set grants anything: the page is byte-identical
    // to how it started.
    const finalHtml = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
    report.steps.noPrivFromClient = finalHtml === before;
} catch (e) {
    report.errors.push('attack: ' + e.message);
} finally {
    await browser.close();
}

const s = report.steps;
report.ok = s.editorLoadable && s.editorRanAsAnon && s.anonSaveRefused &&
            s.anonPageUnchanged && s.noPrivFromClient &&
            (EMP_LOGIN ? (s.empSaveRefused && s.empPageUnchanged) : true);
done(report.ok ? 0 : 1);
