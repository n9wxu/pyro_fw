// @ts-check
const { test, expect } = require('@playwright/test');

/*
 * Web UI tests against mock Pyro server.
 * Server mode is set via PYRO_MODE env var (new/configured/flown).
 * Server started by globalSetup, URL passed via BASE_URL.
 */

const BASE = process.env.BASE_URL || 'http://localhost:3000';

/* ── Helpers ──────────────────────────────────────────────────────── */

async function waitForStatus(page) {
  await page.waitForFunction(() => {
    const el = document.getElementById('sState');
    return el && el.textContent !== '—' && el.textContent !== 'Connection lost';
  }, { timeout: 5000 });
}

function clickTab(page, name) {
  return page.click(`.tab:has-text("${name}")`);
}

/* ══════════════════════════════════════════════════════════════════
   Mode: NEW — fresh device with default config
   ══════════════════════════════════════════════════════════════════ */

test.describe('New device', () => {
  test.skip(process.env.PYRO_MODE !== 'new', 'Skipped: not new mode');

  test('status tab shows PAD_IDLE', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#sState')).toHaveText('PAD_IDLE');
  });

  test('status shows altitude in meters (default units=m)', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const alt = await page.locator('#sAlt').textContent();
    expect(alt).toContain('m');
  });

  test('config tab loads default values', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#p1mode')).toHaveValue('delay');
    await expect(page.locator('#p1val')).toHaveValue('0');
    await expect(page.locator('#p2mode')).toHaveValue('agl');
    await expect(page.locator('#p2val')).toHaveValue('300');
    await expect(page.locator('#cfgUnits')).toHaveValue('1');
  });

  test('pyro channels show OK', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const p1 = await page.locator('#sP1').textContent();
    expect(p1).toContain('OK');
  });

  test('no pending config warning', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#pendingWarn')).toBeHidden();
  });

  test('active config shows default pyro settings', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const p1 = await page.locator('#sCfgP1').textContent();
    expect(p1).toContain('Delay');
    expect(p1).toContain('0');
    const p2 = await page.locator('#sCfgP2').textContent();
    expect(p2).toContain('AGL');
    expect(p2).toContain('300');
  });

  test('flight data tab shows no data', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dWhich')).toContainText('No flight recorded');
    await expect(page.locator('#dDur')).toHaveText('—');
  });

  /* [WEB-UI-04] The log is re-read whenever the tab is shown. */
  test('a flight recorded while the page is open appears on refresh', async ({ page, request }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dWhich')).toContainText('No flight recorded');
    await request.post(BASE + '/api/_test/fly');
    await page.click('button:has-text("Refresh")');
    await expect(page.locator('#dDur')).toContainText('32.4');
    await expect(page.locator('#dWhich')).toContainText('Screamer');
    await request.post(BASE + '/api/_test/reset');
  });

  /* A control that changes nothing is not offered. */
  test('there is no beep mode control', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#cfgBeep')).toHaveCount(0);
  });

  /* [CFG-07] The shipped default name fits its 8-character field. */
  test('the default rocket name is not truncated', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.click('button:has-text("Default")');
    await expect(page.locator('#cfgName')).toHaveValue('MyRocket');
  });

  /* WEB-UI-06, DD-064: the longest flight the log holds, for the plan chosen.
     The mock reports 7,900,000 bytes free, 22-byte records and 100 rows/s. */
  test('log rate: the longest flight the log holds follows the plan', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#logRate')).toHaveValue('1hz');
    await expect(page.locator('#logEst')).toContainText('4.2 days');
    await expect(page.locator('#logEst')).toContainText('1 row/s');
    await page.selectOption('#logRate', 'full');
    await expect(page.locator('#logEst')).toContainText('60 min');
    await expect(page.locator('#logEst')).toContainText('100 rows/s');
    await expect(page.locator('#cfgDirty')).toBeVisible();
    /* 359,090 rows, less 10 events x 2 s x 99 extra rows: 4.1 days. */
    await page.selectOption('#logRate', 'events');
    await expect(page.locator('#logEst')).toContainText('4.1 days');
    await expect(page.locator('#logEst')).toContainText('each event');
  });

  test('log rate: saved with the rest of the tab', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.selectOption('#logRate', 'events');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('Saved', { timeout: 5000 });
    const ini = await (await page.request.get(BASE + '/api/config')).text();
    expect(ini).toContain('log_rate=events');
    await page.click('button:has-text("Default")');
    await expect(page.locator('#logRate')).toHaveValue('1hz');
  });

  /* In flight the log holds the filesystem, so the space cannot be read. */
  test('log rate: no estimate while the flight log is written', async ({ page }) => {
    await page.route('**/api/log/space', r => r.fulfill({ status: 423, body: '{"error":"the flight log holds the filesystem"}' }));
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#logEst')).toContainText('flight log');
  });
});

