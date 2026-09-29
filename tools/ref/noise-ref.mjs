// Dev-only: checksums of src/textures/noise.js textures in the browser, for tests/test_noise.c.
//   node c/tools/ref/noise-ref.mjs
import { chromium } from 'playwright';

const browser = await chromium.launch({ headless: true, args: ['--use-angle=swiftshader'] });
const page = await browser.newPage();
await page.goto('http://localhost:5173/');
const lines = await page.evaluate(async () => {
  const m = await import('/src/textures/noise.js');
  const report = (name, px) => {
    let h = 2166136261 >>> 0, sum = 0;
    for (let i = 0; i < px.length; i++) { h = Math.imul(h ^ px[i], 16777619) >>> 0; sum += px[i]; }
    return `${name} ${sum} ${h}`;
  };
  const out = [];
  const t = m.noiseColorTexture({ size: 512, seed: 91, colorA: [242, 240, 236], colorB: [255, 255, 255], baseCells: 3, speckle: 0.004, contrast: 1.0 });
  out.push(report('color91', t.image.getContext('2d').getImageData(0, 0, 512, 512).data));
  out.push(report('normal17', m.noiseNormalTexture({ size: 512, seed: 17, baseCells: 48, strength: 0.11, octaves: 3 }).image.data));
  out.push(report('normal_default', m.noiseNormalTexture({}).image.data));
  return out;
});
console.log(lines.join('\n'));
await browser.close();
