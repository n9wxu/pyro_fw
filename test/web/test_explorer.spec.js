// @ts-check
/* The flight data explorer, as a visitor finds it. docs/flight_explorer.html
   plots flight logs and other sensor data as layers on one time axis, from
   files on the disk or in the repository; GitHub is answered here from
   test_data/, so the suite needs no network.

   Needs SIM_URL, where static_server.js serves docs/. */
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const SIM_URL = process.env.SIM_URL;
const TEST_DATA = path.resolve(__dirname, '../../test_data');
const ALTOS = 'altos/2012-06-29-serial-058-flight-005.csv';

/* A flight to 100 m at 5 s: one reading every 20 ms, 8.43 cm a pascal. */
function pyroLog() {
  const rows = ['# Pyro MK1C Flight Data', '# Ground Pa: 100000', 'time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event'];
  for (let ms = -1000; ms <= 12000; ms += 20) {
    const t = ms / 1000, m = t < 0 || t > 10 ? 0 : 100 - 4 * (t - 5) * (t - 5), pa = Math.round(100000 - m / 0.0843);
    const event = ms === 200 ? 'LAUNCH' : ms === 4000 ? 'ARMED' : ms === 5200 ? 'APOGEE' : ms === 10400 ? 'LANDING' : '';
    rows.push(`${ms},${pa},${Math.round(m * 100)},4,0,${pa + (ms % 40 ? 2 : -2)},20.0,${event}`);
    if (ms > 0 && ms % 1000 === 0) {
      rows.push(`${ms},,,,,,,LUA count ${ms / 1000}`);
      rows.push(`${ms},,,,,,,EST simple cm=${Math.round(m * 100)} cm/s=${Math.round(-800 * (t - 5))} released=0`);
    }
  }
  return rows.join('\n') + '\n';
}
const parabola = () => 'time_s,height_m\n' + Array.from({ length: 101 }, (_, i) => `${i / 10},${100 - 4.9 * (i / 10 - 5) ** 2}`).join('\n') + '\n';
const disk = (name, text) => ({ name, mimeType: 'text/csv', buffer: Buffer.from(text) });