/* ══════════════════════════════════════════════════════════════════
   Mode: CONFIGURED — non-default config
   ══════════════════════════════════════════════════════════════════ */

test.describe('Configured device', () => {
  test.skip(process.env.PYRO_MODE !== 'configured', 'Skipped: not configured mode');

  test('status shows values in feet', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const alt = await page.locator('#sAlt').textContent();
    expect(alt).toContain('ft');
  });

  test('config tab shows non-default values', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#p1mode')).toHaveValue('delay');
    await expect(page.locator('#p1val')).toHaveValue('2');
    await expect(page.locator('#p2mode')).toHaveValue('agl');
    await expect(page.locator('#p2val')).toHaveValue('500');
    await expect(page.locator('#cfgUnits')).toHaveValue('2');
    await expect(page.locator('#logRate')).toHaveValue('full');
  });

  test('active config shows custom settings', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const p1 = await page.locator('#sCfgP1').textContent();
    expect(p1).toContain('Delay');
    expect(p1).toContain('2');
    const p2 = await page.locator('#sCfgP2').textContent();
    expect(p2).toContain('AGL');
    expect(p2).toContain('500');
  });

  test('edit config → save shows confirmation', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.fill('#p2val', '400');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('Saved', { timeout: 5000 });
  });

  test('save → reboot → config applied', async ({ page }) => {
    test.skip(true, 'Reboot cycle test requires timing stabilization — tracked in #TODO');
  });

  test('default button loads defaults', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.click('button:has-text("Default")');
    await expect(page.locator('#p1mode')).toHaveValue('delay');
    await expect(page.locator('#p1val')).toHaveValue('0');
    await expect(page.locator('#p2mode')).toHaveValue('agl');
    await expect(page.locator('#p2val')).toHaveValue('300');
    await expect(page.locator('#cfgUnits')).toHaveValue('1');
    await expect(page.locator('#cfgDirty')).toBeVisible();
  });

  test('current button restores device config', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.click('button:has-text("Default")');
    await page.click('button:has-text("Current")');
    await expect(page.locator('#p2val')).toHaveValue('500');
    await expect(page.locator('#cfgUnits')).toHaveValue('2');
  });

  /* [WEB-UI-02] Changing units converts the deployment altitudes: 500 ft
     must not become 500 m. */
  test('changing units converts the pyro values', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#p2val')).toHaveValue('500');
    await page.selectOption('#cfgUnits', '1');
    await expect(page.locator('#p2val')).toHaveValue('152');
    await expect(page.locator('#p1val')).toHaveValue('2'); /* a delay is seconds */
    await page.selectOption('#cfgUnits', '2');
    await expect(page.locator('#p2val')).toHaveValue('499');
  });

  /* [WEB-UI-02, CFG-07] The 8-character limit is stated, not discovered. */
  test('the rocket name shows its 8-character limit', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#cfgNameLen')).toContainText('8');
    await page.fill('#cfgName', '');
    await page.type('#cfgName', 'SKYSTREAK');
    await expect(page.locator('#cfgName')).toHaveValue('SKYSTREA');
    await expect(page.locator('#cfgNameLen')).toHaveText('8 of 8 characters');
  });

  test('range warning for value exceeding sensor limit', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.selectOption('#p2mode', 'agl');
    await page.fill('#p2val', '30000');
    await page.locator('#p2val').dispatchEvent('change');
    const warn = await page.locator('#p2warn').textContent();
    expect(warn).toContain('sensor limit');
  });

  /* USB-01: a board reached over USB says it is grounded. */
  test('on USB the status tab says launch detection is off', async ({ page }) => {
    await page.request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#sUsb')).toContainText('launch detection and beeps off');
    await expect(page.locator('#testWarn')).toBeHidden();
    await expect(page.locator('#testMode')).not.toBeChecked();
  });

  /* USB-08: test mode asks first, warns while on, and turns off without a prompt. */
  test('test mode asks first, warns while on, and can be turned off', async ({ page }) => {
    await page.request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    page.once('dialog', d => d.accept());
    await page.click('#testMode');
    await expect(page.locator('#testWarn')).toBeVisible();
    await expect(page.locator('#sUsb')).toContainText('test mode');
    await expect(page.locator('#testMode')).toBeChecked();
    await page.click('#testMode');
    await expect(page.locator('#testWarn')).toBeHidden();
    await expect(page.locator('#sUsb')).toContainText('launch detection and beeps off');
    const st = await (await page.request.get(`${BASE}/api/status`)).json();
    expect(st.test_mode).toBe(false);
  });

  test('declining the test mode prompt leaves it off', async ({ page }) => {
    await page.request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    page.once('dialog', d => d.dismiss());
    await page.click('#testMode');
    await expect(page.locator('#testMode')).not.toBeChecked();
    const st = await (await page.request.get(`${BASE}/api/status`)).json();
    expect(st.test_mode).toBe(false);
  });
});

