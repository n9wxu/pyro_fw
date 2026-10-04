// @ts-check
const { test, expect } = require('@playwright/test');

/*
 * Web UI tests against mock Pyro server [TST-07].
 *
 * Verifies [SYS-WEB-01, SYS-CFG-02, SYS-CFG-03, WEB-API-01, WEB-API-02,
 * WEB-API-03, WEB-API-06, WEB-API-09, WEB-API-12, WEB-UI-01, WEB-UI-02,
 * WEB-UI-03, WEB-UI-04, WEB-UI-05, WEB-UI-06, BUZ-CODE-11, LUA-MGT-01,
 * LUA-MGT-02, LUA-IO-01, LUA-IO-02, LUA-PAD-01, LUA-PAD-03, GND-TEST-12,
 * USB-08, PYR-BOARD-02, PYR-BOARD-03, CFG-10].
 *
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

  /* A flight that happens while the page is open must be shown. The
     tab cached the log for the life of the page. */
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

  /* The shipped default name fits its 8-character field. */
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

  /* SNS-EST-06: the choice is of the estimators the board says it carries. */
  test('estimator: chosen from what the board carries, and saved', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#estimator option')).toHaveText(['lumped', 'constacc']);
    await expect(page.locator('#estimator')).toHaveValue('lumped');
    await page.selectOption('#estimator', 'constacc');
    await expect(page.locator('#cfgDirty')).toBeVisible();
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('Saved', { timeout: 5000 });
    const ini = await (await page.request.get(BASE + '/api/config')).text();
    expect(ini).toContain('estimator=constacc');
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

  /* CFG-10, WEB-UI-03: a saved change is stored, not in force. The status tab
     goes on showing what the board is flying on, and says a change waits. */
  test('a saved change is flagged as waiting for a reboot, not shown as active', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.fill('#p2val', '400');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('reboot to apply', { timeout: 5000 });
    await clickTab(page, 'Status');
    await expect(page.locator('#pendingWarn')).toBeVisible({ timeout: 5000 });
    await expect(page.locator('#sCfgP2')).toContainText('500');
    await expect(page.locator('#sCfgP2')).toContainText('waiting for a reboot');
  });

  /* PYR-REFIRE-01, FLT-EMRG-01, PYR-DEPLOY-02: the fire rules are edited
     here and saved with the rest of the tab. */
  test('the fire rules are saved with the rest of the tab', async ({ page, request }) => {
    await request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.fill('#p1refire', '80');
    await page.fill('#emrgSpeed', '130');
    await page.fill('#fireGap', '4000');
    await page.click('#btnSaveCfg');
    await expect(page.locator('#cfgMsg')).toContainText('Saved', { timeout: 5000 });
    const posts = await (await request.get(`${BASE}/api/_test/config_posts`)).json();
    const ini = posts[posts.length - 1];
    expect(ini).toContain('pyro1_refire_speed=80');
    expect(ini).toContain('pyro2_refire_speed=0');
    expect(ini).toContain('emergency_fire_speed=130');
    expect(ini).toContain('refire_interval=0');
    expect(ini).toContain('fire_gap=4000');
    expect(ini).not.toContain('telem_');
  });

  test('the stored fire rules load into the editor', async ({ page, request }) => {
    await request.post(`${BASE}/api/config`, { data: '[pyro]\r\npyro2_refire_speed=45\r\nrefire_interval=1500\r\n' });
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#p2refire')).toHaveValue('45');
    await expect(page.locator('#refireInterval')).toHaveValue('1500');
    await expect(page.locator('#emrgSpeed')).toHaveValue('0');
  });

  /* PYR-BOARD-02, PYR-BOARD-03: the board's range is the board's to state,
     and a value outside it is brought in, not refused. */
  test('a pyro timing value outside the board range says what the board will use', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#fireGapHint')).toContainText('1000 to 10000 ms');
    await expect(page.locator('#fireGapHint')).toContainText('3000 ms');
    await page.fill('#fireGap', '200');
    await expect(page.locator('#fireGapWarn')).toContainText('it will use 1000 ms');
    await page.fill('#fireGap', '2500');
    await expect(page.locator('#fireGapWarn')).toHaveText('');
    await page.fill('#refireInterval', '60000');
    await expect(page.locator('#refireIntervalWarn')).toContainText('it will use 10000 ms');
  });

  test('a value the board brought into range is flagged on the status tab', async ({ page, request }) => {
    await request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#limitWarn')).toBeHidden();
    await request.post(`${BASE}/api/_test/status`, { data: JSON.stringify({ pyro_limited: true }) });
    await expect(page.locator('#limitWarn')).toBeVisible({ timeout: 5000 });
    await expect(page.locator('#sCfgGap')).toHaveText('3000 ms');
    await request.post(`${BASE}/api/_test/reset`);
  });

  /* FLT-BROWN-05: whether this start resumed a flight, and if not, why. */
  test('the status tab says whether this start resumed a flight', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#sResume')).toHaveText('not resumed: no record');
  });

  /* A speed is a distance a second in the chosen units, so it converts too. */
  test('changing units converts the fire rule speeds', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.fill('#emrgSpeed', '100');
    await page.fill('#fireGap', '4000');
    await page.selectOption('#cfgUnits', '1');
    await expect(page.locator('#emrgSpeed')).toHaveValue('30');
    await expect(page.locator('#fireGap')).toHaveValue('4000'); /* milliseconds stay */
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

  /* Changing units converts the deployment altitudes. Leaving the
     number alone turned a 500 ft main into a 500 m main. */
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

  /* The 8-character limit is stated, not discovered. */
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

  /* SYS-CFG-03, SNS-MAX-01: the limit is the board's own, served by it. The
     mock's sensor reaches 9000 m, 29528 ft. */
  test('range warning for a height above the board\'s height for proper operation', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await page.selectOption('#p2mode', 'agl');
    await page.fill('#p2val', '29000');
    await expect(page.locator('#p2warn')).toHaveText('');
    await page.fill('#p2val', '30000');
    await expect(page.locator('#p2warn')).toContainText('29528 ft');
    await expect(page.locator('#p2warn')).toContainText('height for proper operation');
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
    /* GND-TEST-12: the switch may join the buzzer's pad to another, as the driven pad only. */
    await expect(page.locator('#gtDrive option[value="16"]')).toHaveCount(1);
    await expect(page.locator('#gtDrive option[value="16"]')).toContainText('the buzzer');
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

  /* One Save on the Config tab. The release no longer has a button of
     its own that an operator has to guess the meaning of. */
  test('the config tab has one save button', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Config');
    await expect(page.locator('#tab-config button:has-text("Save")')).toHaveCount(1);
  });

  /* [LUA-IO-01/02] A program that lives only on the device is lost with its
     filesystem. Import must land in the editor and not on the device, so a
     mis-picked file costs nothing until Save. */
  /* LUA-MGT-01: a script that fails its check is not stored. */
  test('a script that fails its check is not saved', async ({ page, request }) => {
    await request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await page.fill('#luSrc', 'this is a syntax error');
    await page.click('text=Save & Apply');
    await expect(page.locator('#luChk')).toContainText('not ready', { timeout: 5000 });
    await expect(page.locator('#luChk')).toContainText('Not saved');
    const lua = await (await request.get(`${BASE}/api/_test/lua`)).json();
    expect(lua.posts).toBe(0);
  });

  /* LUA-MGT-01: edit, check, save and remove. */
  test('a script that passes its check is saved, and can be removed', async ({ page, request }) => {
    await request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await page.fill('#luSrc', 'function tick() end');
    await page.click('text=✓ Check');
    await expect(page.locator('#luChk')).toContainText('ready for flight', { timeout: 5000 });
    await page.click('text=Save & Apply');
    await expect(page.locator('#luChk')).toContainText('Saved', { timeout: 5000 });
    let lua = await (await request.get(`${BASE}/api/_test/lua`)).json();
    expect(lua.script).toBe('function tick() end');
    page.once('dialog', d => d.accept());
    await page.click('#btnLuaRemove');
    await expect(page.locator('#luChk')).toContainText('Removed', { timeout: 5000 });
    lua = await (await request.get(`${BASE}/api/_test/lua`)).json();
    expect(lua.script).toBe('');
    await expect(page.locator('#luSrc')).toHaveValue('');
  });

  /* LUA-MGT-02: the console says whether the script runs, and shows what it printed. */
  test('the console shows the script state and its output', async ({ page, request }) => {
    await request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Lua');
    await expect(page.locator('#luState')).toHaveText('enabled, no script', { timeout: 5000 });
    await request.post(`${BASE}/api/lua/script`, { data: 'print("hello from the script")' });
    await expect(page.locator('#luState')).toHaveText('running', { timeout: 5000 });
    await expect(page.locator('#luCon')).toContainText('hello from the script');
  });

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
    await expect(table).toContainText('take it to the workbench');
    await expect(table).toContainText('Check pyro 1');
    await expect(table).toContainText('Check pyro 2');
    await expect(table).toContainText('OK to fly');
  });

  /* BUZ-CODE-01, BUZ-CODE-11: four outcomes, each with its own sound, and
     they come from the board. */
  test('the vocabulary is four outcomes, one row each', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Beep Codes');
    for (const key of ['general_fault', 'check_pyro_1', 'check_pyro_2', 'ok_to_fly'])
      await expect(page.locator('#brow' + key)).toBeVisible();
    await expect(page.locator('#bpTable tr')).toHaveCount(5); /* the heading and four outcomes */
    await expect(page.locator('#bpSplit')).toHaveCount(0);
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

  /* PYR-FIRE-01, FLT-EMRG-04, PYR-FAULT-03: every pulse is counted, and what
     the board saw of it is shown. */
  test('the status tab counts the pulses and shows an emergency fire and a fault', async ({ page, request }) => {
    await request.post(`${BASE}/api/_test/reset`);
    await page.goto(BASE);
    await waitForStatus(page);
    await expect(page.locator('#sP1')).toContainText('×3');
    await expect(page.locator('#sP2')).not.toContainText('×');
    await expect(page.locator('#sEmrg')).toHaveText('No');
    await request.post(`${BASE}/api/_test/status`,
      { data: JSON.stringify({ emergency_fire: true, pyro_fault: [false, true] }) });
    await expect(page.locator('#sEmrg')).toContainText('FIRED', { timeout: 5000 });
    await expect(page.locator('#sP2')).toContainText('reported a fault');
    await expect(page.locator('#sP1')).not.toContainText('reported a fault');
    await request.post(`${BASE}/api/_test/reset`);
  });

  test('flight data tab shows duration and apogee', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    /* The tab fetches the log on opening: wait for it, don't sample. */
    await expect(page.locator('#dDur')).toContainText('32.4');
    await expect(page.locator('#dApogee')).toContainText('10000');
  });

  /* FLT-APO-08: the summary's apogee is the log's PEAK row, not the highest
     row, and "at least" when the firmware marked it a lower bound. */
  test('apogee is the PEAK row', async ({ page, request }) => {
    await request.post(BASE + '/api/_test/fly_peak/seen');
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dApogee')).toContainText('8202');
    await expect(page.locator('#dApogee')).not.toContainText('at least');
  });

  test('a PEAK_AT_LEAST row makes it a lower bound', async ({ page, request }) => {
    await request.post(BASE + '/api/_test/fly_peak/bound');
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dApogee')).toContainText('at least 8202');
  });

  /* A log written before DD-092 has the Mach lock's rows instead: the apogee
     comes from the rows outside the lock -- and is only a lower bound if the
     lock let go within 2 s of apogee, or never did. */
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

  /* The summary names the flight it describes. */
  test('flight data names the flight on screen', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    await expect(page.locator('#dWhich')).toContainText('Screamer (RACE01)');
  });

  /* The one log slot can be cleared on purpose. */
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