test.describe('Flight data explorer', () => {
  test.skip(!SIM_URL, 'Skipped: SIM_URL not set');

  test.beforeEach(async ({ page }) => {
    await page.route('https://api.github.com/**', r => r.fulfill({
      json: { tree: [{ type: 'blob', path: `test_data/${ALTOS}` }, { type: 'blob', path: 'test_data/open_rocket_export.csv' },
                     { type: 'blob', path: 'README.md' }, { type: 'tree', path: 'test_data' }] } }));
    await page.route('https://raw.githubusercontent.com/**', r => {
      const file = path.join(TEST_DATA, decodeURIComponent(new URL(r.request().url()).pathname.split('/test_data/')[1]));
      r.fulfill({ contentType: 'text/plain', body: fs.readFileSync(file) });
    });
    await page.setViewportSize({ width: 1200, height: 900 });
  });

  test('a flight log from the disk is plotted with its flight events', async ({ page }) => {
    await page.goto(SIM_URL + '/flight_explorer.html');
    await page.setInputFiles('#fileInput', disk('flight.csv', pyroLog()));
    await expect(page.locator('#spanText')).toHaveText('Showing everything, -1.00 s to 12.00 s');
    await expect(page.locator('#events button')).toHaveText([/0\.200 s\s*LAUNCH/, /4\.000 s\s*ARMED/, /5\.200 s\s*APOGEE/, /10\.400 s\s*LANDING/]);
    await expect(page.locator('#panels canvas')).toHaveCount(1);
    await page.locator('label', { hasText: 'Pressure, raw reading' }).click();
    await expect(page.locator('#panels h3')).toHaveText(['Height, m', 'Pressure, Pa']);
  });

  test('a drag across a plot shows that span, and the pointer reads each layer', async ({ page }) => {
    await page.goto(SIM_URL + '/flight_explorer.html');
    await page.setInputFiles('#fileInput', disk('flight.csv', pyroLog()));
    const box = await page.locator('#panels canvas').first().boundingBox();
    const x = t => box.x + 64 + (t + 1) / 13 * (box.width - 78);
    await page.mouse.move(x(4), box.y + 150);
    await page.mouse.down();
    await page.mouse.move(x(6), box.y + 150, { steps: 4 });
    await page.mouse.up();
    await expect(page.locator('#spanText')).toHaveText(/^Showing [34]\.\d+ s to [56]\.\d+ s \(2\.0\d+ s\)/);
    await page.mouse.move(box.x + box.width / 2, box.y + 150);
    await expect(page.locator('#readout')).toHaveText(/t = [45]\.\d+ s.*Height, raw reading: \d+\.\d+ m.*Height, Kalman filter: \d+\.\d+ m/s);
    await page.click('#whole');
    await expect(page.locator('#spanText')).toHaveText(/^Showing everything/);
  });

  test('the events are narrowed by source and by name, and one is shown on request', async ({ page }) => {
    await page.goto(SIM_URL + '/flight_explorer.html');
    await page.setInputFiles('#fileInput', disk('flight.csv', pyroLog()));
    await page.uncheck('#src_flight');
    await page.check('#src_lua');
    await expect(page.locator('#events button')).toHaveCount(12);
    await expect(page.locator('#events button').first()).toHaveText(/LUA count 1/);
    await page.check('#src_flight');
    await page.fill('#eventText', 'apogee, count 7');
    await expect(page.locator('#events button')).toHaveText([/APOGEE/, /LUA count 7/]);
    await page.locator('#events button', { hasText: 'APOGEE' }).click();
    await expect(page.locator('#spanText')).toHaveText(/^Showing 4\.200 s to 6\.200 s/);
  });

  test('a second order fit of the span shown reports the top of the flight', async ({ page }) => {
    await page.goto(SIM_URL + '/flight_explorer.html');
    await page.setInputFiles('#fileInput', disk('arc.csv', parabola()));
    await page.selectOption('#toolKind', 'poly');
    await page.fill('#pOrder', '2');
    await page.click('#toolRun');
    await expect(page.locator('#toolNote')).toHaveText(/top 100\.000 m at 5\.000 s/);
    await expect(page.locator('#derived .layer')).toHaveCount(1);
    await page.selectOption('#toolKind', 'local');
    await page.selectOption('#pOutput', '1');
    await page.click('#toolRun');
    await expect(page.locator('#panels h3')).toHaveText(['Height, m', 'Speed, m/s']);
    await page.selectOption('#toolKind', 'interp');
    await page.selectOption('#pMethod', 'spline');
    await page.fill('#pStep', '20');
    await page.click('#toolRun');
    await expect(page.locator('#toolNote')).toHaveText(/cubic spline; 101 readings to 501 points/);
  });

  test('the files of the repository are listed and added as layers beside a file from the disk', async ({ page }) => {
    await page.goto(SIM_URL + '/flight_explorer.html');
    await expect(page.locator('#repoNote')).toHaveText('2 CSV files on main.');
    await page.setInputFiles('#fileInput', disk('flight.csv', pyroLog()));
    await page.fill('#repoFilter', 'altos');
    await expect(page.locator('#repoFiles option')).toHaveCount(1);
    await page.selectOption('#repoFiles', `test_data/${ALTOS}`);
    await page.click('#repoAdd');
    await expect(page.locator('.filename')).toHaveText(['flight.csv', path.basename(ALTOS)]);
    await expect(page.locator('#events button', { hasText: 'DROGUE' })).toHaveCount(1);
    await page.locator('label', { hasText: 'Acceleration, accelerometer' }).click();
    await expect(page.locator('#panels h3')).toHaveText(['Height, m', 'Acceleration, m/s²']);
  });

  test('a link that names a repository file opens it', async ({ page }) => {
    await page.goto(SIM_URL + '/flight_explorer.html?file=test_data/open_rocket_export.csv');
    await expect(page.locator('.filename')).toHaveText(['open_rocket_export.csv']);
    await expect(page.locator('#panels h3')).toHaveText(['ft']);
    await expect(page.locator('#events button', { hasText: 'APOGEE' })).toHaveCount(1);
  });
});