/* ══════════════════════════════════════════════════════════════════
   Mode: FLOWN — post-flight with data
   ══════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
 * Pin assignment
 *
 * The Config tab's release section and the Lua tab's pin table are both
 * rendered from /api/pins/caps. They shipped with no coverage here, against a
 * mock server that did not serve the endpoint -- so the tabs silently fell
 * back to their error text and nothing noticed.
 * ══════════════════════════════════════════════════════════════════ */

test.describe('Pin assignment', () => {
  test.skip(process.env.PYRO_MODE !== 'configured', 'configured mode only');

  test('release section renders from the board capability table', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#relTable')).toBeVisible();
    /* The board's own topology, not a string baked into app.js. */
    await expect(page.locator('#relHint')).toContainText('high side');
    await expect(page.locator('#rel1pins')).toContainText('GPIO21');
    await expect(page.locator('#rel2pins')).toContainText('GPIO22');
    await expect(page.locator('#relCommon')).toContainText('GPIO15');
  });

  /* Item 3: an operator wiring a rocket is holding a connector, not a GPIO. */
  test('pin tables name the connector, not just the GPIO', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#rel1pins')).toContainText('CN1.1 drogue');
    await expect(page.locator('#rel2pins')).toContainText('CN1.4 main');
    await expect(page.locator('#relCommon')).toContainText('CN1.2-3 common');

    await clickTab(page, 'Lua');
    await expect(page.locator('#luaPins')).toContainText('J1 user pad');
  });

  /* Item 1: the buzzer pad is assignable. The board's own pad is the default
     and every digital pad is on offer; a buzzer-only pad is not, because
     moving the buzzer onto the pad it is already on means nothing. */
  test('buzzer can be moved to another pad', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    const sel = page.locator('#bzPin');
    await expect(sel).toBeVisible();
    await expect(sel.locator('option[value="board"]')).toHaveCount(1);
    await expect(sel.locator('option[value="8"]')).toHaveCount(1);
    await expect(sel.locator('option[value="16"]')).toHaveCount(0);
    await expect(page.locator('#bzHint')).toContainText('frees GPIO16');

    await sel.selectOption('8');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#relMsg')).toContainText('saved', { timeout: 5000 });
  });

  /* [GND-TEST-12] The ground test switch: none by default, a switch to
     ground on one pad, or a switch across two -- MK1A's ground pad is
     crowded. Only a digital pad is offered, and the second pad only for a
     switch across two. */
  test('the ground test switch is wired to ground or across two pads', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    const w = page.locator('#gtWiring');
    await expect(w).toBeVisible();
    await expect(w).toHaveValue('none');
    await expect(page.locator('#gtPin')).toBeHidden();
    await expect(page.locator('#gtDrive')).toBeHidden();

    await w.selectOption('ground');
    await expect(page.locator('#gtPin')).toBeVisible();
    await expect(page.locator('#gtDrive')).toBeHidden();
    await expect(page.locator('#gtPin option[value="8"]')).toHaveCount(1);
    await expect(page.locator('#gtPin option[value="16"]')).toHaveCount(0); /* the buzzer's */
    await expect(page.locator('#gtPin option[value="26"]')).toHaveCount(0); /* a sense pad */

    await w.selectOption('pair');
    await expect(page.locator('#gtDrive')).toBeVisible();
    await page.locator('#gtPin').selectOption('8');
    await page.locator('#gtDrive').selectOption('21');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#relMsg')).toContainText('saved', { timeout: 5000 });
    const ini = await page.evaluate(() => fetch('/api/pins').then(r => r.text()));
    expect(ini).toContain('ground_test=pair');
    expect(ini).toContain('ground_test_pin=8');
    expect(ini).toContain('ground_test_drive_pin=21');
  });

  /* One Save on the Config tab, for config.ini and pins.ini both. */
  test('the config tab has one save button', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#tab-config button:has-text("Save")')).toHaveCount(1);
  });

  /* [LUA-IO-01/02] A program that lives only on the device is lost with its
     filesystem. Import must land in the editor and not on the device, so a
     mis-picked file costs nothing until Save. */
  test('lua program exports to a file', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await page.fill('#luSrc', '-- exported by a test\nfunction tick() end\n');

    const dl = page.waitForEvent('download');
    await page.click('text=Export .lua');
    const download = await dl;
    expect(download.suggestedFilename()).toMatch(/\.lua$/);
    await expect(page.locator('#luIoMsg')).toContainText('exported');
  });

  test('lua program imports into the editor without saving', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await page.fill('#luSrc', 'about to be replaced');

    await page.setInputFiles('#luFile', {
      name: 'rocket.lua',
      mimeType: 'text/plain',
      buffer: Buffer.from('-- imported\nfunction tick() end\n'),
    });
    await expect(page.locator('#luSrc')).toHaveValue(/imported/);
    await expect(page.locator('#luIoMsg')).toContainText('press Save');
  });

  test('an oversized import is refused before it is read', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await page.fill('#luSrc', 'untouched');

    await page.setInputFiles('#luFile', {
      name: 'huge.lua',
      mimeType: 'text/plain',
      buffer: Buffer.alloc(70000, 0x20),
    });
    await expect(page.locator('#luIoMsg')).toContainText('the limit is 65536');
    await expect(page.locator('#luSrc')).toHaveValue('untouched');
  });

  test('releasing one channel warns about the shared element', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.uncheck('#rel2');
    /* The firmware cannot prevent this one, so the UI has to say it. */
    await expect(page.locator('#relWarn')).toContainText('One channel released, one retained');
  });

  test('lua tab lists only pads with a lua capability', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    const table = page.locator('#luaPins');
    await expect(table).toContainText('GPIO8');
    await expect(table).toContainText('GPIO21');
    /* GPIO16 is buzzer-only and GPIO26 is analog-only: neither can take a
       role, so neither belongs in the table. */
    await expect(table).not.toContainText('GPIO16');
    await expect(table).not.toContainText('GPIO26');
  });

  test('a role menu offers only what the pin is capable of', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    /* The table is rendered after /api/pins/caps resolves, and
       allTextContents() does not auto-retry the way expect() does -- so wait
       for the row to exist before reading its options. */
    await expect(page.locator('#pr15')).toBeVisible();
    /* GPIO15 is the common: digital and bridge, but no pwm and no serial. */
    const opts = await page.locator('#pr15 option').allTextContents();
    expect(opts.join(',')).toContain('half-bridge');
    expect(opts.join(',')).not.toContain('serial');
    expect(opts.join(',')).not.toContain('dimmable');
  });

  test('an incomplete half-bridge is flagged before saving', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await expect(page.locator('#pr15')).toBeVisible();
    await page.selectOption('#pr15', 'off');
    await expect(page.locator('#luaPinsWarn')).toContainText('one channel element plus the common');
  });

  test('saving the pin assignment reports success', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await expect(page.locator('#pr15')).toBeVisible();
    await page.click('#btnSavePins');
    await expect(page.locator('#luaPinsMsg')).toContainText('saved', { timeout: 5000 });
  });
});

