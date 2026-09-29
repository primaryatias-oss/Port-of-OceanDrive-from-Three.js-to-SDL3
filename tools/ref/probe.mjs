// Dev-only: evaluates an expression in the JS app (?shot=1 by default) once the scene is ready
// and prints the JSON result.   node c/tools/ref/probe.mjs 'EXPR' [query]
import { chromium } from 'playwright';
const [expr, query = 'shot=1'] = process.argv.slice(2);
const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--use-angle=swiftshader'] });
const page = await browser.newPage({ viewport: { width: 320, height: 200 } });
await page.goto(`http://localhost:5173/?${query}`);
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
console.log(JSON.stringify(await page.evaluate((e) => (0, eval)(e), expr)));
await browser.close();
