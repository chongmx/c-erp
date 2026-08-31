/**
 * render.mjs — drive a real browser at a real screen.
 *
 *   node tests/lib/render.mjs "Products" "Configuration" "Categories" .ct-shell
 *
 * Arguments: the menu path to click, then the selector that proves the screen
 * arrived. Prints a report and exits non-zero if the selector never appears or
 * the browser console reported an error.
 *
 * Why this exists: an OWL template error is SILENT server-side. Every RPC
 * returns 200 while the panel renders nothing, so an API-only test stays green
 * in front of a screen that is completely broken. Only a browser catches that.
 *
 * See tests/docs/browser-render-checks.md for the traps this file already
 * works around — chiefly that there is no hash router, so a screen is reached
 * by CLICKING, and that the session is cookie-only.
 */
const BASE = process.env.BASE || 'http://127.0.0.1:8069';
const DB = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';

const args = process.argv.slice(2);
const selector = args.length ? args[args.length - 1] : '.o_content';
const menuPath = args.slice(0, -1);
const shotPath = process.env.SHOT || '/tmp/render.png';

const puppeteer = await import('puppeteer-core');

// 1. A session, obtained the ordinary way. Chrome's CLI cannot set a cookie,
//    but a driver can, so no bootstrap page is needed here.
const authRes = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
        db: DB, login: 'admin', password: 'admin' } }),
});
const authJson = await authRes.json();
const sid = authJson?.session_id || authJson?.result?.session_id;
if (!sid) { console.log('FAIL could not authenticate'); process.exit(1); }

const browser = await puppeteer.launch({
    executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'],
});
const page = await browser.newPage();
await page.setViewport({ width: 1400, height: 900 });

// Collect everything the browser complains about. A screen that renders WITH
// an exception is not a screen that works, and this is the only place such an
// error is visible at all.
const errors = [];
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
page.on('requestfailed', r => errors.push('requestfailed: ' + r.url()));

await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
await page.goto(BASE + '/', { waitUntil: 'networkidle2' });

/**
 * Click the first visible element whose trimmed text equals `text`.
 *
 * POLLS for it. The element usually does not exist yet: the previous click is
 * still fetching, and OWL flushes on requestAnimationFrame. A single attempt
 * after a fixed wait fails whenever the machine is a little slower than the
 * day the wait was chosen — which is the worst kind of test, because it passes
 * often enough to be believed.
 */
async function clickByText(text, timeoutMs = 10000) {
    const started = Date.now();
    for (;;) {
        const ok = await page.evaluate((t) => {
            // Find the element that OWNS the text — the innermost label, not
            // the wrappers around it.
            //
            // This app's menu is <div.nav-section-wrap><div.nav-section-btn>
            // <span>Part Lookup</span>. All three have textContent
            // "Part Lookup", and document order returns the outer wrap first —
            // which carries no handler, so the click silently did nothing and
            // the screen never changed. That looked like a broken page for
            // several attempts; it was a missed target.
            const own = (e) => [...e.childNodes]
                .filter(n => n.nodeType === 3).map(n => n.textContent).join('').trim();
            const visible = (e) => e.offsetParent !== null;

            let label = [...document.querySelectorAll('*')]
                .find(e => own(e) === t && visible(e));
            if (!label) {
                // No element owns the text directly: fall back to the DEEPEST
                // element whose whole text matches, which is the closest thing
                // to a label available.
                const all = [...document.querySelectorAll('*')]
                    .filter(e => e.textContent.trim() === t && visible(e));
                label = all[all.length - 1];
            }
            if (!label) return false;

            // Walk out to whatever actually handles the click. Real controls
            // first, then the class conventions this UI uses — it is built from
            // divs, so `closest('a,button')` alone finds nothing.
            const target = label.closest('a,button,[role="button"]')
                        || label.closest('[class*="btn"],[class*="item"],[class*="menu"],[class*="tab"]')
                        || label.parentElement
                        || label;
            target.click();
            return true;
        }, text);
        if (ok) break;
        if (Date.now() - started > timeoutMs)
            throw new Error(`not found or not visible after ${timeoutMs}ms: "${text}"`);
        await new Promise(r => setTimeout(r, 200));
    }
    await new Promise(r => setTimeout(r, 300));   // let the click's render land
}

const report = { menu: [], found: false, errors };
try {
    for (const item of menuPath) { await clickByText(item); report.menu.push(item); }
    await page.waitForSelector(selector, { timeout: 15000 });
    report.found = true;
} catch (e) {
    report.failure = e.message;
}

report.dom = await page.evaluate((sel) => {
    const q = s => document.querySelector(s);
    const all = s => [...document.querySelectorAll(s)];
    return {
        present: !!q(sel),
        rows: all('.ct-row').length,
        rowText: all('.ct-row').slice(0, 5).map(r => r.textContent.trim().replace(/\s+/g, ' ')),
        twists: all('.ct-twist:not(.blank)').length,
        badges: all('.ct-n').length,
        sidebar: q('.ct-side') ? Math.round(q('.ct-side').getBoundingClientRect().width) : 0,
        grip: !!q('.ct-grip'),
        blankPanel: !!q('.ct-blank'),
        title: document.title,
    };
}, selector);

// Click the first tree row and see whether the detail panel actually fills in.
if (report.dom.rows > 0) {
    await page.click('.ct-row');
    await page.waitForSelector('.ct-panel', { timeout: 8000 }).catch(() => {});
    report.detail = await page.evaluate(() => {
        const q = s => document.querySelector(s);
        const all = s => [...document.querySelectorAll(s)];
        return {
            panel: !!q('.ct-panel'),
            title: q('.ct-h2')?.textContent.trim() || '',
            crumbs: all('.ct-crumb').length,
            stats: all('.ct-stat-n').map(e => e.textContent.trim()),
            sections: all('.ct-h3').map(e => e.textContent.trim().split('\n')[0]),
            productRows: all('.ct-table tbody tr').length,
        };
    });
}

await page.screenshot({ path: shotPath });
await browser.close();

console.log(JSON.stringify(report, null, 2));
console.log('screenshot: ' + shotPath);
process.exit(report.found && errors.length === 0 ? 0 : 1);