/* ══════════════════════════════════════════════════════════════════
 * Beep codes
 *
 * A code was a two-digit number an operator heard at the pad with nothing to
 * look it up in, and seven of thirteen were never emitted. The tab exists so
 * the meaning is reachable and the code is changeable.
 * ══════════════════════════════════════════════════════════════════ */

test.describe('Beep codes', () => {
  test.skip(process.env.PYRO_MODE !== 'configured', 'configured mode only');

  test('the shipped personality is the Eggtimer convention', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await expect(page.locator('#bpSel')).toHaveValue('0');
    await expect(page.locator('#bpName')).toHaveValue('Default');
    /* Ready to fly is a chirp you never count -- the whole point of matching
       Eggtimer rather than inventing a code for the good case. */
    await expect(page.locator('#bkok_to_fly')).toHaveValue('chirp');
    await expect(page.locator('#bkcheck_pyro_1')).toHaveValue('code');
    await expect(page.locator('#bd1check_pyro_1')).toHaveValue('5');
  });

  test('every outcome shows what it means', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    const table = page.locator('#bpTable');
    await expect(table).toContainText('Safe the system and leave the pad');
    await expect(table).toContainText('Check pyro 1');
    await expect(table).toContainText('OK to fly');
  });

  test('the beep counts hide when the sound is not a count', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    /* A chirp has no number, so offering one would be offering nonsense. */
    await expect(page.locator('#bd1ok_to_fly')).toBeHidden();
    await page.selectOption('#bkok_to_fly', 'code');
    await expect(page.locator('#bd1ok_to_fly')).toBeVisible();
  });

  test('merging the pyro channels removes the second row', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await expect(page.locator('#browcheck_pyro_2')).toBeVisible();
    await page.uncheck('#bpSplit');
    /* Channel 2 is never played when merged, so a sound for it would be one
       the board cannot say. */
    await expect(page.locator('#browcheck_pyro_2')).toHaveCount(0);
  });

  test('two outcomes that sound alike are flagged', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.fill('#bd1check_pyro_1', '2');
    await expect(page.locator('#bpWarn')).toContainText('sound the same');
  });

  test('a zero beep count is flagged', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.fill('#bd1check_pyro_1', '0');
    await expect(page.locator('#bpWarn')).toContainText('cannot be heard');
  });

  test('switching personality loads its own settings', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.fill('#bpName', 'Loud');
    await expect(page.locator('#bpSel')).toContainText('Loud');
    await page.selectOption('#bpSel', '1');
    await expect(page.locator('#bpName')).toHaveValue('Custom 1');
    await page.selectOption('#bpSel', '0');
    await expect(page.locator('#bpName')).toHaveValue('Loud');
  });

  test('a row can be auditioned before saving', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.fill('#bd1check_pyro_1', '7');
    await page.locator('#browcheck_pyro_1 button').click();
    await expect(page.locator('#bpMsg')).toContainText('7 beeps', { timeout: 5000 });
  });

  test('the chirp can be auditioned too', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.locator('#browok_to_fly button').click();
    await expect(page.locator('#bpMsg')).toContainText('chirp', { timeout: 5000 });
  });

  test('Eggtimer defaults restore into the form', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.selectOption('#bpSel', '1');
    await page.selectOption('#bkok_to_fly', 'tone');
    await page.click('button:has-text("Eggtimer defaults")');
    await expect(page.locator('#bkok_to_fly')).toHaveValue('chirp');
  });

  test('saving a valid table reports success', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    await page.click('#btnSaveBeeps');
    await expect(page.locator('#bpMsg')).toContainText('saved', { timeout: 5000 });
  });
});

