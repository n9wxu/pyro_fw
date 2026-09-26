// @ts-check
/* H2: the browser demo, as a visitor finds it. docs/sim.html runs the flight
   software compiled to WASM against the page's physics; from power-on it must
   reach the pad, launch, and land.

   Needs SIM_URL, where static_server.js serves docs/. */
const { test, expect } = require('@playwright/test');

const SIM_URL = process.env.SIM_URL;

test.describe('Browser simulator', () => {
  test.skip(!SIM_URL, 'Skipped: SIM_URL not set');

  test('flies from power-on to LANDED', async ({ page }) => {
    test.setTimeout(180000);
    await page.goto(SIM_URL + '/sim.html');
    await expect(page.locator('#modeBadge')).toContainText('WASM', { timeout: 30000 });
    await page.selectOption('#speed', '1600');
    await page.click('#btnPower');
    await expect(page.locator('#btnLaunch')).toBeEnabled({ timeout: 30000 });
    await page.click('#btnLaunch');
    await expect(page.locator('#state')).toHaveText('LANDED', { timeout: 150000 });
    await expect(page.locator('#p1status')).toContainText('FIRED');
  });
});
