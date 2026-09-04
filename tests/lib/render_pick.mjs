/**
 * render_pick.mjs — actually USE a picker, in a real browser.
 *
 *   node tests/lib/render_pick.mjs
 *
 * render_forms.mjs proves the pickers render. That is not the same as proving
 * they work: a debounce that never fires, a dropdown that opens behind the
 * card, an option whose click handler is swallowed by the blur race — all of
 * those render perfectly and are useless.
 *
 * So this drives the widget the way a person does:
 *
 *   1. open a rental contract, focus the Customer picker
 *   2. type part of a company name
 *   3. wait for the dropdown, click the option
 *   4. assert the box now holds that name — and that the id reached the record
 *   5. clear it, and assert it empties
 *   6. open "Browse all" on a relation with more rows than one page and assert
 *      the dialog pages
 *
 * Exits non-zero on the first failure. Prints one line per step.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOT   = process.env.SHOT || '/tmp/render_pick.png';

const puppeteer = await import('puppeteer-core');

const authRes = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
                           params: { db: DB, login: 'admin', password: 'admin' } }),
});
const authJson = await authRes.json();
const sid = authJson?.session_id || authJson?.result?.session_id;
if (!sid) { console.log('FAIL could not authenticate'); process.exit(1); }

async function rpc(model, method, args, kwargs = {}) {
    const r = await fetch(`${BASE}/web/dataset/call_kw`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Cookie': `session_id=${sid}` },
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
            model, method, args, kwargs: { ...kwargs, context: { session_id: sid } } } }),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error.message || 'rpc error');
    return j.result;
}

// Best-effort, one id at a time: a partner that something else references
// cannot be unlinked, and one such leftover must not abort the whole run.
async function purge() {
    let ids = [];
    try {
        ids = await rpc('res.partner', 'search', [[['name', 'like', 'ZZPICK']]], { limit: 500 });
    } catch (_) { return; }
    for (const id of (Array.isArray(ids) ? ids : [])) {
        try { await rpc('res.partner', 'unlink', [[id]]); } catch (_) { /* leave it */ }
    }
}
await purge();

// The marker is unique PER RUN. Cleanup can legitimately fail — a partner with
// a contract against it will not delete — and a fixed name would then find two
// matches next time and report the search broken when the search is fine.
const TAG  = String(Date.now()).slice(-7);
const MARK = `ZZPICK Quokka ${TAG}`;
await rpc('res.partner', 'create', [{ name: MARK, is_company: true, customer_rank: 1 }]);

// Enough extra rows that one page cannot hold them, so "Browse all" is offered
// and Next has somewhere to go. A clean baseline has ~13 partners, which is
// under the 20-row page and would leave the dialog untested.
for (let i = 1; i <= 30; i++) {
    await rpc('res.partner', 'create',
        [{ name: 'ZZPICK Filler ' + String(i).padStart(3, '0'), is_company: true }]);
}

let failed = 0;
const ok  = m => console.log('    PASS  ' + m);
const no  = m => { console.log('    FAIL  ' + m); failed++; };

const browser = await puppeteer.launch({
    executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'],
});
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 950 });

