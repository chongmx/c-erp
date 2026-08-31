// Browser UX test (docs/072): multi-company login chooser + top-bar switcher.
const BASE = process.env.BASE || 'http://127.0.0.1:8172';
const wait = ms => new Promise(r => setTimeout(r, ms));
const results = [];
const check = (name, cond) => results.push({ name, ok: !!cond });

(async () => {
  const puppeteer = (await import('puppeteer-core')).default;
  const browser = await puppeteer.launch({
    executablePath: '/usr/bin/google-chrome', headless: 'new',
    args: ['--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--window-size=1440,900'],
  });
  const page = await browser.newPage();
  await page.setViewport({ width: 1440, height: 900 });
  const errors = [];
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });

  await page.goto(BASE + '/', { waitUntil: 'networkidle2', timeout: 30000 });
  await wait(1800);

  // 1) Login chooser: enter an email that maps to 2 companies; expect a Company
  //    <select> listing both (resolved via a debounced input, not blur).
  const loginSel = 'input[placeholder="you@company.com"]';
  await page.evaluate((sel) => {
    const inp = document.querySelector(sel);
    inp.value = 'chief@corp.example';
    inp.dispatchEvent(new Event('input', { bubbles: true }));
  }, loginSel);
  await wait(1400);   // debounce (450ms) + lookup
  const opts = await page.$$eval('.login-card select option', os => os.map(o => o.textContent.trim())).catch(() => []);
  check('login chooser shows a company dropdown', opts.length >= 2);
  check('chooser lists both companies', opts.some(o => /Company A/i.test(o)) && opts.some(o => /Company B/i.test(o)));

  // 2) Pick Company A, enter password, sign in.
  await page.evaluate(() => {
    const sel = document.querySelector('.login-card select');
    if (sel) { const o = [...sel.options].find(x => /Company A/i.test(x.textContent)); if (o) { sel.value = o.value; sel.dispatchEvent(new Event('change', { bubbles: true })); } }
  });
  await wait(400);
  await page.click('input[type=password]');
  await page.type('input[type=password]', 'admin');
  await page.click('.login-btn');
  await wait(2800);

  // 3) Home loaded + switcher visible showing company A.
  const switcherText = await page.$eval('.company-switcher', el => el.innerText).catch(() => null);
  check('company switcher appears after login', switcherText !== null);
  check('switcher shows the current company A', switcherText && /Company A/i.test(switcherText));

  // 4) Open switcher, click Company B.
  await page.click('.company-current').catch(() => {});
  await wait(500);
  const items = await page.$$('.company-item');
  check('switcher dropdown lists companies', items.length >= 2);
  const clicked = await page.evaluate(() => {
    const it = [...document.querySelectorAll('.company-item')].find(x => /Company B/i.test(x.textContent));
    if (it) { it.click(); return true; } return false;
  });
  check('clicked switch to Company B', clicked);

  // 5) The switch triggers an SSO reload into B. Poll for the switcher to show
  //    Company B (robust to a slow reload under load).
  let afterText = null;
  for (let i = 0; i < 24; i++) {
    await wait(500);
    afterText = await page.$eval('.company-switcher', el => el.innerText).catch(() => null);
    if (afterText && /Company B/i.test(afterText)) break;
  }
  check('after switch, current company is B', afterText && /Company B/i.test(afterText));

  // Ignore benign navigation aborts from the reload; count only real JS errors.
  const realErrors = errors.filter(e => !/ERR_ABORTED|Failed to fetch|NetworkError|load cancelled/i.test(e));
  check('no JS errors during the whole flow', realErrors.length === 0);
  console.log('UI_ERRORS=' + JSON.stringify(errors.slice(0, 8)));
  for (const r of results) console.log((r.ok ? '    PASS  ' : '    FAIL  ') + r.name);
  console.log(results.every(r => r.ok) ? 'All checks passed.' : '*** FAILURES ***');
  await browser.close();
})().catch(e => { console.error('FATAL', (e && e.stack) || e); console.log('*** FAILURES ***'); process.exit(1); });
