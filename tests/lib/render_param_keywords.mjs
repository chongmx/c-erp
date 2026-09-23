/**
 * render_param_keywords.mjs — Products ▸ Configuration ▸ Parameter Keywords.
 *
 *   node tests/lib/render_param_keywords.mjs ZZPK
 *
 * CERP-8: "when new parameter types emerges, how should we handle it? should
 * we create a new one? or should we put it under our existing parameters? …
 * also, I need a configuration page for me to look at and configure the list
 * of supported parameters keywords."
 *
 * So this drives the page, by clicking:
 *
 *   1. the menu — it is where a configuration screen should be, next to Part
 *      Units and Footprints
 *   2. "Needs a decision" lists the names in the catalogue nobody has ruled
 *      on, each with what the server thinks it is
 *   3. ADD AS NEW on one of them — it becomes a parameter of its own
 *   4. MERGE INTO on the other — the parts that used it are renamed, and the
 *      old spelling is kept so the question never comes back
 *   5. "Try a name" answers for a name that is not in the catalogue at all
 *
 * Step 4 is the one worth having: it writes to product data, so the screen
 * asks first, and the row has to disappear afterwards — a decision that looks
 * undone is a decision somebody makes twice.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/param_keywords';

const PFX   = process.argv[2] || 'ZZPK';
const ADOPT = `${PFX} Ripple Current`;     // nothing like it — adopt it
const MERGE = `${PFX} Ohmic Value`;        // belongs under Resistance

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
// Merging rewrites product rows, so the screen asks first. An unhandled
// confirm() freezes the page rather than merely skipping the click.
page.on('dialog', async d => { await d.accept(); });

const pause = ms => new Promise(r => setTimeout(r, ms));

async function openMenu(section, leaf) {
    await page.evaluate(() => { const b = document.querySelector('.nav-home-btn'); if (b) b.click(); });
    await pause(700);
    await page.evaluate(() => {
        const t = [...document.querySelectorAll('.app-tile')]
            .find(x => /Products/.test(x.textContent || ''));
        if (t) t.click();
    });
    await pause(1400);
    // A section with children renders name + caret in one button, so the
    // label span is what to compare against.
    const gotSection = await page.evaluate((s) => {
        const btn = [...document.querySelectorAll('.nav-section-btn')]
            .find(x => { const l = x.querySelector('span'); return l && l.textContent.trim() === s; });
        if (!btn) return false;
        btn.click(); return true;
    }, section);
    if (!gotSection) return `no "${section}" section in the Products menu`;
    await pause(500);
    const gotLeaf = await page.evaluate((s) => {
        const el = [...document.querySelectorAll('.dropdown-item')]
            .find(x => (x.textContent || '').trim() === s);
        if (!el) return false;
        el.click(); return true;
    }, leaf);
    if (!gotLeaf) return `no "${leaf}" under ${section}`;
    await pause(2200);
    return null;
}

const unmatchedRows = () => page.evaluate(() =>
    [...document.querySelectorAll('[data-unmatched]')].map(r => ({
        name: r.dataset.unmatched,
        text: r.textContent.replace(/\s+/g, ' ').trim(),
    })));

const keywordNames = () => page.evaluate(() =>
    [...document.querySelectorAll('[data-keyword]')].map(r => r.dataset.keyword));

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1. the screen is where a configuration screen belongs ------------
    const err = await openMenu('Configuration', 'Parameter Keywords');
    if (err) { no(err); throw new Error(err); }
    await page.waitForSelector('.pk-shell', { timeout: 12000 });
    await pause(1200);
    ok('Products ▸ Configuration ▸ Parameter Keywords opens from the menu');

    const known = await keywordNames();
    if (known.includes('Resistance') && known.includes('Capacitance'))
        ok(`the vocabulary is listed (${known.length} parameters)`);
    else no(`the vocabulary reads ${JSON.stringify(known.slice(0, 8))}`);

    const aliasCount = await page.evaluate(() =>
        document.querySelectorAll('.pk-chip').length);
    if (aliasCount > 0) ok(`each parameter shows the other spellings that mean it (${aliasCount} on screen)`);
    else no('no alias chips are shown');
    await page.screenshot({ path: `${SHOTDIR}/1-vocabulary.png` });

    // ---- 2. what needs deciding ------------------------------------------
    let rows = await unmatchedRows();
    const adoptRow = rows.find(r => r.name === ADOPT);
    const mergeRow = rows.find(r => r.name === MERGE);
    if (adoptRow && mergeRow) ok('both unmatched names from the catalogue are listed for a decision');
    else { no(`the list shows ${JSON.stringify(rows.map(r => r.name))}`); throw new Error('rows'); }

    if (/new/.test(adoptRow.text)) ok(`"${ADOPT}" is reported as new`);
    else no(`"${ADOPT}" was described as: ${adoptRow.text}`);
    if (/close to/.test(mergeRow.text) && /Resistance/.test(mergeRow.text))
        ok(`"${MERGE}" is reported as close to Resistance — the suggestion the ticket asked for`);
    else no(`"${MERGE}" was described as: ${mergeRow.text}`);
    await page.screenshot({ path: `${SHOTDIR}/2-decisions.png` });

    // ---- 3. add as new ----------------------------------------------------
    const adopted = await page.evaluate((n) => {
        const row = [...document.querySelectorAll('[data-unmatched]')].find(r => r.dataset.unmatched === n);
        if (!row) return false;
        const b = row.querySelector('[data-pk="adopt"]');
        if (!b) return false;
        b.click(); return true;
    }, ADOPT);
    if (!adopted) { no('the row offers no "Add as new"'); throw new Error('adopt'); }
    await pause(2600);
    if ((await keywordNames()).includes(ADOPT)) ok('"Add as new" adopts it into the vocabulary');
    else no(`after adopting, the vocabulary is ${JSON.stringify(await keywordNames())}`);
    if (!(await unmatchedRows()).some(r => r.name === ADOPT))
        ok('and it leaves the decisions list — the question is settled');
    else no('the adopted name is still listed as undecided');

    // ---- 4. merge into an existing parameter ------------------------------
    const merged = await page.evaluate((n) => {
        const row = [...document.querySelectorAll('[data-unmatched]')].find(r => r.dataset.unmatched === n);
        if (!row) return null;
        const sel = row.querySelector('select.pk-select');
        if (!sel) return null;
        const opt = [...sel.options].find(o => o.textContent.trim() === 'Resistance');
        if (!opt) return null;
        sel.value = opt.value;
        sel.dispatchEvent(new Event('change', { bubbles: true }));
        return opt.textContent.trim();
    }, MERGE);
    if (merged === 'Resistance') ok('the row offers the suggested parameter first in "Merge into…"');
    else { no(`could not pick Resistance to merge into (got ${JSON.stringify(merged)})`); throw new Error('merge'); }
    await pause(3000);

    const notice = await page.evaluate(() => {
        const n = document.querySelector('.pk-notice');
        return n ? n.textContent.trim() : '';
    });
    if (/Renamed on 1 part/.test(notice)) ok(`the screen says what it changed: "${notice}"`);
    else no(`after merging the screen said "${notice}"`);
    if (!(await unmatchedRows()).some(r => r.name === MERGE))
        ok('and that name no longer needs deciding');
    else no('the merged name is still listed as undecided');
    await page.screenshot({ path: `${SHOTDIR}/3-merged.png` });

    // ---- 5. try a name ----------------------------------------------------
    await page.type('[data-pk="try"]', 'Tolerence', { delay: 30 });
    await pause(1800);
    const verdict = await page.evaluate(() => {
        const el = document.querySelector('.pk-try-out');
        return el ? el.textContent.trim() : '';
    });
    if (/Tolerance/.test(verdict)) ok(`a misspelling is recognised on the spot: "${verdict}"`);
    else no(`"Tolerence" was judged: "${verdict}"`);

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
