/*
 * Check the web UI served by a real board, not the mock. Read-only: nothing is
 * saved, so the board's configuration is untouched.
 *
 *   node hw_ui_check.js 192.168.222.1
 */
const { chromium } = require('playwright');

const HOST = process.argv[2] || 'pyro.local';
const BASE = `http://${HOST}`;
let failures = 0;

function check(name, ok, detail) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (detail ? `  [${detail}]` : ''));
  if (!ok) failures++;
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  await page.goto(BASE);
  await page.waitForFunction(() => {
    const el = document.getElementById('sState');
    return el && el.textContent !== '—' && el.textContent !== 'Connection lost';
  }, { timeout: 10000 });
  const board = await page.locator('#sBoard').textContent();
  check('status tab loads from the board', true, `${board}, ${await page.locator('#sState').textContent()}`);
  const usb = await page.locator('#sUsb').textContent();
  check('status tab says the board is grounded on USB (USB-01)', usb.includes('launch detection and beeps off'), usb);
  check('test mode is off and offered (USB-08)', (await page.locator('#testMode').count()) === 1 &&
        !(await page.locator('#testMode').isChecked()) && !(await page.locator('#testWarn').isVisible()));

  await page.click('.tab:has-text("Config")');
  check('no beep mode control (REV-12)', (await page.locator('#cfgBeep').count()) === 0);
  check('one Save button on the Config tab (REV-21)',
        (await page.locator('#tab-config button:has-text("Save")').count()) === 1);
  check('name field states its limit (REV-20)',
        (await page.locator('#cfgNameLen').textContent()).includes('8'),
        await page.locator('#cfgNameLen').textContent());

  const mode = await page.locator('#p2mode').inputValue();
  const units = await page.locator('#cfgUnits').inputValue();
  const before = parseInt(await page.locator('#p2val').inputValue());
  if (mode !== 'delay' && mode !== 'none') {
    const to = units === '2' ? '1' : '2';
    await page.selectOption('#cfgUnits', to);
    const after = parseInt(await page.locator('#p2val').inputValue());
    const cm = { 0: 1, 1: 100, 2: 30.48 };
    const want = Math.round(before * cm[units] / cm[to]);
    check('changing units converts the main altitude (REV-19)', after === want,
          `${before} ${['cm', 'm', 'ft'][units]} -> ${after} ${['cm', 'm', 'ft'][to]}`);
  }

  await page.click('.tab:has-text("Flight Data")');
  await page.waitForFunction(() => document.getElementById('dWhich').textContent.indexOf('Loading') < 0,
                             { timeout: 10000 });
  const which = await page.locator('#dWhich').textContent();
  check('flight data tab reads the board\'s log (REV-10)', which.length > 0, which);
  check('erase button present (REV-10)', (await page.locator('#btnEraseFlight').count()) === 1);

  await browser.close();
  console.log(failures ? `${failures} failed` : 'all passed');
  process.exit(failures ? 1 : 0);
})();
