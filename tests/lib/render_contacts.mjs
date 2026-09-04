/**
 * render_contacts.mjs — adding and removing contacts, entirely by clicking.
 *
 *   node tests/lib/render_contacts.mjs ZZCAR
 *
 * Contacts → Contacts, as a person uses it: add a company, add the people who
 * work there, find them in the list, correct a typo, remove someone added by
 * mistake, remove a company that turned out not to be a customer. Then the two
 * cases that are not simply "delete": a contact with history, and getting an
 * archived contact back.
 *
 * Nothing is created over the API. The `read` helper below is used only to
 * check what the database ended up with — never to make something happen.
 *
 * What the click version tests that the API version could not: the list
 * actually SHOWS what was saved (those two disagreed once — setting a company
 * cleared the free-text company_name the list was rendering, and every linked
 * contact showed a blank Company cell while the database was perfectly
 * correct), the delete dialog states its consequences before you commit, and
 * "Show archived" is a real route back to an archived record.
 */
const BASE   = process.env.BASE || 'http://127.0.0.1:8069';
const DB     = process.env.DBN || 'odoo';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/contacts';

const PFX  = process.argv[2] || 'ZZCAR';
const CO   = `${PFX} Green Valley Sdn Bhd`;
const AMY  = `${PFX} Amy Lim`;
const BEN  = `${PFX} Ben Ooi`;
const BEN2 = `${PFX} Ben Ooi Wei`;
const OOPS = `${PFX} Wrong Person`;
const DEAD = `${PFX} Not A Customer Bhd`;
const TEMP = `${PFX} Temp Contact`;

const puppeteer = await import('puppeteer-core');
const fs = await import('node:fs');
fs.mkdirSync(SHOTDIR, { recursive: true });

const auth = await (await fetch(`${BASE}/web/session/authenticate`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
        params: { db: DB, login: 'admin', password: 'admin' } }) })).json();
const sid = auth?.session_id || auth?.result?.session_id;
if (!sid) { console.log('FAIL could not authenticate'); process.exit(1); }

/** READ-ONLY. Assertions only. */
async function read(model, method, args, kwargs = {}) {
    const r = await fetch(`${BASE}/web/dataset/call_kw`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Cookie: `session_id=${sid}` },
        body: JSON.stringify({ jsonrpc: '2.0', method: 'call', params: {
            model, method, args, kwargs: { ...kwargs, context: { session_id: sid } } } }),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error.data?.message || j.error.message);
    return j.result;
}

let failed = 0;
const ok = m => console.log('    PASS  ' + m);
const no = m => { console.log('    FAIL  ' + m); failed++; };
const pause = ms => new Promise(r => setTimeout(r, ms));

const browser = await puppeteer.launch({ executablePath: CHROME,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu'] });
const page = await browser.newPage();
await page.setViewport({ width: 1500, height: 1000 });
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message +
    (e.stack ? ' @ ' + String(e.stack).split('\n').slice(1, 3).join(' | ').trim() : '')));
