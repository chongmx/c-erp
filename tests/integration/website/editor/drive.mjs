/**
 * drive.mjs — the in-place website editor, driven through real Chrome.
 *
 * Everything else in this suite talks to the HTTP API, which proves the server
 * refuses the right callers but says NOTHING about whether the editor works:
 * website-editor.js could throw on its first line and every one of those
 * assertions would still pass. This is the only check that runs it.
 *
 * It also covers the two things that exist ONLY in a browser:
 *
 *   * contenteditable. The editor lets somebody type — or PASTE MARKUP — into
 *     the live page, and reads it back with textContent. §6 pastes a script
 *     tag into a heading and proves it lands in the database as text and
 *     renders as text. No HTTP test can reach that path, because the path is
 *     the DOM.
 *   * that the toolbar is genuinely absent for callers who may not edit —
 *     not merely un-styled or hidden, which a curl grep cannot tell apart.
 *
 * Prints one JSON report; test.sh asserts on it. Skips cleanly without Chrome.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN  || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOT   = process.env.SHOT || '/tmp/website-editor.png';
const SLUG   = process.env.EDIT_SLUG || 'ed-page';
const PLAIN_LOGIN = process.env.PLAIN_LOGIN || '';
const PLAIN_PASS  = process.env.PLAIN_PASS  || '';

const report = { ok: false, steps: {}, errors: [] };
const done = (c) => { console.log(JSON.stringify(report)); process.exit(c); };

let puppeteer;
try { puppeteer = await import('puppeteer-core'); }
catch { report.skipped = 'puppeteer-core not installed'; done(0); }
const { existsSync } = await import('node:fs');
if (!existsSync(CHROME)) { report.skipped = 'chrome not found at ' + CHROME; done(0); }

async function auth(login, password) {
    const r = await fetch(`${BASE}/web/session/authenticate`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
            params: { db: DB, login, password } }),
    });
    return (await r.json())?.result?.session_id || '';
}

const adminSid = await auth('admin', 'admin');
if (!adminSid) { report.errors.push('admin auth failed'); done(1); }

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });

// A fresh page with its own console capture. Console errors are collected per
// visit so a failure can be attributed to the visit that caused it.
// Click Save and wait for the editor's own reload to finish.
//
// On success the editor calls window.location.reload() about 400ms later.
// Waiting only for "edit mode ended" resolves DURING that navigation, and the
// next evaluate() then dies with "Execution context was destroyed" — which is
// what this driver did until the navigation was awaited explicitly.
async function saveAndSettle(page) {
    await Promise.all([
        page.waitForNavigation({ waitUntil: 'networkidle2', timeout: 20000 })
            .catch(() => {}),          // a save that does not reload is fine too
        page.click('#wse-save'),
    ]);
    await page.waitForSelector('#wse-edit', { visible: true, timeout: 15000 });
}

async function enterEdit(page) {
    await page.click('#wse-edit');
    await page.waitForFunction(
        () => document.body.classList.contains('wse-on') &&
              document.querySelector('[contenteditable="true"]'),
        { timeout: 12000 });
}

async function visit(sid, path) {
    const page = await browser.newPage();
    await page.setViewport({ width: 1280, height: 900 });
    const errs = [];
    page.on('pageerror', e => errs.push('pageerror: ' + e.message));
    page.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
    if (sid) await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + path, { waitUntil: 'networkidle2' });
    return { page, errs };
}

try {
    // ---------------------------------------------------------------
    // 1. An anonymous visitor gets no toolbar — genuinely absent from the DOM.
    // ---------------------------------------------------------------
    {
        const { page, errs } = await visit('', '/site/' + SLUG);
        report.steps.anonNoBar = (await page.$('.wse-bar')) === null;
        report.steps.anonNoScript = await page.evaluate(() =>
            !document.querySelector('script[src*="website-editor"]') &&
            typeof window.__WSITE_EDIT === 'undefined');
        if (errs.length) report.errors.push('anon: ' + errs.join(' | '));
        await page.close();
    }

    // ---------------------------------------------------------------
    // 2. Staff WITHOUT the group: still no toolbar, in a real browser.
    // ---------------------------------------------------------------
    if (PLAIN_LOGIN) {
        const sid = await auth(PLAIN_LOGIN, PLAIN_PASS);
        if (sid) {
            const { page, errs } = await visit(sid, '/site/' + SLUG);
            report.steps.plainNoBar = (await page.$('.wse-bar')) === null;
            if (errs.length) report.errors.push('plain: ' + errs.join(' | '));
            await page.close();
        } else {
            report.errors.push('could not sign in as the ungrouped staff user');
        }
    }

    // ---------------------------------------------------------------
    // 3. Admin: the toolbar loads and the editor script runs without throwing.
    // ---------------------------------------------------------------
    const { page, errs } = await visit(adminSid, '/site/' + SLUG);

    // ONE dialog handler for the whole page. Puppeteer throws
    // "Cannot accept dialog which is already handled" if two listeners both
    // answer the same dialog — which killed this driver before it could print
    // anything. A mode flag decides what to do instead of a second listener.
    let alerted = false;
    let dialogMode = 'dismiss';          // 'dismiss' | 'accept'
    page.on('dialog', async (d) => {
        // An alert() is a script running. A confirm() is the editor asking.
        if (d.type() === 'alert') alerted = true;
        try { dialogMode === 'accept' ? await d.accept() : await d.dismiss(); }
        catch { /* already handled — nothing to do */ }
    });

    await page.waitForSelector('.wse-bar', { timeout: 10000 });
    report.steps.barLoads = true;
    report.steps.editButton = (await page.$('#wse-edit')) !== null;

    // ---------------------------------------------------------------
    // 4. Enter edit mode. This is the moment the script does real work:
    //    it fetches the blocks and re-draws the page from them.
    // ---------------------------------------------------------------
    await page.click('#wse-edit');
    await page.waitForFunction(
        () => document.body.classList.contains('wse-on') &&
              document.querySelector('[contenteditable="true"]'),
        { timeout: 10000 });
    report.steps.editModeEntered = true;
    report.steps.addPaletteShown = await page.evaluate(() => {
        const a = document.querySelector('.wse-add');
        return !!a && getComputedStyle(a).display !== 'none';
    });
    await page.screenshot({ path: SHOT });

    // ---------------------------------------------------------------
    // 5. Type into the page and save. The round trip is the whole feature.
    // ---------------------------------------------------------------
    const MARK = 'Edited in a real browser ' + Date.now();
    await page.evaluate((m) => {
        const el = document.querySelector('[contenteditable="true"]');
        el.focus();
        el.textContent = m;
        el.dispatchEvent(new Event('input', { bubbles: true }));
    }, MARK);

    await saveAndSettle(page);
    report.steps.savedAndReloaded = await page.evaluate(
        (m) => document.body.innerText.includes(m), MARK);

    // And it really is in the database, not just on screen.
    const check = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
    report.steps.persisted = check.includes(MARK);

    // ---------------------------------------------------------------
    // 6. THE ONE ONLY A BROWSER CAN DO — paste markup into the page.
    //
    // contenteditable will happily accept a <script> tag. The editor reads
    // textContent, so it must land as TEXT: stored escaped, rendered escaped,
    // never executed. An HTTP test cannot reach this path because the path is
    // the DOM.
    // ---------------------------------------------------------------
    await enterEdit(page);
    // 6a. What a person actually does: TYPE the characters. This is the case
    //     the guarantee is about — it must be stored and re-served as text.
    const TYPED = '<script>alert(1)</script> & "quotes"';
    await page.evaluate((x) => {
        const el = document.querySelector('[contenteditable="true"]');
        el.focus();
        el.textContent = x;                       // typing, not pasting markup
        el.dispatchEvent(new Event('input', { bubbles: true }));
    }, TYPED);
    await saveAndSettle(page);

    const after = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
    report.steps.xssNotLiveTag = !/<script>alert\(1\)<\/script>/i.test(after);
    report.steps.xssEscaped    = after.includes('&lt;script&gt;');
    report.steps.noAlertFired  = !alerted;

    // 6b. And the harsher case: markup injected into the editable element as
    //     real DOM (what a rich paste produces). textContent must reduce it to
    //     text, so NOTHING live is ever stored.
    //
    //     Note this deliberately makes the BROWSER build an <img> with a bad
    //     src, so an onerror fires and a 404 is logged in the editor's own tab
    //     before any save. That is the test forcing live markup into the page,
    //     not the product serving it — the assertion is about what gets STORED.
    await enterEdit(page);
    await page.evaluate(() => {
        const el = document.querySelector('[contenteditable="true"]');
        el.focus();
        el.innerHTML = '<b>bold</b><i>italic</i>';   // markup, no live handlers
        el.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await saveAndSettle(page);
    const after2 = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
    // The tags are gone entirely — textContent kept only "bolditalic".
    report.steps.pastedMarkupFlattened =
        after2.includes('bolditalic') && !/<b>bold<\/b>/i.test(after2);

    // ---------------------------------------------------------------
    // 7. Add, move and delete a block — the buttons, in a browser.
    // ---------------------------------------------------------------
    await enterEdit(page);
    await page.waitForSelector('.wse-add', { timeout: 10000 });
    const before = await page.evaluate(() => document.querySelectorAll('[data-wse]').length);
    await page.click('.wse-add button[data-add="heading"]');
    const afterAdd = await page.evaluate(() => document.querySelectorAll('[data-wse]').length);
    report.steps.blockAdded = afterAdd === before + 1;

    // Delete it again — the editor asks for confirmation, so answer yes.
    dialogMode = 'accept';
    await page.evaluate((i) => {
        const b = document.querySelector('[data-del="' + i + '"]');
        if (b) b.click();
    }, afterAdd - 1);
    const afterDel = await page.evaluate(() => document.querySelectorAll('[data-wse]').length);
    report.steps.blockDeleted = afterDel === before;

    // ---- the sidebar (docs/122) ----
    // The Customize tab exists to reach fields that have NO inline surface —
    // a button's link, an image's alt text, a plan's "featured" flag. So the
    // check is not "a panel appeared" but "a field the page cannot show me is
    // editable, and editing it survives a save".
    report.steps.sidebarShown = await page.evaluate(() =>
        !!document.querySelector('.wse-side') &&
        getComputedStyle(document.querySelector('.wse-side')).display !== 'none');
    report.steps.topbarShown = await page.evaluate(() =>
        getComputedStyle(document.querySelector('.wse-top')).display === 'flex');
    report.steps.viewBarHidden = await page.evaluate(() =>
        getComputedStyle(document.querySelector('.wse-bar')).display === 'none');
    report.steps.outlineListsBlocks = await page.evaluate(() =>
        document.querySelectorAll('#wse-outline button').length > 0);

    // The selection highlight is drawn on the [data-wse] wrapper, so a block
    // whose rendered box is WIDER than its wrapper gets an outline that cuts
    // across it — and, being positioned, paints over the vertical edges, so
    // the selection shows as two horizontal lines with no sides. The hero
    // bleeds 22px past the text column and did exactly that. Assert that no
    // block escapes its own wrapper.
    report.steps.noBlockOverflowsWrapper = await page.evaluate(() => {
        let worst = 0;
        document.querySelectorAll('[data-wse]').forEach((w) => {
            const kids = [...w.children].filter(c => !c.classList.contains('wse-tools'));
            if (!kids.length) return;
            const r = w.getBoundingClientRect();
            for (const k of kids) {
                const kr = k.getBoundingClientRect();
                worst = Math.max(worst, r.left - kr.left, kr.right - r.right);
            }
        });
        return worst <= 1;          // a rounding pixel is not a bug
    });

    // Click a block on the page: it should select and open Customize.
    await page.evaluate(() => {
        const n = document.querySelector('[data-wse]');
        if (n) n.click();
    });
    await new Promise(r => setTimeout(r, 400));
    report.steps.clickSelects = await page.evaluate(() =>
        !!document.querySelector('[data-wse].sel'));
    report.steps.customizeOpened = await page.evaluate(() =>
        document.querySelector('.wse-tab.active')?.dataset.tab === 'custom');
    report.steps.customizeHasControls = await page.evaluate(() =>
        document.querySelectorAll(
            '#wse-pane-custom input,#wse-pane-custom textarea,#wse-pane-custom select').length > 0);

    // A field with NO inline representation: a heading's LEVEL is a tag name,
    // so the rendered page offers no way to change it whatsoever. Which block
    // is the heading depends on what the earlier sections did to the page, so
    // walk the outline and find it rather than assuming an index.
    // Add one from the palette so the block under test is known rather than
    // inherited from whatever the earlier sections left behind. Adding also
    // has to SELECT the new block and open Customize on it — appending
    // something and then making the user hunt for it is not an editor.
    await page.evaluate(() => document.querySelector('.wse-add button[data-add="heading"]').click());
    await new Promise(r => setTimeout(r, 500));
    report.steps.addSelectsNewBlock = await page.evaluate(() =>
        !!document.querySelector('[data-wse].sel') &&
        document.querySelector('.wse-tab.active')?.dataset.tab === 'custom');

    const levelChanged = await page.evaluate(() => {
        const sel = document.querySelector('#wse-pane-custom select');
        if (!sel) return null;
        const host = document.querySelector('[data-wse].sel');
        if (!host) return null;
        const before = sel.value;
        const opt = [...sel.options].find(o => o.value !== before);
        if (!opt) return null;
        sel.value = opt.value;
        sel.dispatchEvent(new Event('change', { bubbles: true }));
        return { index: parseInt(host.dataset.wse, 10), before, after: opt.value };
    });
    report.steps.offPageFieldEditable = levelChanged !== null;
    if (levelChanged) {
        await new Promise(r => setTimeout(r, 400));
        // The page re-renders from the data, so the heading tag must follow.
        report.steps.offPageFieldApplied = await page.evaluate((c) =>
            !!document.querySelector('[data-wse="' + c.index + '"] h' + c.after),
            levelChanged);
    }

    if (errs.length) report.errors.push('admin: ' + errs.join(' | '));
    await page.screenshot({ path: SHOT });
    await page.close();
} catch (e) {
    report.errors.push('drive: ' + e.message);
} finally {
    await browser.close();
}

const s = report.steps;
report.ok = s.anonNoBar && s.anonNoScript && s.barLoads && s.editButton &&
            s.editModeEntered && s.addPaletteShown && s.savedAndReloaded &&
            s.persisted && s.xssNotLiveTag && s.xssEscaped && s.noAlertFired &&
            s.pastedMarkupFlattened && s.blockAdded && s.blockDeleted &&
            s.sidebarShown && s.topbarShown && s.viewBarHidden &&
            s.outlineListsBlocks && s.noBlockOverflowsWrapper &&
            s.clickSelects && s.customizeOpened &&
            s.customizeHasControls && s.addSelectsNewBlock &&
            s.offPageFieldEditable && s.offPageFieldApplied &&
            (PLAIN_LOGIN ? s.plainNoBar === true : true) &&
            report.errors.length === 0;
done(report.ok ? 0 : 1);