test.describe('Flown device', () => {
  test.skip(process.env.PYRO_MODE !== 'flown', 'Skipped: not flown mode');
  test.afterEach(async ({ request }) => { await request.post(BASE + '/api/_test/reset'); });

  test('status shows LANDED', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#sState')).toHaveText('LANDED');
  });

  test('max altitude shows ~10000ft', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const max = await page.locator('#sMax').textContent();
    /* 304800cm in ft = 304800 * 100 / 3048 / 100 = 10000.0 */
    expect(max).toContain('10000');
  });

  test('pyro channels show FIRED', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    const p1 = await page.locator('#sP1').textContent();
    const p2 = await page.locator('#sP2').textContent();
    expect(p1).toContain('FIRED');
    expect(p2).toContain('FIRED');
  });

  test('flight data tab shows duration and apogee', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    /* The tab fetches the log on opening: wait for it, don't sample. */
    await expect(page.locator('#dDur')).toContainText('32.4');
    await expect(page.locator('#dApogee')).toContainText('10000');
  });

  /* N27, FLT-MACH-07: while the Mach lock stands the ports' altitude is not
     the rocket's, so the apogee comes from the rows outside it -- and is only
     a lower bound if the lock let go within 2 s of apogee, or never did. */
  async function openLocked(page, request, how) {
    await request.post(BASE + '/api/_test/fly_locked/' + how);
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
  }

  test('apogee skips the Mach lock', async ({ page, request }) => {
    await openLocked(page, request, 'early');
    await expect(page.locator('#dApogee')).toContainText('10000');
    await expect(page.locator('#dApogee')).not.toContainText('at least');
  });

  test('a lock let go near apogee makes it a lower bound', async ({ page, request }) => {
    await openLocked(page, request, 'late');
    await expect(page.locator('#dApogee')).toContainText('at least');
  });

  test('a lock that fell back makes it a lower bound', async ({ page, request }) => {
    await openLocked(page, request, 'fallback');
    await expect(page.locator('#dApogee')).toContainText('at least');
    await expect(page.locator('#dApogee')).not.toContainText('13780');
  });

  /* The log's own column names decide where the event is: the firmware
     writes a thrust flag before it, which a fixed column 5 read as the event. */
  test('pyro events shown in flight summary', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dP1')).toContainText('Fired at 8.1s');
    await expect(page.locator('#dP2')).toContainText('Fired at 28.0s, 500.0 ft');
  });

  /* [WEB-UI-04] The summary names the flight it describes. */
  test('flight data names the flight on screen', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dWhich')).toContainText('Screamer (RACE01)');
  });

  /* [WEB-API-09] The one log slot can be cleared on purpose. */
  test('the flight log can be erased', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dDur')).toContainText('32.4');
    page.once('dialog', d => d.accept());
    await page.click('#btnEraseFlight');
    await expect(page.locator('#dMsg')).toContainText('erased');
    await expect(page.locator('#dWhich')).toContainText('No flight recorded');
  });

  test('flight CSV download link works', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    const [download] = await Promise.all([
      page.waitForEvent('download'),
      page.click('button:has-text("Download Flight CSV")')
    ]);
    expect(download.suggestedFilename()).toBe('flight.csv');
  });

  test('update tab shows firmware version', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');
    await expect(page.locator('#uFwVer')).toHaveText('1.3.0');
  });

  /* The OTA filename guard.
   *
   * Every board is an RP2040, so another board's image installs and runs
   * against the wrong pin map. Until the firmware checks itself, the only
   * place the board is still knowable is the filename, here. */
  test('OTA refuses another board\'s image', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');

    var alerted = null;
    page.on('dialog', async d => { alerted = d.message(); await d.dismiss(); });

    await page.setInputFiles('#fwfile', {
      name: 'fw_mk1a_fota.bin', mimeType: 'application/octet-stream',
      buffer: Buffer.from('not really firmware')
    });
    await page.click('text=⬆ Upload Firmware');
    await expect.poll(() => alerted).toContain('is for mk1a');
    expect(alerted).toContain('mk1b');
    await expect(page.locator('#fwmsg')).toContainText('Refused');
  });

  test('OTA refuses a .uf2, which is the BOOTSEL path', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');

    var alerted = null;
    page.on('dialog', async d => { alerted = d.message(); await d.dismiss(); });

    await page.setInputFiles('#fwfile', {
      name: 'fw_mk1b.uf2', mimeType: 'application/octet-stream',
      buffer: Buffer.from('UF2\n')
    });
    await page.click('text=⬆ Upload Firmware');
    await expect.poll(() => alerted).toContain('BOOTSEL');
  });

  test('OTA accepts this board\'s image, naming it in the confirm', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');

    var asked = null;
    /* Dismissed, so nothing is uploaded -- the point is the question. */
    page.on('dialog', async d => { asked = d.message(); await d.dismiss(); });

    await page.setInputFiles('#fwfile', {
      name: 'fw_mk1b_fota.bin', mimeType: 'application/octet-stream',
      buffer: Buffer.from('not really firmware')
    });
    await page.click('text=⬆ Upload Firmware');
    await expect.poll(() => asked).toContain('fw_mk1b_fota.bin');
    expect(asked).toContain('mk1b');
    expect(asked).not.toContain('cannot be checked');
  });

  test('OTA cautions on a name that does not say the board', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');

    var asked = null;
    page.on('dialog', async d => { asked = d.message(); await d.dismiss(); });

    await page.setInputFiles('#fwfile', {
      name: 'pyro_fw_c_fota_image.bin', mimeType: 'application/octet-stream',
      buffer: Buffer.from('legacy image')
    });
    await page.click('text=⬆ Upload Firmware');
    await expect.poll(() => asked).toContain('does not say which board');
  });

  test('all four tabs are navigable', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    var tabs = [
      ['Status', '#tab-status'],
      ['Config', '#tab-config'],
      ['Flight Data', '#tab-data'],
      ['Update', '#tab-update']
    ];
    for (const [name, id] of tabs) {
      await clickTab(page, name);
      await expect(page.locator(id)).toBeVisible();
    }
  });
});