page.on('console',   m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

async function clickText(sel, text) {
    return page.evaluate((s, t) => {
        const el = [...document.querySelectorAll(s)].find(x => x.textContent.trim() === t);
        if (!el) return false;
        el.click(); return true;
    }, sel, text);
}
async function setField(name, value) {
    return page.evaluate((n, v) => {
        const el = document.querySelector(`[data-field="${n}"]`);
        if (!el) return false;
        el.focus(); el.value = v;
        el.dispatchEvent(new Event('input',  { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    }, name, value);
}
/**
 * Get to the Contacts LIST.
 *
 * Save keeps you on the record, as it should, and ErpNav.openRecord(model, 0)
 * cannot get you back — it clears the pending id but the action is already
 * open on a form, so the screen does not change. A person clicks the
 * "Contacts" breadcrumb; so does this.
 */
/** Is the CONTACTS list on screen? Any other list also has .list-table. */
async function onContactList() {
    return page.evaluate(() => {
        const t = document.querySelector('.list-table');
        if (!t) return false;
        const heads = [...t.querySelectorAll('thead th')].map(h => h.textContent.trim());
        return heads.includes('Name') && heads.includes('Company') && heads.includes('Email');
    });
}

async function openContactList() {
    // Checking for ".list-table" alone is not enough: after the rental-contract
    // detour that selector matches the CONTRACTS list, and every row lookup
    // then silently finds nothing on the wrong screen.
    if (await onContactList()) { await pause(400); return; }

    await page.evaluate(() => {
        const bc = [...document.querySelectorAll('.so-bc-link, .gf-bc-link')]
            .find(x => x.textContent.trim().toLowerCase() === 'contacts');
        if (bc) bc.click();
    });
    await pause(1000);
    if (await onContactList()) { await pause(400); return; }

    for (let attempt = 0; attempt < 3; attempt++) {
        await page.evaluate(() => window.ErpNav.openRecord('res.partner', 0));
        await pause(1400);
        if (await onContactList()) { await pause(400); return; }
    }
    throw new Error('could not reach the Contacts list');
}
async function clickNewContact() {
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('.view-toolbar button')]
            .find(x => /^new$/i.test(x.textContent.trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.contact-name-row [data-field="name"]', { timeout: 12000 });
    await pause(400);
}
/**
 * The Contacts list, as the browser is rendering it right now.
 *
 * Rows are keyed by the NAME column, located from the header rather than
 * assumed to be first — the column order comes from the view's arch and a
 * change there should not silently break every row lookup here.
 */
async function listRows() {
    return page.evaluate(() => {
        const heads = [...document.querySelectorAll('.list-table thead th')]
            .map(t => t.textContent.trim());
        const i = Math.max(0, heads.indexOf('Name'));
        return [...document.querySelectorAll('.list-table tbody tr')].map(tr => {
            const cells = [...tr.children].map(td => td.textContent.trim());
            return { name: cells[i] || '', row: cells.join(' | ') };
        });
    });
}
/** Click the list row whose Name cell is `name`. */
async function openRow(name) {
    const hit = await page.evaluate((n) => {
        const heads = [...document.querySelectorAll('.list-table thead th')]
            .map(t => t.textContent.trim());
        const i = Math.max(0, heads.indexOf('Name'));
        const tr = [...document.querySelectorAll('.list-table tbody tr')]
            .find(r => r.children[i] && r.children[i].textContent.trim() === n);
        if (!tr) return false;
        tr.click(); return true;
    }, name);
    if (hit) await page.waitForSelector('.contact-badge', { timeout: 12000 }).catch(() => {});
    await pause(900);
    return hit;
}
async function saveContact() {
    await clickText('.so-action-btns button', 'Save');
    await pause(1800);
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1. add a customer company ----------------------------------------
    await openContactList();
    await clickNewContact();
    await setField('name', CO);
    await clickText('.contact-badge', 'Company');
    await clickText('.contact-badge', 'Customer');
    await setField('email', 'hello@greenvalley.test');
    await setField('phone', '04-1234567');
    await setField('city', 'Ipoh');
    await saveContact();

    let rows = await read('res.partner', 'search_read', [[['name', '=', CO]]],
        { fields: ['id', 'is_company', 'customer_rank'] });
    if (rows.length === 1) ok('the company is added');
    else { no(`expected 1 company, found ${rows.length}`); throw new Error('setup'); }
    const CO_ID = rows[0].id;

    await openContactList();
    let listed = await listRows();
    if (listed.some(r => r.name === CO)) ok('and appears in the Contacts list');
    else no('the company is not in the list the browser rendered');
    if (listed.some(r => r.row.includes('hello@greenvalley.test')))
        ok('with the details that were typed');
    else no('the list does not show the email that was entered');

    // ---- 2. the people who work there --------------------------------------
    for (const person of [AMY, BEN]) {
        await openContactList();
        await clickNewContact();
        await setField('name', person);
        await pause(300);
        // Choose the employer in the company picker.
        const sel = '.m2o[data-model="res.partner"] input.m2o-input';
        await page.click(sel);
        await page.type(sel, 'Green Valley', { delay: 30 });
        const found = await page.waitForFunction(
            (n) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                     .some(o => o.textContent.trim() === n), { timeout: 8000 }, CO)
            .then(() => true).catch(() => false);
        if (!found) no(`the company picker did not offer "${CO}" for ${person}`);
        await page.evaluate((n) => {
            const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                .find(o => o.textContent.trim() === n);
            if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
        }, CO);
        await pause(500);
        await saveContact();
    }
    const people = await read('res.partner', 'search_read',
        [[['parent_id', '=', CO_ID]]], { fields: ['id', 'name'] });
    if (people.length === 2) ok('both contacts are added under the company');
    else no(`expected 2 contacts under the company, found ${people.length}`);

    await openContactList();
    listed = await listRows();
    if (listed.some(r => r.name === AMY)) ok('Amy is in the list');
    else no('Amy is missing from the list');
    if (listed.some(r => r.name === BEN)) ok('Ben is in the list');
    else no('Ben is missing from the list');
    // The reported bug: the Company cell must not be blank for a linked contact.
    const amyRow = (listed.find(r => r.name === AMY) || {}).row || '';
    if (amyRow.includes(CO)) ok('and their rows show which company they work for');
    else no(`Amy's row shows no company: "${amyRow}"`);
    await page.screenshot({ path: `${SHOTDIR}/list.png` });

    // ---- 3. correct a typo -------------------------------------------------
    await openRow(BEN);
    await setField('name', BEN2);
    await setField('phone', '012-9998888');
    await saveContact();
    const ben = await read('res.partner', 'search_read', [[['name', '=', BEN2]]],
        { fields: ['id', 'parent_id', 'commercial_company_name'] });
    if (ben.length === 1) ok('the name is corrected');
    else no(`expected 1 contact named "${BEN2}", found ${ben.length}`);
    const bp = ben.length && (Array.isArray(ben[0].parent_id) ? ben[0].parent_id[0] : ben[0].parent_id);
    if (bp === CO_ID) ok('and editing did not drop his company link');
    else no(`after the edit parent_id=${bp}, expected ${CO_ID}`);
    if (ben.length && ben[0].commercial_company_name === CO) ok('his Company cell still reads correctly');
    else no('the Company cell is wrong after the edit');

    // ---- 4. remove a contact added by mistake ------------------------------
    await openContactList();
    await clickNewContact();
    await setField('name', OOPS);
    await saveContact();
    let oops = await read('res.partner', 'search_read', [[['name', '=', OOPS]]], { fields: ['id'] });
    if (oops.length === 1) ok('a contact is added by mistake');
    else no('could not create the throwaway contact');

    await openContactList();
    await openRow(OOPS);
    if (await clickText('.so-action-btns button', 'Delete')) ok('the form offers Delete');
    else no('no Delete button on the contact form');
    await page.waitForSelector('.m2o-modal', { timeout: 8000 })
        .then(() => ok('a confirmation dialog appears'))
        .catch(() => no('Delete did not ask for confirmation'));
    await pause(500);
    await page.screenshot({ path: `${SHOTDIR}/delete-confirm.png` });
    await clickText('.m2o-modal-foot button', 'Delete');
    await pause(2200);
    oops = await read('res.partner', 'search_count', [[['name', '=', OOPS]]]);
    if (oops === 0) ok('the contact is removed');
    else no('the contact is still there after confirming Delete');

    await openContactList();
    listed = await listRows();
    if (!listed.some(r => r.name === OOPS)) ok('and it has left the list');
    else no('the removed contact is still listed');

    // ---- 5. remove a company — its people survive --------------------------
    await openContactList();
    await clickNewContact();
    await setField('name', DEAD);
    await clickText('.contact-badge', 'Company');
    await saveContact();
    await openContactList();
    await clickNewContact();
    await setField('name', TEMP);
    await pause(300);
    const sel2 = '.m2o[data-model="res.partner"] input.m2o-input';
    await page.click(sel2);
    await page.type(sel2, 'Not A Customer', { delay: 30 });
    await page.waitForFunction(
        (n) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                 .some(o => o.textContent.trim() === n), { timeout: 8000 }, DEAD)
        .catch(() => no('the picker did not offer the second company'));
    await page.evaluate((n) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === n);
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, DEAD);
    await pause(500);
    await saveContact();

    await openContactList();
    await openRow(DEAD);
    await clickText('.so-action-btns button', 'Delete');
    await page.waitForSelector('.m2o-modal', { timeout: 8000 }).catch(() => {});
    await pause(600);
    const warn = await page.evaluate(() =>
        (document.querySelector('.m2o-modal') || {}).textContent || '');
    if (/contacts working at this company/i.test(warn))
        ok('the dialog warns that a contact will be detached');
    else no(`the dialog did not mention the detached contact: "${warn.slice(0, 120)}"`);
    await clickText('.m2o-modal-foot button', 'Delete');
    await pause(2200);

    const deadGone = await read('res.partner', 'search_count', [[['name', '=', DEAD]]]);
    if (deadGone === 0) ok('the company is removed');
    else no('the company survived the delete');
    const temp = await read('res.partner', 'search_read', [[['name', '=', TEMP]]],
        { fields: ['id', 'parent_id'] });
    if (temp.length === 1) ok('its contact is NOT deleted with it');
    else no('deleting the company deleted its contact too');
    if (temp.length && !temp[0].parent_id) ok('they are detached, not left pointing at nothing');
    else no('the surviving contact still points at the deleted company');

    // ---- 6. a contact with history is archived, not removed ----------------
    // Give the company a rental contract, by clicking, so it has history.
    await page.evaluate(() => window.ErpNav.openRecord('rental.contract', 0));
    await pause(1400);
    await page.evaluate(() => {
        const b = [...document.querySelectorAll('button, .btn')]
            .find(x => /^(new|create)$/i.test(x.textContent.trim()));
        if (b) b.click();
    });
    await page.waitForSelector('.gf-shell', { timeout: 12000 });
    await pause(900);
    const rcSel = '.m2o[data-model="res.partner"] input.m2o-input';
    await page.click(rcSel);
    await page.type(rcSel, 'Green Valley', { delay: 30 });
    await page.waitForFunction(
        (n) => [...document.querySelectorAll('.m2o-pop .m2o-opt')]
                 .some(o => o.textContent.trim() === n), { timeout: 8000 }, CO).catch(() => {});
    await page.evaluate((n) => {
        const el = [...document.querySelectorAll('.m2o-pop .m2o-opt')]
            .find(o => o.textContent.trim() === n);
        if (el) el.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }, CO);
    await pause(400);
    await setField('name', `${PFX}-RC-9`);
    await setField('date_start', '2026-01-01');
    await clickText('.gf-actions button', 'Create');
    await pause(2400);
    const hist = await read('rental.contract', 'search_count', [[['name', '=', `${PFX}-RC-9`]]]);
    if (hist === 1) ok('the company now has a rental contract');
    else no('could not give the company any history');

    await openContactList();
    await openRow(CO);
    await clickText('.so-action-btns button', 'Delete');
    await page.waitForSelector('.m2o-modal', { timeout: 8000 }).catch(() => {});
    await pause(600);
    const blocked = await page.evaluate(() =>
        (document.querySelector('.m2o-modal') || {}).textContent || '');
    if (/cannot be deleted/i.test(blocked)) ok('the dialog refuses to delete it');
    else no(`the dialog did not refuse: "${blocked.slice(0, 140)}"`);
    if (/rental contracts/i.test(blocked)) ok('and names the rental contract as the reason');
    else no('the reason does not name the contract');
    await page.screenshot({ path: `${SHOTDIR}/delete-blocked.png` });

    if (await clickText('.m2o-modal-foot button', 'Archive instead'))
        ok('it offers Archive instead');
    else no('no "Archive instead" button on the blocked dialog');
    await pause(2400);

    const arch = await read('res.partner', 'search_read',
        [['|', ['active', '=', true], ['active', '=', false], ['id', '=', CO_ID]]],
        { fields: ['id', 'active'] });
    if (arch.length && arch[0].active === false) ok('the company is archived');
    else no('Archive instead did not archive the company');
    const stillThere = await read('rental.contract', 'search_count', [[['name', '=', `${PFX}-RC-9`]]]);
    if (stillThere === 1) ok('and its contract is untouched');
    else no('archiving disturbed the contract');

    // ---- 7. archived: out of the list, and findable again ------------------
    await openContactList();
    listed = await listRows();
    if (!listed.some(r => r.name === CO)) ok('an archived contact drops out of the list');
    else no('the archived company is still in the ordinary list');

    if (await clickText('.view-toolbar button', 'Show archived')) ok('the list offers Show archived');
    else no('no "Show archived" button on the Contacts list');
    await pause(1800);
    listed = await listRows();
    if (listed.some(r => r.name === CO)) ok('and it finds the archived company again');
    else no('Show archived did not bring the archived company back');
    await page.screenshot({ path: `${SHOTDIR}/show-archived.png` });

    await openRow(CO);
    if (await clickText('.so-action-btns button', 'Unarchive')) ok('the form offers Unarchive');
    else no('no Unarchive button on an archived contact');
    await pause(2400);
    const back = await read('res.partner', 'read', [[CO_ID], ['active']]);
    if (back[0].active !== false) ok('the company is active again');
    else no('Unarchive did not restore it');

    // ---- 8. a name can be reused -------------------------------------------
    await openContactList();
    await clickNewContact();
    await setField('name', TEMP);
    await saveContact();
    const dupes = await read('res.partner', 'search_count', [[['name', '=', TEMP]]]);
    if (dupes >= 2) ok('a name already in use can be used again');
    else no(`expected 2 contacts named "${TEMP}", found ${dupes}`);

    if (errs.length) no('browser errors: ' + errs.slice(0, 2).join(' | '));
    else ok('no browser console errors in the whole journey');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
    try { await page.screenshot({ path: `${SHOTDIR}/failure.png` }); } catch (_) {}
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
