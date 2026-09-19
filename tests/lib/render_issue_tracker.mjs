/**
 * render_issue_tracker.mjs — file a bug and work it, the way a developer does.
 *
 *   node tests/lib/render_issue_tracker.mjs ZZIF
 *
 * Asked for: "proper project management with issues tracker ... issues update
 * with image upload etc... assignments, similar to a jira system ... I want to
 * use it to manage the development of this c-erp."
 *
 * The journey, all clicks and typing:
 *   1. Project → Projects → New: a project with ticket prefix ZZIF;
 *   2. Task Board → pick the project → + Create;
 *   3. a Bug, priority High, with a screenshot PASTED into the description;
 *   4. Create → the ticket is ZZIF-1, the screenshot shows inline and is in
 *      the ticket's attachments;
 *   5. Status → In Progress, Assign to me, a label typed and Enter;
 *   6. a comment with an image added through "Add image" (a real file chooser);
 *   7. the history says what changed, in words;
 *   8. Back → the board card shows key, label, comment and file counts;
 *   9. the board's search, label and type filters.
 *
 * The one synthetic event is the paste: headless Chrome cannot put an image on
 * the system clipboard, so a ClipboardEvent carrying a File is dispatched on
 * the textarea — the same event a real Ctrl+V delivers. The comment image goes
 * through a genuine file chooser.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/issue_tracker';

const PFX     = process.argv[2] || 'ZZIF';
const PROJECT = `${PFX} Issue Tracker`;
const TITLE   = `${PFX} Save button stays on Saving`;
const LABEL   = `${PFX.toLowerCase()}-ui`;
const KEY     = `${PFX}-1`;

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
await page.setViewport({ width: 1500, height: 1000 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
page.on('dialog',    d => d.accept());
// Every board query, so a filter check that fails can say what was asked.
const boardCalls = [];
page.on('request', r => {
    const body = r.postData() || '';
    if (r.url().includes('call_kw') && body.includes('"board"')) {
        try { boardCalls.push(JSON.stringify(JSON.parse(body).params.args[0])); } catch (_) {}
    }
});

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
async function typeInto(sel, text) {
    await page.waitForSelector(sel, { visible: true, timeout: 10000 });
    await clearField(sel);
    await page.type(sel, text, { delay: 10 });
}
/** Select everything and delete it — a triple-click selects only a word in
 *  some inputs, which once left "zzif-" behind in the search box. */
