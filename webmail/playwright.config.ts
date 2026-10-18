import { defineConfig, devices } from "@playwright/test";
import { readFileSync } from "node:fs";
const state = process.env.WEBMAIL_E2E_STATE;
if (!state)
  throw new Error(
    "Run test/scripts/webmail_ui_fixture.py with a local DB config; it supplies WEBMAIL_E2E_STATE.",
  );
const fixture = JSON.parse(readFileSync(state, "utf8"));
export default defineConfig({
  testDir: "./tests",
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 30000,
  expect: { timeout: 8000 },
  reporter: "list",
  outputDir: "../artifacts/webmail-tests",
  use: {
    baseURL: fixture.baseURL,
    ...devices["Desktop Chrome"],
    viewport: { width: 1440, height: 960 },
    channel: "chromium",
    trace: "retain-on-failure",
  },
});
