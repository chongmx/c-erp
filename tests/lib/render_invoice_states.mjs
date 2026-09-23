/**
 * render_invoice_states.mjs — where an invoice ENDS UP, read off the screen.
 *
 *   node tests/lib/render_invoice_states.mjs ZZIS
 *
 * Asked for (CERP-6): an invoice has more than one ending, and the form said
 * "Posted" for all of them. Paid, reversed by a credit note and cancelled are
 * three different facts about the same posted document, and the only way to
 * tell them apart was to read the ledger.
 *
 * The journey, all clicks — no RPC drives any step:
 *
 *   1. Accounting ▸ Customers ▸ Invoices, open the first seeded invoice
 *   2. it is a DRAFT, and says so in the corner            (CERP-7)
 *   3. Confirm  -> posted, numbered, and NO ribbon: posted and unpaid is
 *      the ordinary state, and the status bar already says it
 *   4. Register Payment ▸ Validate -> "Paid"
 *   5. a second invoice: Confirm ▸ Cancel -> "Cancelled"
 *   6. a third: Confirm ▸ Add Credit Note, confirm the credit note under
 *      Credit Notes, reopen the invoice -> "Reversed"
 *
 * Step 6 is the one that cannot be faked: reversal is not a state on the
 * invoice. The credit note points back at it, so the ribbon is a claim about
 * a record the form never loaded until this was built.
 */
const BASE    = process.env.BASE || 'http://127.0.0.1:8069';
const DB      = process.env.DBN || 'odoo';
const CHROME  = process.env.CHROME_PATH || '/usr/bin/google-chrome';
const SHOTDIR = process.env.SHOTDIR || '/tmp/invoice_states';

const PFX  = process.argv[2] || 'ZZIS';
const PAY  = `${PFX} Pay`;
const CANC = `${PFX} Cancel`;
const REV  = `${PFX} Reverse`;

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
// "Add Credit Note" reports through alert(), and Delete asks through
// confirm(); an unhandled dialog freezes the page, not just the click.
const dialogs = [];
page.on('dialog', async d => { dialogs.push(d.message()); await d.accept(); });

const pause = ms => new Promise(r => setTimeout(r, ms));

// --- clicking, by what the user sees --------------------------------------
const clickText = (sel, text) => page.evaluate((s, t) => {
    const el = [...document.querySelectorAll(s)]
        .find(x => (x.textContent || '').trim() === t);
    if (!el) return false;
    el.click();
    return true;
}, sel, text);

async function openList(section, leaf) {
    // Home, then the app tile, then the menu — the route a user takes.
    await page.evaluate(() => {
        const b = document.querySelector('.nav-home-btn');
        if (b) b.click();
    });
    await pause(700);
    if (!(await clickText('.app-tile-name', 'Accounting'))) {
        // The tile's text node is the name; the tile itself is the target.
        await page.evaluate(() => {
            const t = [...document.querySelectorAll('.app-tile')]
                .find(x => /Accounting/.test(x.textContent || ''));
            if (t) t.click();
        });
    }
    await pause(1400);
    // A section with children renders its name and a caret in the same
    // button, so its textContent is "Customers▾" — match the label span.
    const gotSection = await page.evaluate((s) => {
        const btn = [...document.querySelectorAll('.nav-section-btn')]
            .find(x => {
                const lbl = x.querySelector('span');
                return lbl && lbl.textContent.trim() === s;
            });
        if (!btn) return false;
        btn.click();
        return true;
    }, section);
    if (!gotSection)
        return `no "${section}" section in the Accounting menu`;
    await pause(500);
    if (!(await clickText('.dropdown-item', leaf)))
        return `no "${leaf}" item under ${section}`;
    await pause(2000);
    return null;
}

async function openRow(text) {
    const hit = await page.evaluate((t) => {
        const row = [...document.querySelectorAll('.list-row')]
            .find(r => (r.textContent || '').includes(t));
        if (!row) return null;
        row.click();
        return true;
    }, text);
    if (!hit) {
        const rows = await page.evaluate(() =>
            [...document.querySelectorAll('.list-row')].slice(0, 8)
                .map(r => r.textContent.trim().replace(/\s+/g, ' ')));
        return `no row for "${text}" — the list showed ${JSON.stringify(rows)}`;
    }
    await page.waitForSelector('.so-card', { timeout: 12000 });
    await pause(1200);
    return null;
}

