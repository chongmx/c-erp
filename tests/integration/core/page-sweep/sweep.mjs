/**
 * sweep.mjs — open every screen in the product and check it renders.
 *
 * WHY THIS EXISTS
 *
 * A one-line change to a getter every generic list depends on (`altViews`,
 * docs/128) treated a field MAP as a list. `{}.some` is undefined, so the
 * getter threw, OWL swallowed the exception, and every list view in the ERP
 * rendered blank — with no console error to say why.
 *
 * Nothing caught it. The whole suite passed: it drives the HTTP API, and the
 * API was fine. The damage was entirely in the browser, on screens no test
 * ever opened. It was found by a person clicking around.
 *
 * So this walks the menu the way a person does — every app tile, every entry
 * on every app's bar, every child of every dropdown — and asserts each screen
 * puts something on the page and logs no errors.
 *
 * It is deliberately SHALLOW. It does not check what a screen shows, only that
 * it shows something, because the failure it exists to catch is a white
 * rectangle. A deep assertion per screen would be a second copy of every other
 * test in the suite and would rot.
 *
 * Prints one JSON report; test.sh asserts on it. Skips cleanly without Chrome.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN  || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';

const report = { ok: false, apps: 0, visited: 0, blank: [], errored: [] };
const done = (c) => { console.log(JSON.stringify(report)); process.exit(c); };

let puppeteer;
try { puppeteer = await import('puppeteer-core'); }
catch { report.skipped = 'puppeteer-core not installed'; done(0); }
const { existsSync } = await import('node:fs');
if (!existsSync(CHROME)) { report.skipped = 'chrome not found'; done(0); }

const sid = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }),
}).then(r => r.json()).then(j => j?.result?.session_id).catch(() => '');
if (!sid) { report.errored.push('admin auth failed'); done(1); }

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const p = await browser.newPage();
let errs = [];
p.on('pageerror', e => errs.push('pageerror: ' + e.message.slice(0, 120)));
p.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text().slice(0, 120)); });
await p.setCacheEnabled(false);
await p.setViewport({ width: 1500, height: 950 });
await p.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });

const home = async () => {
    await p.goto(BASE + '/login', { waitUntil: 'networkidle2' });
    await new Promise(r => setTimeout(r, 2600));
};
const clickText = (label) => p.evaluate((l) => {
    const t = [...document.querySelectorAll('*')]
        .find(e => e.children.length === 0 && e.textContent.trim() === l);
    if (t) { t.click(); return true; }
    return false;
}, label);

const results = [];
try {
    await home();
    const tiles = await p.evaluate(() =>
        [...document.querySelectorAll('.app-tile, .home-tile, [class*=tile]')]
            .map(t => t.textContent.trim().replace(/^[^\w]+/, ''))
            .filter(Boolean));
    const apps = [...new Set(tiles)];
    report.apps = apps.length;

    for (const app of apps) {
        await home();
        if (!await clickText(app)) {
            results.push({ app, leaf: '(tile not found)', chars: 0, rows: 0,
                           custom: false, errs: 0, ok: false });
            continue;
        }
        await new Promise(r => setTimeout(r, 1800));

        const sections = await p.evaluate(() =>
            [...document.querySelectorAll('.nav-section-btn')]
                .map((b, i) => ({ i, label: b.textContent.trim().replace(/[▾▸]/g, '').trim() })));

        for (const s of sections) {
            await p.evaluate((i) => {
                const b = document.querySelectorAll('.nav-section-btn')[i];
                if (b) b.click();
            }, s.i);
            await new Promise(r => setTimeout(r, 700));

            const kids = await p.evaluate(() =>
                [...document.querySelectorAll('.dropdown-menu')]
                    .flatMap(d => [...d.children].map(c => c.textContent.trim()))
                    .filter(Boolean));

            for (const kid of (kids.length ? kids : [null])) {
                errs = [];
                if (kid) {
                    await p.evaluate((i) => {
                        const b = document.querySelectorAll('.nav-section-btn')[i];
                        if (b) b.click();
                    }, s.i);
                    await new Promise(r => setTimeout(r, 400));
                    await p.evaluate((k) => {
                        const el = [...document.querySelectorAll('.dropdown-menu *')]
                            .find(n => n.children.length === 0 && n.textContent.trim() === k);
                        if (el) el.click();
                    }, kid);
                }
                await new Promise(r => setTimeout(r, 1900));

                const seen = await p.evaluate(() => {
                    const main = document.querySelector('.action-view, .main-content, main')
                              || document.body;
                    const txt = main.innerText.trim();
                    return {
                        rows: document.querySelectorAll('table tbody tr').length,
                        // A custom screen may legitimately have few words but
                        // a lot of structure — a grid, a switcher, a canvas.
                        custom: !!document.querySelector('[class*="-wrap"], [class*="-grid"], .rv-switch'),
                        chars: txt.length,
                    };
                });
                const blank = seen.chars < 40 && seen.rows === 0 && !seen.custom;
                results.push({ app, leaf: kid || s.label, ...seen,
                               errs: errs.length, ok: !blank && errs.length === 0 });
            }
        }
    }
} catch (e) {
    report.errored.push('sweep: ' + e.message);
} finally {
    await browser.close();
}

report.visited = results.length;
report.blank   = results.filter(r => !r.ok && !r.errs).map(r => r.app + ' -> ' + r.leaf);
report.errored = report.errored.concat(
    results.filter(r => r.errs).map(r => r.app + ' -> ' + r.leaf));
// A sweep that visited almost nothing is a broken sweep, not a clean product.
report.ok = report.visited > 40 && !report.blank.length && !report.errored.length;
done(report.ok ? 0 : 1);
