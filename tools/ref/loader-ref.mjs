// screenshots of the JS page's loader / overlay at given times (ms after navigation)
import { chromium } from 'playwright';
const out = process.argv[2], times = process.argv.slice(3).map(Number);
const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--use-angle=swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
const t0 = Date.now();
await page.goto('http://localhost:5173/' + (process.env.Q || ''));
for (const t of times) {
  if (t === -1) { await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 }); await page.waitForTimeout(2500); }
  else { const w = t - (Date.now() - t0); if (w > 0) await page.waitForTimeout(w); }
  await page.screenshot({ path: `${out}-${t}.png` });
  console.log('shot', t, await page.evaluate(() => document.querySelector('.ld-label')?.textContent + ' ' + document.querySelector('.ld-pct')?.textContent));
}
await browser.close();
