// @ts-check
const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: '.',
  testMatch: '*.spec.js',
  timeout: 15000,
  retries: 0,
  use: {
    headless: true,
    viewport: { width: 800, height: 600 },
  },
  projects: [
    { name: 'chromium', use: { browserName: 'chromium' } },
  ],
});
