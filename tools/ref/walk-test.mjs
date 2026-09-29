// Dev-only: the scripted walker runs of `oceandrive --dump @walk`, on the JS app's Walker
// (Walker.simulate, 1/60 s steps), printed in the same format.
//   node c/tools/ref/walk-test.mjs [NSTATIC_CIRCLES]
// NSTATIC_CIRCLES: keep only the first N walk circles (street furniture + palms), i.e. leave
// out colliders of modules the C port has not added yet (people, vehicles).
import { chromium } from 'playwright';

const nStatic = process.argv[2] ? +process.argv[2] : null;
const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--use-angle=swiftshader'] });
const page = await browser.newPage({ viewport: { width: 320, height: 200 } });
await page.goto('http://localhost:5173/?shot=1');
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
const lines = await page.evaluate((nStatic) => {
  const w = window.__walker, W = w.world, out = [];
  out.push(`world boxes=${W.boxes.length} circles=${W.circles.length}`);
  const circles = W.circles;
  if (nStatic != null) W.circles = circles.slice(0, nStatic);
  const RUNS = [
    [false, -26, 1.85, 40, 342, 4, ['KeyW'], 8],
    [false, -20, 1.85, 0, 90, 0, ['KeyW'], 25],
    [true, 60, 0, -10, 90, 0, ['KeyW', 'ShiftLeft'], 20],
    [true, 40, 0, 13, 35, 0, ['KeyW'], 10],
    [false, -26, 1.85, 0, 270, 0, ['KeyW'], 4],
    [false, -26, 1.85, 40, 180, 0, ['KeyW', 'Space'], 3],
    [false, -15, 1.85, 320, 180, 0, ['KeyW'], 10],
    [false, -18, 1.85, -60, 45, 0, ['KeyD', 'KeyS'], 6],
  ];
  let steps = 0;
  const onStep = w.onStep;
  w.onStep = () => { steps++; };
  const g = (v) => String(v);
  RUNS.forEach(([tele, x, y, z, h, p, keys, secs], i) => {
    if (tele) w.teleport(x, z, h, p); else w.set(x, y, z, h, p);
    steps = 0;
    w.simulate(keys, secs);
    const c = w.camera.position;
    out.push(`run${i} x=${g(w.pos.x)} z=${g(w.pos.y)} feet=${g(w.feetY)} phase=${g(w.phase)} steps=${steps} cam=${g(c.x)},${g(c.y)},${g(c.z)}`);
  });
  w.onStep = onStep;
  W.circles = circles;
  return out;
}, nStatic);
console.log(lines.join('\n'));
await browser.close();