async function clearField(sel) {
    await page.click(sel);
    await page.keyboard.down('Control');
    await page.keyboard.press('KeyA');
    await page.keyboard.up('Control');
    await page.keyboard.press('Backspace');
}
/** Choose an <option> by its visible text, as a person would. */
async function selectByText(sel, text) {
    const val = await page.$eval(sel, (s, t) => {
        const o = [...s.options].find(x => x.textContent.trim().includes(t));
        return o ? o.value : null;
    }, text);
    if (val === null) return false;
    await page.focus(sel);
    await page.select(sel, val);
    return true;
}
/** Wait until the activity feed's history contains `text`. */
async function historyHas(text, timeout = 8000) {
    try {
        await page.waitForFunction((t) =>
            [...document.querySelectorAll('.tf-track')].some(e => e.textContent.includes(t)),
            { timeout }, text);
        return true;
    } catch (_) { return false; }
}
/** Every inline image in `scope` has actually loaded (not a broken icon). */
async function imagesLoaded(scope) {
    return page.evaluate(async (s) => {
        const imgs = [...document.querySelectorAll(s + ' img.tf-inline-img')];
        await Promise.all(imgs.map(i => i.complete ? null : new Promise(r => { i.onload = i.onerror = r; })));
        return { n: imgs.length, ok: imgs.filter(i => i.naturalWidth > 0).length };
    }, scope);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });

    // A real image to attach later: a picture of the page itself.
    const IMG = `${SHOTDIR}/attach-me.png`;
    await page.screenshot({ path: IMG, clip: { x: 0, y: 0, width: 420, height: 180 } });

    // ---- 1. a project with a ticket prefix ------------------------------
    await clickByText('Project');
    await clickByText('Projects');
    await clickByText('New');
    await page.waitForSelector('[data-field="name"]', { timeout: 10000 });
    await typeInto('[data-field="name"]', PROJECT);
    if (await page.$('[data-field="task_prefix"]')) {
        await typeInto('[data-field="task_prefix"]', PFX.toLowerCase());
        ok('the project form offers a Ticket Key Prefix');
    } else no('the project form has no Ticket Key Prefix field');
    await clickByText('Create');
    await pause(1500);

    // ---- 2. the board, this project, + Create ---------------------------
    await clickByText('Task Board');
    await page.waitForSelector('.tb-shell .tb-sel', { timeout: 10000 });
    await pause(600);
    if (await selectByText('.tb-head .tb-sel', PROJECT)) ok(`the board lists the new project`);
    else { no('the new project is not in the board\'s project picker'); throw new Error('no project'); }
    await pause(900);
    await page.click('[data-tb="create"]');
    await page.waitForSelector('[data-tf="new-name"]', { timeout: 10000 });
    await pause(700);
    const proj = await page.$eval('.tf-new .m2o input.m2o-input', e => e.value).catch(() => '');
    if (proj.includes(PROJECT)) ok('Create opens a new ticket already in the board\'s project');
    else no(`the new ticket's project reads "${proj}"`);

    // ---- 3. a bug, High, with a pasted screenshot --------------------------
    await typeInto('[data-tf="new-name"]', TITLE);
    await selectByText('[data-tf="new-type"]', 'Bug');
    await selectByText('[data-tf="new-priority"]', 'High');
    await typeInto('[data-tf="new-desc"]', 'Steps: open Settings, press Save.\nExpected: Saved. Actual: stuck.\n');
    // The paste: the File is built in the page from the PNG on disk.
    const b64 = fs.readFileSync(IMG).toString('base64');
    await page.focus('[data-tf="new-desc"]');
    await page.evaluate((data) => {
        const bin = atob(data);
        const bytes = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
        const file = new File([bytes], 'image.png', { type: 'image/png' });
        const dt = new DataTransfer();
        dt.items.add(file);
        const ta = document.querySelector('[data-tf="new-desc"]');
        ta.selectionStart = ta.selectionEnd = ta.value.length;
        ta.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true }));
    }, b64);
    try {
        await page.waitForFunction(() =>
            /!\[screenshot-\d{8}-\d{6}\.png\]\(\/web\/content\/\d+\)/.test(
                document.querySelector('[data-tf="new-desc"]').value), { timeout: 10000 });
        ok('pasting a screenshot uploads it and puts a reference in the description');
    } catch (_) { no('the pasted screenshot never appeared in the description'); }
    await pause(400);
    let im = await imagesLoaded('.tf-preview');
    if (im.n === 1 && im.ok === 1) ok('and the preview shows the image');
    else no(`the preview shows ${im.ok}/${im.n} images`);
    await page.screenshot({ path: `${SHOTDIR}/1-create.png` });

    // ---- 4. Create -----------------------------------------------------------
    await page.click('[data-tf="create"]');
    try {
        await page.waitForFunction((k) => {
            const e = document.querySelector('.tf-crumb .tf-key');
            return e && e.textContent.trim() === k;
        }, { timeout: 10000 }, KEY);
        ok(`the ticket is created as ${KEY}`);
    } catch (_) { no(`no ${KEY} on screen after Create`); throw new Error('create'); }
    await pause(600);
    im = await imagesLoaded('.tf-desc');
    if (im.n === 1 && im.ok === 1) ok('the screenshot shows inline in the description');
    else no(`the description shows ${im.ok}/${im.n} images`);
    const thumbs = await page.$$eval('.tf-thumb', els => els.length);
    if (thumbs === 1) ok('and it is in the ticket\'s attachments');
    else no(`the attachments show ${thumbs} files, expected 1`);

    // ---- 5. status, assignee, label ---------------------------------------
    await selectByText('[data-tf="stage"]', 'In Progress');
    if (await historyHas('Status: New → In Progress')) ok('Status → In Progress, and the history says so');
    else no('no "Status: New → In Progress" in the history');

    await page.click('[data-tf="assign-me"]');
    if (await historyHas('Assignee: — →')) ok('Assign to me, and the history says so');
    else no('assigning did not reach the history');

    await page.click('[data-tf="tag"]');
    await page.type('[data-tf="tag"]', LABEL, { delay: 10 });
    await page.keyboard.press('Enter');
    try {
        await page.waitForFunction((l) =>
            [...document.querySelectorAll('.tf-tag')].some(e => e.dataset.tag === l), { timeout: 8000 }, LABEL);
        ok(`typing a label and Enter adds it ("${LABEL}")`);
    } catch (_) { no('the label chip never appeared'); }

    // ---- 6. a comment with an image through the file chooser -------------
    await page.click('[data-tf="comment"]');
    await page.type('[data-tf="comment"]', 'Reproduced here:\n', { delay: 5 });
    const [chooser] = await Promise.all([
        page.waitForFileChooser({ timeout: 8000 }),
        page.click('[data-tf="comment-image"]'),
    ]);
    await chooser.accept([IMG]);
    try {
        await page.waitForFunction(() =>
            /\/web\/content\/\d+/.test(document.querySelector('[data-tf="comment"]').value), { timeout: 10000 });
        ok('"Add image" puts the uploaded image into the comment');
    } catch (_) { no('the chosen image never reached the comment'); }
    await page.click('[data-tf="post"]');
    try {
        await page.waitForFunction(() => document.querySelectorAll('.tf-entry.tf-comment').length === 1,
                                   { timeout: 8000 });
        ok('the comment is posted');
    } catch (_) { no('the comment did not appear in the feed'); }
    await pause(500);
    im = await imagesLoaded('.tf-entry.tf-comment');
    if (im.n === 1 && im.ok === 1) ok('and its image shows inline');
    else no(`the comment shows ${im.ok}/${im.n} images`);
    const who = await page.$eval('.tf-entry.tf-comment .tf-entry-h b', e => e.textContent.trim()).catch(() => '');
    if (who) ok(`signed by the person who wrote it ("${who}")`);
    else no('the comment has no author on screen');

    // Hit-test: the composer and the sidebar are really on screen.
    const visible = await page.evaluate(() => {
        const hit = (sel) => {
            const el = document.querySelector(sel);
            if (!el) return false;
            el.scrollIntoView({ block: 'center' });
            const r = el.getBoundingClientRect();
            const at = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
            return at === el || el.contains(at);
        };
        return { stage: hit('[data-tf="stage"]'), post: hit('[data-tf="post"]') };
    });
    if (visible.stage && visible.post) ok('the status picker and the Comment button are on screen, not covered');
    else no(`covered or missing: ${JSON.stringify(visible)}`);
    await page.evaluate(() => document.querySelector('.tf-main').scrollTo(0, 0));
    await pause(300);
    await page.screenshot({ path: `${SHOTDIR}/2-ticket.png` });
    await page.evaluate(() => {
        const m = document.querySelector('.tf-main');
        m.scrollTo(0, m.scrollHeight);
    });
    await pause(300);
    await page.screenshot({ path: `${SHOTDIR}/3-activity.png` });

    // ---- 8. back to the board ---------------------------------------------
    await clickByText('← Back');
    await page.waitForSelector('.tb-shell', { timeout: 10000 });
    try {
        await page.waitForFunction((k) => !!document.querySelector(`.tb-card[data-key="${k}"]`),
                                   { timeout: 10000 }, KEY);
        ok('Back returns to the board, still on this project');
    } catch (_) { no(`back on the board, ${KEY} is not there`); }
    const card = await page.evaluate((k) => {
        const c = document.querySelector(`.tb-card[data-key="${k}"]`);
        if (!c) return null;
        return {
            col: c.closest('.tb-col').querySelector('.tb-col-n').textContent.trim(),
            text: c.textContent.replace(/\s+/g, ' ').trim(),
            tags: [...c.querySelectorAll('.tb-tag')].map(t => t.textContent.trim()),
            bug: !!c.querySelector('.tf-type-bug'),
        };
    }, KEY);
    if (card && card.col === 'In Progress') ok('the card sits in In Progress');
    else no(`the card is in "${card && card.col}"`);
    if (card && card.bug && card.tags.includes(LABEL)) ok('it shows the bug mark and the label');
    else no(`card marks: ${JSON.stringify(card)}`);
    if (card && card.text.includes('💬 1') && card.text.includes('📎 2'))
        ok('and one comment, two files');
    else no(`card counts read: ${card && card.text}`);
    await page.screenshot({ path: `${SHOTDIR}/4-board.png` });

    // ---- 9. the filters --------------------------------------------------------
    const cards = () => page.$$eval('.tb-card', els => els.map(e => e.dataset.key));
    await typeInto('[data-tb="search"]', KEY.toLowerCase());
    await pause(900);
    let ks = await cards();
    if (ks.length === 1 && ks[0] === KEY) ok('search by key finds it');
    else no(`search "${KEY.toLowerCase()}" shows ${JSON.stringify(ks)}`);
    await typeInto('[data-tb="search"]', 'nothing-matches-this');
    await pause(900);
    ks = await cards();
    if (ks.length === 0) ok('a search that matches nothing shows nothing');
    else no(`an unmatched search shows ${JSON.stringify(ks)}`);
    await clearField('[data-tb="search"]');
    await pause(900);
    const q = await page.$eval('[data-tb="search"]', e => e.value);
    if (q === '') ok('the search box clears');
    else no(`the search box still reads "${q}"`);
    await selectByText('[data-tb="type"]', 'Feature');
    await pause(900);
    ks = await cards();
    if (!ks.includes(KEY)) ok('filtering by Feature hides the bug');
    else no('the Feature filter still shows the bug');
    await selectByText('[data-tb="type"]', 'All types');
    await selectByText('[data-tb="label"]', LABEL);
    await pause(900);
    ks = await cards();
    if (ks.length === 1 && ks[0] === KEY) ok('filtering by the label shows exactly it');
    else no(`the label filter shows ${JSON.stringify(ks)}; last board queries: ${boardCalls.slice(-4).join(' | ')}`);
} catch (e) {
    no('the journey stopped: ' + e.message);
    try { await page.screenshot({ path: `${SHOTDIR}/error.png` }); } catch (_) {}
}

const real = errs.filter(e => !/favicon/.test(e));
if (real.length) no('console errors: ' + real.slice(0, 5).join(' || '));
else ok('no console errors');

await browser.close();
process.exit(failed ? 1 : 0);
