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
    const dur = await page.locator('#dDur').textContent();
    expect(dur).toContain('0.0');
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
    await page.click('#btnSaveRel');
    await expect(page.locator('#relMsg')).toContainText('saved', { timeout: 5000 });
  });

  /* TODO from the clean-wipe decision: a firmware update wipes the
     filesystem, so a program that lives only on the device is one the next
     update destroys. Import must land in the editor and not on the device,
     so a mis-picked file costs nothing until Save. */
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
    const dur = await page.locator('#dDur').textContent();
    expect(dur).toContain('32.4');
    const apogee = await page.locator('#dApogee').textContent();
    expect(apogee).toContain('10000');
  });

  test('pyro events shown in flight summary', async ({ page }) => {
    await page.goto(BASE);
    await waitForStatus(page);
    await clickTab(page, 'Flight Data');
    const p1 = await page.locator('#dP1').textContent();
    const p2 = await page.locator('#dP2').textContent();
    expect(p1).toContain('Fired');
    expect(p2).toContain('Fired');
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
