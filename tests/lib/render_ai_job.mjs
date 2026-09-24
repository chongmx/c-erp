/**
 * render_ai_job.mjs — asking the agent without the browser holding the line.
 *
 *   node tests/lib/render_ai_job.mjs ZZAJF
 *
 * CERP-10. The old shape was one HTTP request held open for as long as the
 * model took, and three separate layers each gave up before it did — the
 * browser at 45 s, nginx at 60 s, a CDN sooner still. The answer arrived to a
 * screen that had stopped listening.
 *
 * Now the question is a job on the server and the screen polls it, which is
 * what this drives:
 *
 *   1. ask, and get an answer back on screen — the whole path, through a job
 *   2. RELOAD while a question is running: the screen finds it again and says
 *      it is still going. This is the case a held-open request can never
 *      survive, and the reason for the change.
 *   3. the answer lands without another click, because the screen is polling
 *   4. "Stop waiting" ends the job rather than just hiding it
 *
 * Steps 2-4 work on a job row the test puts there directly. That is not a
 * shortcut around the feature: a row in that state is exactly what a reloaded
 * browser finds, and driving it this way makes the test independent of how
 * long a model happens to take today.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/ai_job';

const PFX = process.argv[2] || 'ZZAJF';

const puppeteer = await import('puppeteer-core');
const { execFileSync } = await import('node:child_process');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

/**
 * psql -tAc prints the RETURNING row AND the command tag ("INSERT 0 1"), so a
 * two-line answer comes back for a one-value question and the next statement
 * is built from both. The first line is the value; the tag never is.
 */
const sql = (q) => execFileSync('psql', ['-h', 'localhost', '-U', DB, '-d', DB, '-tAc', q],
                                { env: { ...process.env, PGPASSWORD: 'odoo' } })
                       .toString().trim().split('\n')[0].trim();

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

const pause = ms => new Promise(r => setTimeout(r, ms));
const openLookup = async () => {
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });
    await page.evaluate(() => window.ErpNav.openRecord('part.lookup', 0));
    await page.waitForSelector('.pl-ask', { timeout: 12000 });
    await pause(1500);
};
const waitingText = () => page.evaluate(() => {
    const w = document.querySelector('.pl-waiting');
    return w ? w.textContent.replace(/\s+/g, ' ').trim() : '';
});

const ADMIN = sql("SELECT id FROM res_users WHERE login='admin'");

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await openLookup();

    // ---- 1. ask, and get an answer -----------------------------------------
    await page.type('.pl-ask-in', `${PFX} 4.7k 0805 1% resistor`, { delay: 10 });
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.pl-ask button')]
            .find(x => /ask the agent/i.test(x.textContent || ''));
        if (b) b.click();
    });
    // The mock provider answers locally, so this is quick — but it still goes
    // through the job and the poll, which is the path under test.
    let got = false;
    for (let i = 0; i < 20; i++) {
        await pause(1000);
        got = await page.evaluate(() => !!document.querySelector('.pl-agent'));
        if (got) break;
    }
    if (got) ok('asking produces an answer on screen, through a job');
    else no('no answer appeared within 20 s');
    await page.screenshot({ path: `${SHOTDIR}/1-answered.png` });

    const jobs = Number(sql(`SELECT count(*) FROM ir_ai_job WHERE query LIKE '${PFX}%'`));
    if (jobs >= 1) ok(`the question was recorded as a job (${jobs})`);
    else no('no job row was created');

    // ---- 2. a question still running, found again after a RELOAD ------------
    const running = sql(
        `INSERT INTO ir_ai_job (kind,state,user_id,query,started_at)
         VALUES ('lookup','running',${ADMIN},'${PFX} slow question', now()) RETURNING id`);
    await openLookup();                     // a fresh page load, as after F5
    let txt = '';
    for (let i = 0; i < 10; i++) {
        txt = await waitingText();
        if (txt) break;
        await pause(700);
    }
    if (txt) ok('after a reload the screen finds the question still running');
    else no('a running question was not picked up after a reload');
    if (/server/i.test(txt)) ok(`and says where it is running: "${txt.slice(0, 80)}…"`);
    else no(`the waiting line reads "${txt}"`);
    await page.screenshot({ path: `${SHOTDIR}/2-resumed.png` });

    // ---- 3. the answer arrives without another click ------------------------
    const result = JSON.stringify({
        ok: true, provider: 'mock', model: 'mock-1', mocked: true, searched: false,
        notes: `${PFX} answered while nobody was looking`,
        sources: [], searches: [],
        candidates: [{ query: `${PFX} slow question`, mpn: `${PFX}-LATE-1`,
                       manufacturer: 'Mock', name: `${PFX} late answer`,
                       confidence: 0.8, parameters: [] }],
    }).replace(/'/g, "''");
    sql(`UPDATE ir_ai_job SET state='done', result='${result}'::jsonb, finished_at=now()
          WHERE id=${running}`);
    let late = false;
    for (let i = 0; i < 12; i++) {
        await pause(1000);
        late = await page.evaluate((mpn) =>
            (document.querySelector('.pl-agent') || {}).textContent?.includes(mpn) || false,
            `${PFX}-LATE-1`);
        if (late) break;
    }
    if (late) ok('when it finishes, the answer appears on its own — the screen was polling');
    else no('the finished answer never reached the screen');
    if (!(await waitingText())) ok('and the waiting line goes away');
    else no('the screen still says it is waiting');
    await page.screenshot({ path: `${SHOTDIR}/3-late-answer.png` });

    // ---- 4. stop waiting ----------------------------------------------------
    const cancelMe = sql(
        `INSERT INTO ir_ai_job (kind,state,user_id,query,started_at)
         VALUES ('lookup','running',${ADMIN},'${PFX} cancel me', now()) RETURNING id`);
    await openLookup();
    await page.waitForSelector('[data-pl="cancel-ask"]', { timeout: 10000 })
        .then(() => ok('a running question offers "Stop waiting"'))
        .catch(() => no('no way to stop waiting'));
    await page.evaluate(() => document.querySelector('[data-pl="cancel-ask"]').click());
    await pause(2000);
    const state = sql(`SELECT state FROM ir_ai_job WHERE id=${cancelMe}`);
    if (state === 'cancelled') ok('which cancels the job on the server, not just on screen');
    else no(`the job is "${state}" after stopping`);

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
