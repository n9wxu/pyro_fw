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
    /* Trigger onchange since fill may not */
    await page.locator('#p2val').dispatchEvent('change');
    await page.click('button:has-text("Save")');
    await expect(page.locator('#cfgMsg')).toContainText('Saved', { timeout: 5000 });
    await expect(page.locator('#cfgDirty')).toBeVisible();
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
    const warn = await page.locator('#p2warn').textContent();
    expect(warn).toContain('sensor limit');
  });
});

/* ══════════════════════════════════════════════════════════════════
   Mode: FLOWN — post-flight with data
   ══════════════════════════════════════════════════════════════════ */

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
    for (const tab of ['Status', 'Config', 'Flight Data', 'Update']) {
      await clickTab(page, tab);
      const panel = page.locator(`#tab-${tab.toLowerCase().replace(' ', '-')}, #tab-${tab.toLowerCase().split(' ')[0]}`).first();
      await expect(panel).toBeVisible();
    }
  });
});
