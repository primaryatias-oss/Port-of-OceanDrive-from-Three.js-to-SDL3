// Dev-only: renders a reference frame from the original JS app (headless Chromium, ?shot=1:
// frozen time, 'high' tier) and dumps the exact camera state next to it, so the C port can
// render the same view for an image diff.
//   node c/tools/ref/ref-shot.mjs OUT_BASENAME x y z heading pitch [keep]
// keep: 'all' (default), 'sky' (only the sky dome and the lights) or a comma list of top-level
// object names to keep besides those.
// Writes OUT_BASENAME.png and OUT_BASENAME.json (camera + size).
import { chromium } from 'playwright';
import fs from 'node:fs';

const [out, x, y, z, heading, pitch, keep = 'all', w = '1024', h = '576'] = process.argv.slice(2);
const W = +w, H = +h;
const browser = await chromium.launch({
  headless: true,
  args: ['--mute-audio', '--ignore-gpu-blocklist', '--enable-gpu', '--use-angle=vulkan', '--enable-features=Vulkan'],
});
const page = await browser.newPage({ viewport: { width: W, height: H }, deviceScaleFactor: 1 });
const errors = [];
// src/renderer/post.js makes only the composer's first target multisampled, but the scene pass
// renders into readBuffer, which alternates between the two targets every frame (3 swapping
// passes per frame): the JS app gets MSAA on every other frame only. The port always renders the
// scene with MSAA (what post.js intends), so the reference keeps both targets multisampled.
await page.route('**/src/renderer/post.js*', async (route) => {
  const res = await route.fetch();
  const body = (await res.text()).replace('composer.renderTarget2.samples = 0;', '');
  await route.fulfill({ response: res, body });
});
page.on('pageerror', (e) => errors.push(String(e)));
// REF_QUERY: extra query parameters (e.g. 'people=closeup'; the C side takes EXTRA='--people closeup')
await page.goto(`http://localhost:5173/?shot=1${process.env.REF_QUERY ? '&' + process.env.REF_QUERY : ''}`);
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
const cam = await page.evaluate(({ x, y, z, heading, pitch, keep }) => {
  window.__setCam(+x, +y, +z, +heading, +pitch);
  if (keep !== 'all') {
    // 'sky' or a comma list of top-level object names to keep (the sky dome and lights stay)
    const names = keep === 'sky' ? [] : keep.split(',');
    // 'cars': car.js adds its objects unnamed straight to the scene, right after the street:
    // 4 sedans, the hero, 8 fleet meshes, 6 district blocks and 3 moving cars
    const kids = window.__scene.children, st = kids.findIndex((o) => o.name === 'street');
    const cars = names.includes('cars') && st >= 0 ? new Set(kids.slice(st + 1, st + 23)) : new Set();
    // 'beach': buildBeach's unnamed objects, after the cars and up to the ocean
    if (names.includes('beach') && st >= 0) for (const o of kids.slice(st + 23, kids.findIndex((q) => q.name === 'ocean'))) cars.add(o);
    // 'people': the figures (SkinnedMesh + shadow SkinnedMesh + skeleton root bone each)
    if (names.includes('people')) for (const o of kids) if (o.isSkinnedMesh || o.isBone) cars.add(o);
    // 'vehicles': the bike and ATV with their blob shadows, and the spray points (after the people)
    if (names.includes('vehicles')) for (const o of kids.slice(kids.findLastIndex((q) => q.isSkinnedMesh) + 1).slice(0, 5)) cars.add(o);
    for (const o of kids) {
      const isDome = o.isMesh && o.material?.name === 'Sky';
      if (!isDome && !o.isLight && !names.includes(o.name) && !cars.has(o)) o.visible = false;
    }
  }
  const c = window.__walker.camera ?? window.__walker.cam ?? null;
  return c ? { p: c.position.toArray(), q: c.quaternion.toArray(), fov: c.fov, aspect: c.aspect, near: c.near, far: c.far } : null;
}, { x, y, z, heading, pitch, keep });
// the camera after the frames have run: the walker eases the eye height toward the ground with
// real frame time, so wait until it has settled, and check it did not move during the capture
const readCam = () => page.evaluate(() => {
  const c = window.__walker.camera ?? null;
  return c ? { p: c.position.toArray(), q: c.quaternion.toArray(), fov: c.fov, aspect: c.aspect, near: c.near, far: c.far } : null;
});
let cam2 = null;
for (let i = 0; i < 60; i++) {
  await page.waitForTimeout(250);
  const c = await readCam();
  if (cam2 && JSON.stringify(c) === JSON.stringify(cam2)) break;
  cam2 = c;
}
await page.waitForTimeout(500);
await page.screenshot({ path: `${out}.png` });
const cam3 = await readCam();
const canvasInfo = await page.evaluate(() => {
  const c = document.querySelector('canvas');
  const gl = document.createElement('canvas').getContext('webgl2');
  const ext = gl?.getExtension('WEBGL_debug_renderer_info');
  const app = c.getContext('webgl2');   // the app's own context
  return { w: c.width, h: c.height, cssW: c.clientWidth, cssH: c.clientHeight, dyn: window.__dynres?.(),
           maxSamples: app?.getParameter(app.MAX_SAMPLES), attrs: app?.getContextAttributes(),
           gpu: ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : null };
});
if (JSON.stringify(cam3) !== JSON.stringify(cam2)) errors.push('camera moved during the capture');
fs.writeFileSync(`${out}.json`, JSON.stringify({ w: W, h: H, keep, camera: cam2 ?? cam, canvas: canvasInfo, errors }, null, 1));
console.log('wrote', `${out}.png`, JSON.stringify(cam2 ?? cam));
await browser.close();