/* ══════════════════════════════════════════════════════════════════
 * What the page sends, and what it believes about the answer
 * ══════════════════════════════════════════════════════════════════ */

test.describe('Saving', () => {
  test.skip(process.env.PYRO_MODE !== 'configured', 'configured mode only');
  test.afterEach(async ({ request }) => {
    await request.post(BASE + '/api/_test/reset');
  });

  /* The board refuses a POST without it: a page on another site cannot send
     it without a preflight, which the board does not grant. */
  test('every POST carries the X-Pyro header', async ({ page }) => {
    const posts = [];
    page.on('request', r => { if (r.method() === 'POST') posts.push(r.headers()['x-pyro']); });
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.fill('#p2val', '400');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('Saved', { timeout: 5000 });
    expect(posts.length).toBeGreaterThan(0);
    for (const h of posts) expect(h).toBe('1');
  });

  /* [WEB-UI-02] The board keeps a deploy value in 16 bits, in the units
     chosen: 700 m is 70000 cm, which does not fit. */
  test('a unit change that would overflow the deploy value is refused', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.selectOption('#cfgUnits', '1');
    await page.fill('#p2val', '700');
    await page.locator('#p2val').dispatchEvent('change');
    await page.selectOption('#cfgUnits', '0');
    await expect(page.locator('#cfgUnits')).toHaveValue('1');
    await expect(page.locator('#p2val')).toHaveValue('700');
    await expect(page.locator('#p2warn')).toContainText('65535');
  });

  test('an out-of-range deploy value is not saved', async ({ page }) => {
    const posts = [];
    page.on('request', r => { if (r.method() === 'POST' && r.url().endsWith('/api/config')) posts.push(r); });
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.selectOption('#cfgUnits', '0');
    await expect(page.locator('#p2val')).toHaveAttribute('max', '65535');
    await page.fill('#p2val', '70000');
    await page.locator('#p2val').dispatchEvent('change');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('not saved');
    expect(posts.length).toBe(0);
  });

  /* [WEB-UI-03] Written but not applied is pending, whatever the status code. */
  test('a config saved but not applied shows as pending', async ({ page, request }) => {
    await request.post(BASE + '/api/_test/config_reload_fails');
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.fill('#p2val', '400');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('reboot', { timeout: 5000 });
    await clickTab(page, 'Status');
    await expect(page.locator('#pendingWarn')).toBeVisible();
    await expect(page.locator('#sCfgP2')).toContainText('400');
  });

  test('an uploaded config file is shown pending with its own values', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.setInputFiles('#cfgFile', {
      name: 'config.ini', mimeType: 'text/plain',
      buffer: Buffer.from('[pyro]\r\npyro1_mode=delay\r\npyro1_value=3\r\npyro2_mode=agl\r\npyro2_value=777\r\nunits=ft\r\n')
    });
    await expect(page.locator('#cfgMsg')).toContainText('reboot', { timeout: 5000 });
    await clickTab(page, 'Status');
    await expect(page.locator('#sCfgP2')).toContainText('777');
    await expect(page.locator('#sCfgP2')).toContainText('not yet applied');
  });

  test('a pin release that failed to save is sent again on the next save', async ({ page, request }) => {
    const pins = [];
    page.on('request', r => { if (r.method() === 'POST' && r.url().endsWith('/api/pins')) pins.push(r); });
    await request.post(BASE + '/api/_test/pins_fail');
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#relTable')).toBeVisible();
    await page.click('#rel1');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#relMsg')).toContainText('✗', { timeout: 5000 });
    await page.click('#btnSaveCfg');
    await expect.poll(() => pins.length).toBe(2);
  });

  test('a Lua save whose settings were refused says so', async ({ page, request }) => {
    await request.post(BASE + '/api/_test/config_refused');
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await page.fill('#luSrc', 'function tick() end');
    await page.click('button:has-text("Save & Apply")');
    await expect(page.locator('#luChk')).toContainText('not saved', { timeout: 5000 });
  });

  test('the web files report their own version', async ({ page }) => {
    const fs = require('fs');
    const path = require('path');
    const want = fs.readFileSync(path.join(__dirname, '..', '..', 'VERSION'), 'utf8').trim();
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');
    await expect(page.locator('#uWebVer')).toHaveText(want);
  });

  /* A release's name comes from GitHub, not from this page. */
  test('a release name is shown as text, never as markup', async ({ page }) => {
    await page.route('https://api.github.com/**', route => route.fulfill({
      status: 200, contentType: 'application/json',
      body: JSON.stringify({tag_name: 'v9.9.9<img id="pwned" src="x">',
        assets: [{name: 'fw_mk1b_fota.bin', browser_download_url: 'javascript:alert(1)'}]})
    }));
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');
    await page.click('button:has-text("Check for Updates")');
    await expect(page.locator('#updMsg')).toContainText('<img');
    await expect(page.locator('#pwned')).toHaveCount(0);
    await expect(page.locator('#updMsg a[href^="javascript"]')).toHaveCount(0);
  });
});

test.describe('Firmware upload', () => {
  test.skip(process.env.PYRO_MODE !== 'flown', 'flown mode only');

  /* [WEB-UI-05] A refused image is not a reboot. */
  test('a failed OTA says it failed and why', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Update');
    page.on('dialog', async d => { await d.accept(); });
    await page.setInputFiles('#fwfile', {
      name: 'fw_mk1b_fota.bin', mimeType: 'application/octet-stream',
      buffer: Buffer.from('BAD image')
    });
    await page.click('text=⬆ Upload Firmware');
    await expect(page.locator('#fwmsg')).toContainText('failed', { timeout: 5000 });
    await expect(page.locator('#fwmsg')).toContainText('did not write');
    await expect(page.locator('#fwmsg')).not.toContainText('Rebooting');
  });
});
