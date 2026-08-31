/**
 * journey.mjs — a day's editing, in a real browser.
 *
 * Every other test in this suite proves a PIECE works: the endpoint refuses the
 * right callers, the renderer escapes text, the sniffer rejects an SVG. None of
 * them proves the thing people actually do works, because what they do is a
 * sequence — add a block, type in it, upload a picture, move something, delete
 * something, save — and each step happens in a DOM that the previous step
 * rebuilt.
 *
 * That sequence is where the bugs live. `draw()` re-renders the page from data
 * after every change, so a stale index, a lost selection or a harvest that runs
 * against the wrong DOM shows up here and nowhere else.
 *
 * The last third is the part that matters most: it reloads the page with NO
 * session and checks that a visitor actually receives what was typed.
 *
 * Prints one JSON report; test.sh asserts on it. Skips cleanly without Chrome.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN  || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SLUG   = process.env.EDIT_SLUG || 'bk-page';

const report = { ok: false, steps: {}, errors: [] };
const done = (c) => { console.log(JSON.stringify(report)); process.exit(c); };

let puppeteer;
try { puppeteer = await import('puppeteer-core'); }
catch { report.skipped = 'puppeteer-core not installed'; done(0); }
const { existsSync, writeFileSync } = await import('node:fs');
const zlib = await import('node:zlib');
if (!existsSync(CHROME)) { report.skipped = 'chrome not found'; done(0); }

// --- a genuine 1x1 PNG to upload, built here so the test carries no binary ---
const crcTable = [...Array(256)].map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
});
const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type), data]);
    let c = 0xffffffff;
    for (const b of body) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    const crc = Buffer.alloc(4); crc.writeUInt32BE((c ^ 0xffffffff) >>> 0);
    return Buffer.concat([len, body, crc]);
};
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(1, 0); ihdr.writeUInt32BE(1, 4); ihdr[8] = 8; ihdr[9] = 2;
const PNG = '/tmp/journey-pic.png';
writeFileSync(PNG, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(Buffer.from([0, 255, 0, 0]))),
    chunk('IEND', Buffer.alloc(0)),
]));

const sid = await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }),
}).then(r => r.json()).then(j => j?.result?.session_id).catch(() => '');
if (!sid) { report.errors.push('admin auth failed'); done(1); }

const HEADING = 'Storage that fits ' + Date.now();
const QUOTE   = 'They had one free the same day.';
const CELL    = '50 sq ft';
const VIDEO   = 'https://www.youtube.com/watch?v=dQw4w9WgXcQ';

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const errs = [];

// Add a block from the palette. It selects the new block and opens Customize,
// so every later step can assume the sidebar is describing what was just added.
async function add(page, type) {
    await page.evaluate((t) => {
        const b = document.querySelector('.wse-add button[data-add="' + t + '"]');
        if (b) b.click();
    }, type);
    await new Promise(r => setTimeout(r, 450));
}

// Type into the Nth editable region of the SELECTED block, on the page itself.
async function typeInSelected(page, nth, text) {
    return page.evaluate((n, t) => {
        const host = document.querySelector('[data-wse].sel');
        if (!host) return false;
        const els = host.querySelectorAll('[contenteditable="true"]');
        if (!els[n]) return false;
        els[n].focus();
        els[n].textContent = t;
        els[n].dispatchEvent(new Event('input', { bubbles: true }));
        return true;
    }, nth, text);
}

// Set a field through the sidebar — the only way to reach something with no
// representation on the page, such as a video's URL.
async function setField(page, label, value) {
    return page.evaluate((l, v) => {
        const flds = [...document.querySelectorAll('#wse-pane-custom .wse-fld')];
        const f = flds.find(x => x.querySelector('label') &&
                                 x.querySelector('label').textContent.indexOf(l) === 0);
        if (!f) return false;
        const input = f.querySelector('input[type=text], textarea, select');
        if (!input) return false;
        input.value = v;
        input.dispatchEvent(new Event(input.tagName === 'SELECT' ? 'change' : 'input',
                                      { bubbles: true }));
        return true;
    }, label, value);
}

try {
    const page = await browser.newPage();
    await page.setViewport({ width: 1440, height: 950 });
    page.on('pageerror', e => errs.push('pageerror: ' + e.message));
    page.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
    page.on('dialog', async d => { try { await d.accept(); } catch {} });
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });

    await page.goto(`${BASE}/site/${SLUG}`, { waitUntil: 'networkidle2' });
    await page.waitForSelector('.wse-bar', { timeout: 15000 });
    await page.click('#wse-edit');
    await page.waitForSelector('.wse-side', { visible: true, timeout: 15000 });
    await new Promise(r => setTimeout(r, 500));
    report.steps.enteredEdit = true;

    // --- write a page, block by block, the way somebody would ---
    await add(page, 'heading');
    report.steps.addedHeading = await typeInSelected(page, 0, HEADING);

    await add(page, 'quote');
    report.steps.addedQuote = await typeInSelected(page, 0, QUOTE);

    await add(page, 'stats');
    report.steps.addedStats = await page.evaluate(() =>
        !!document.querySelector('[data-wse].sel .w-stats'));

    await add(page, 'table');
    report.steps.addedTable = await typeInSelected(page, 0, CELL);

    // A video's URL has no representation on the page at all — sidebar only.
    await add(page, 'video');
    report.steps.addedVideo = await setField(page, 'Video', VIDEO);
    await new Promise(r => setTimeout(r, 400));

    // Upload a real picture into a gallery, through the real control.
    await add(page, 'gallery');
    const file = await page.$('#wse-pane-custom input[type=file]');
    if (file) {
        await file.uploadFile(PNG);
        await new Promise(r => setTimeout(r, 2500));
    }
    report.steps.uploadedIntoGallery = await page.evaluate(() =>
        !!document.querySelector('[data-wse] img[src^="/site/media/"]'));

    // --- rearrange, the way a real edit goes ---
    const before = await page.evaluate(() =>
        [...document.querySelectorAll('[data-wse]')].map(n => n.textContent.slice(0, 12)));
    await page.evaluate(() => {
        const up = document.querySelectorAll('[data-up]');
        if (up.length > 1) up[up.length - 1].click();     // move the last block up
    });
    await new Promise(r => setTimeout(r, 450));
    const after = await page.evaluate(() =>
        [...document.querySelectorAll('[data-wse]')].map(n => n.textContent.slice(0, 12)));
    report.steps.reordered = JSON.stringify(before) !== JSON.stringify(after);

    // --- add something, then delete it again ---
    await add(page, 'text');
    const DOOMED = 'This paragraph should not survive';
    await typeInSelected(page, 0, DOOMED);
    const countBefore = await page.evaluate(() => document.querySelectorAll('[data-wse]').length);
    await page.evaluate(() => {
        const sel = document.querySelector('[data-wse].sel');
        if (sel) sel.querySelector('[data-del]').click();
    });
    await new Promise(r => setTimeout(r, 450));
    const countAfter = await page.evaluate(() => document.querySelectorAll('[data-wse]').length);
    report.steps.deleted = countAfter === countBefore - 1;

    // --- save ---
    await Promise.all([
        page.waitForNavigation({ waitUntil: 'networkidle2', timeout: 20000 }).catch(() => {}),
        page.evaluate(() => document.getElementById('wse-save').click()),
    ]);
    await new Promise(r => setTimeout(r, 1200));
    report.steps.saved = await page.evaluate(() =>
        !document.body.classList.contains('wse-on'));
    await page.close();

    // --- THE PART THAT MATTERS: what does a visitor get? ---
    const html = await fetch(`${BASE}/site/${SLUG}`).then(r => r.text());
    report.steps.visitorSeesHeading = html.includes(HEADING);
    report.steps.visitorSeesQuote   = html.includes(QUOTE);
    report.steps.visitorSeesTable   = html.includes(CELL);
    report.steps.visitorSeesVideo   =
        html.includes('youtube-nocookie.com/embed/dQw4w9WgXcQ');
    report.steps.visitorSeesImage   = /<img[^>]+src="\/site\/media\/\d+"/.test(html);
    report.steps.deletedGone        = !html.includes(DOOMED);
} catch (e) {
    report.errors.push('journey: ' + e.message);
} finally {
    await browser.close();
}

if (errs.length) report.errors.push(errs.join(' | '));
report.steps.errorCount = errs.length;

const s = report.steps;
report.ok = s.enteredEdit && s.addedHeading && s.addedQuote && s.addedStats &&
            s.addedTable && s.addedVideo && s.uploadedIntoGallery &&
            s.reordered && s.deleted && s.saved &&
            s.visitorSeesHeading && s.visitorSeesQuote && s.visitorSeesTable &&
            s.visitorSeesVideo && s.visitorSeesImage && s.deletedGone &&
            errs.length === 0;
done(report.ok ? 0 : 1);
