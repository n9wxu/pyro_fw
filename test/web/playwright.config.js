// @ts-check
const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: '.',
  testMatch: '*.spec.js',
  timeout: 30000,
  retries: 1,
  use: {
    headless: true,
    viewport: { width: 800, height: 600 },
    screenshot: 'only-on-failure',
    trace: 'on-first-retry',
  },
  reporter: [['list'], ['html', { open: 'never' }]],
  projects: [
    { name: 'chromium', use: { browserName: 'chromium' } },
  ],
});