// What the form is showing right now: the ribbon, the highlighted step and
// the document number.
const formState = () => page.evaluate(() => {
    const rib  = document.querySelector('.doc-ribbon');
    const step = document.querySelector('.so-statusbar .so-step.active');
    const num  = document.querySelector('.so-doc-id');
    return {
        ribbon: rib ? rib.textContent.trim() : null,
        kind:   rib ? rib.className.replace('doc-ribbon', '').trim() : null,
        step:   step ? step.textContent.trim() : null,
        number: num ? num.textContent.trim() : null,
        buttons: [...document.querySelectorAll('.so-action-btns button')]
                    .map(b => b.textContent.trim()).filter(Boolean),
    };
});

async function pressAction(label) {
    const hit = await page.evaluate((t) => {
        const b = [...document.querySelectorAll('.so-action-btns button')]
            .find(x => (x.textContent || '').trim() === t);
        if (!b) return false;
        b.click();
        return true;
    }, label);
    if (!hit) return false;
    await pause(2600);
    return true;
}

try {
    await page.setCookie({ name: 'session_id', value: sid, domain: '127.0.0.1', path: '/' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle2' });
    await page.waitForFunction('window.ErpNav && window.ErpNav.available', { timeout: 15000 });

    // ---- 1 + 2. a draft says it is a draft --------------------------------
    let err = await openList('Customers', 'Invoices');
    if (err) { no(err); throw new Error(err); }
    ok('Accounting ▸ Customers ▸ Invoices opens from the menu');

    err = await openRow(PAY);
    if (err) { no(err); throw new Error(err); }

    let s = await formState();
    if (s.ribbon === 'Draft') ok('an unposted invoice is ribboned "Draft"');
    else no(`the ribbon on a draft reads ${JSON.stringify(s.ribbon)}`);
    if (s.kind === 'draft')   ok('and is styled as a draft, not as a verdict');
    else no(`ribbon class is "${s.kind}"`);
    if (s.step === 'Draft')   ok('the status bar agrees: Draft');
    else no(`the highlighted step is "${s.step}"`);
    await page.screenshot({ path: `${SHOTDIR}/1-draft.png` });

    // ---- 3. Confirm -------------------------------------------------------
    if (!(await pressAction('Confirm'))) { no('no Confirm button on the draft'); throw new Error('confirm'); }
    s = await formState();
    if (s.step === 'Posted') ok('Confirm posts it');
    else no(`after Confirm the step is "${s.step}"`);
    if (/^INV\d+/.test(s.number || '')) ok(`and numbers it (${s.number}) — a draft carried no number`);
    else no(`the posted document is numbered "${s.number}"`);
    if (s.ribbon === null) ok('posted and unpaid wears no ribbon — that is the ordinary case');
    else no(`a posted, unpaid invoice shows "${s.ribbon}"`);
    await page.screenshot({ path: `${SHOTDIR}/2-posted.png` });

    // ---- 4. Register Payment ---------------------------------------------
    if (!(await pressAction('Register Payment'))) { no('no Register Payment button'); throw new Error('pay'); }
    await page.waitForSelector('.pay-dialog-actions', { timeout: 8000 });
    // The dialog opens filled in with the full amount due, today, and the
    // bank journal — so paying it in full is one click.
    const validated = await page.evaluate(() => {
        const b = [...document.querySelectorAll('.pay-dialog-actions button')]
            .find(x => /validate/i.test(x.textContent || ''));
        if (!b) return false;
        b.click();
        return true;
    });
    if (!validated) no('the payment dialog has no Validate button');
    await pause(3000);
    const payErr = await page.evaluate(() => {
        const e = document.querySelector('.pay-dialog-error');
        return e ? e.textContent.trim() : null;
    });
    if (payErr) no(`the payment was refused: ${payErr}`);
    s = await formState();
    if (s.ribbon === 'Paid' && s.kind === 'paid') ok('a settled invoice is ribboned "Paid"');
    else no(`after payment the ribbon reads ${JSON.stringify(s.ribbon)} (${s.kind})`);
    await page.screenshot({ path: `${SHOTDIR}/3-paid.png` });

    // ---- 5. Cancel --------------------------------------------------------
    err = await openList('Customers', 'Invoices');
    if (err) { no(err); throw new Error(err); }
    err = await openRow(CANC);
    if (err) { no(err); throw new Error(err); }
    if (!(await pressAction('Confirm'))) { no('the second invoice has no Confirm button'); throw new Error('confirm2'); }
    if (!(await pressAction('Cancel')))  { no('a posted invoice offers no Cancel'); throw new Error('cancel'); }
    s = await formState();
    if (s.ribbon === 'Cancelled' && s.kind === 'cancelled') ok('a cancelled invoice is ribboned "Cancelled"');
    else no(`after Cancel the ribbon reads ${JSON.stringify(s.ribbon)} (${s.kind})`);
    if ((s.buttons || []).includes('Reset to Draft')) ok('and offers Reset to Draft, so cancelling is not a dead end');
    else no(`the cancelled form offers ${JSON.stringify(s.buttons)}`);
    await page.screenshot({ path: `${SHOTDIR}/4-cancelled.png` });

    // ---- 6. Reversed by a credit note -------------------------------------
    err = await openList('Customers', 'Invoices');
    if (err) { no(err); throw new Error(err); }
    err = await openRow(REV);
    if (err) { no(err); throw new Error(err); }
    if (!(await pressAction('Confirm'))) { no('the third invoice has no Confirm button'); throw new Error('confirm3'); }
    s = await formState();
    const revNumber = s.number;

    if (!(await pressAction('Add Credit Note'))) { no('a posted invoice offers no Add Credit Note'); throw new Error('reverse'); }
    if (dialogs.some(m => /credit note created as a draft/i.test(m)))
        ok('Add Credit Note reports that the credit note is a DRAFT, and says where to find it');
    else no(`the reversal reported ${JSON.stringify(dialogs)}`);

    // A draft credit note reverses nothing yet, so the invoice must not
    // claim it does. This is the check that a lazier ribbon would fail.
    err = await openList('Customers', 'Invoices');
    if (err) { no(err); throw new Error(err); }
    err = await openRow(REV);
    if (err) { no(err); throw new Error(err); }
    s = await formState();
    if (s.ribbon === null) ok('while the credit note is still a draft, the invoice is NOT marked reversed');
    else no(`a draft credit note already flipped the invoice to ${JSON.stringify(s.ribbon)}`);

    err = await openList('Customers', 'Credit Notes');
    if (err) { no(err); throw new Error(err); }
    err = await openRow(REV);
    if (err) { no(err); throw new Error(err); }
    s = await formState();
    if (s.step === 'Draft') ok('the credit note is waiting under Credit Notes, as a draft');
    else no(`the credit note opened at step "${s.step}"`);
    if (!(await pressAction('Confirm'))) { no('the credit note has no Confirm button'); throw new Error('confirm-cn'); }
    s = await formState();
    if (/^RINV\d+/.test(s.number || '')) ok(`posting it numbers it in its own series (${s.number})`);
    else no(`the posted credit note is numbered "${s.number}"`);
    await page.screenshot({ path: `${SHOTDIR}/5-credit-note.png` });

    err = await openList('Customers', 'Invoices');
    if (err) { no(err); throw new Error(err); }
    err = await openRow(REV);
    if (err) { no(err); throw new Error(err); }
    s = await formState();
    if (s.ribbon === 'Reversed' && s.kind === 'reversed')
        ok(`${revNumber} now reads "Reversed" — the ending the status bar could not show`);
    else no(`the reversed invoice shows ${JSON.stringify(s.ribbon)} (${s.kind})`);
    await page.screenshot({ path: `${SHOTDIR}/6-reversed.png` });

    if (errs.length) no('browser errors: ' + errs.slice(0, 3).join(' | '));
    else ok('no browser console errors');
} catch (e) {
    no('drive failed: ' + (e.message || e).split('\n')[0]);
} finally {
    await browser.close();
}

console.log('    screenshots: ' + SHOTDIR);
process.exit(failed ? 1 : 0);