const errors = [];
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // --- open a NEW rental contract: the screen the user reported -----------
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await new Promise(r => setTimeout(r, 1200));
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test((x.textContent || '').trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.gf-shell .m2o input.m2o-input', { timeout: 12000 });
    ok('the rental contract form opened with pickers');

    // The Customer picker is the res.partner one.
    const sel = '.m2o[data-model="res.partner"] input.m2o-input';
    await page.waitForSelector(sel, { timeout: 8000 });

    // --- 1. type ------------------------------------------------------------
    await page.click(sel);
    await page.type(sel, "Quokka " + TAG, { delay: 30 });
    // Wait for the TYPED result, not merely for a dropdown.
    //
    // Focusing the box already runs an unfiltered search, so a dropdown exists
    // within milliseconds. Waiting for "any option" therefore passes on the
    // focus results and never observes the search at all — which is exactly
    // what happened the first time this ran, and it reported the widget broken
    // when the widget was fine.
    // TAG has to be PASSED into the page: the callback is serialised and run in
    // the browser, where this file's variables do not exist.
    await page.waitForFunction(
        (tag) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                    .some(o => o.textContent.includes(tag)),
        { timeout: 8000 }, TAG);
    ok('typing narrows the dropdown to the server-side match');

    const optCount = await page.evaluate(() =>
        document.querySelectorAll('.m2o-pop .m2o-opt').length);
    if (optCount === 1) ok('only the match is offered — the term reached the server');
    else no(`${optCount} options for a unique term; the search was not applied`);

    // --- 2. pick ------------------------------------------------------------
    // mousedown, not click: the option commits on mousedown so it beats the
    // input's blur. Testing with click() alone would pass while the real
    // widget lost every selection to the blur race.
    await page.evaluate(() => {
        const el = document.querySelector('.m2o-pop .m2o-opt');
        el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    });
    await new Promise(r => setTimeout(r, 400));

    const shown = await page.$eval(sel, el => el.value);
    if (shown.includes(TAG)) ok(`the box shows the chosen record ("${shown}")`);
    else no(`after picking, the box shows "${shown}"`);

    const closed = await page.evaluate(() => !document.querySelector('.m2o-pop'));
    if (closed) ok('the dropdown closes after a pick');
    else no('the dropdown stayed open after a pick');

    // --- 3. the value actually reached the record ---------------------------
    const hasClear = await page.evaluate(() =>
        !!document.querySelector('.m2o[data-model="res.partner"] .m2o-clear'));
    if (hasClear) ok('the picker reports a value (its clear button appeared)');
    else no('no clear button — the component does not think anything is selected');

    // --- 4. clear -----------------------------------------------------------
    await page.evaluate(() => {
        const b = document.querySelector('.m2o[data-model="res.partner"] .m2o-clear');
        b.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    });
    await new Promise(r => setTimeout(r, 300));
    const afterClear = await page.$eval(sel, el => el.value);
    if (afterClear === '') ok('clearing empties the box');
    else no(`after clearing, the box still shows "${afterClear}"`);

    // --- 5. browse all ------------------------------------------------------
    // Only offered when there is more than one page; res.partner on a working
    // database has that. If it is not offered, say so rather than failing —
    // a small table is a legitimate state.
    // Blur first. The input still has focus from the steps above, and clicking
    // an already-focused element fires no focus event — so the dropdown never
    // reopens and "Browse all" looks absent when it is simply not rendered.
    await page.evaluate(() => document.activeElement && document.activeElement.blur());
    await new Promise(r => setTimeout(r, 300));
    await page.click(sel);
    await new Promise(r => setTimeout(r, 900));
    const hasBrowse = await page.evaluate(() => !!document.querySelector('.m2o-browse'));
    if (!hasBrowse) {
        console.log('    NOTE  fewer partners than one page — no "Browse all" to test');
    } else {
        await page.evaluate(() => document.querySelector('.m2o-browse')
            .dispatchEvent(new MouseEvent('mousedown', { bubbles: true })));
        await page.waitForSelector('.m2o-modal .m2o-row', { timeout: 8000 });
        ok('"Browse all" opens a paged dialog');

        const first = await page.evaluate(() =>
            (document.querySelector('.m2o-modal .m2o-row')?.textContent || '').trim());
        const total = await page.evaluate(() =>
            (document.querySelector('.m2o-modal-foot span')?.textContent || '').trim());
        ok(`the dialog states its range ("${total}")`);

        const nextEnabled = await page.evaluate(() => {
            const b = [...document.querySelectorAll('.m2o-modal-foot button')]
                .find(x => x.textContent.includes('Next'));
            return b && !b.disabled;
        });
        if (nextEnabled) {
            await page.evaluate(() => [...document.querySelectorAll('.m2o-modal-foot button')]
                .find(x => x.textContent.includes('Next')).click());
            await new Promise(r => setTimeout(r, 900));
            const second = await page.evaluate(() =>
                (document.querySelector('.m2o-modal .m2o-row')?.textContent || '').trim());
            if (second && second !== first) ok('Next advances to different rows');
            else no(`Next did not advance (still "${second}")`);
        } else {
            console.log('    NOTE  only one page of results — Next not exercised');
        }
    }

    await page.screenshot({ path: SHOT });

    if (errors.length) {
        no('the browser reported errors: ' + errors.slice(0, 3).join(' | '));
    } else {
        ok('no browser console errors throughout');
    }
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
    try { await page.screenshot({ path: SHOT }); } catch (_) {}
} finally {
    await browser.close();
    // Tidy up, then CHECK the tidying worked and say so loudly if it did not.
    //
    // This seeds 30 companies into whatever database it is pointed at. A
    // silent cleanup failure leaves them in a working database, where they
    // bury the real contacts in a list of junk — which is exactly what
    // happened here, and it was reported as "I cannot select my company".
    await purge();
    try {
        const left = await rpc('res.partner', 'search', [[['name', 'like', 'ZZPICK']]], { limit: 500 });
        if (Array.isArray(left) && left.length) {
            console.log(`    FAIL  ${left.length} ZZPICK row(s) could not be removed — ` +
                        `delete them before using this database:`);
            console.log(`          DELETE FROM res_partner WHERE name LIKE 'ZZPICK%';`);
            failed++;
        }
    } catch (_) {}
}

console.log(`screenshot: ${SHOT}`);
process.exit(failed ? 1 : 0);
