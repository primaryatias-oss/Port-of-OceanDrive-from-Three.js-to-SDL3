// Dev-only: the scripted vehicle rides of `oceandrive --dump @ride`, on the JS app's vehicles
// (vehicles.simulate, 1/60 s steps), printed in the same format (ms / mb rounded to 0.01 as the
// JS simulate returns them).
//   node c/tools/ref/ride-test.mjs
import { chromium } from 'playwright';

const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--use-angle=swiftshader'] });
const page = await browser.newPage({ viewport: { width: 320, height: 200 } });
await page.goto('http://localhost:5173/?shot=1');
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
const lines = await page.evaluate(() => {
  const V = window.__vehicles, out = [];
  const pr = (tag, r) => {
    const v = V.current;
    if (!v) { out.push(`${tag} none`); return; }
    out.push(`${tag} x=${v.x} z=${v.z} yaw=${v.yaw} lon=${v.lon} gy=${v.groundY} by=${v.bodyY} pitch=${v.pitch} roll=${v.roll} wr=${v.wheelRot} crank=${v.crank} rpm=${v.rpm} ms=${r.maxSpeed} mb=${r.maxBump}`);
  };
  V.mount('bike');
  pr('bike1', V.simulate(['KeyW'], 6));
  pr('bike2', V.simulate(['KeyW', 'ShiftLeft', 'KeyD'], 4));
  V.place(20, 0, -Math.PI / 2);
  pr('bike3', V.simulate(['KeyW'], 8));
  pr('bike4', V.simulate(['KeyW', 'KeyA'], 3));
  pr('bike5', V.simulate(['KeyW', 'Space'], 2));
  pr('bike6', V.simulate(['KeyS'], 3));
  out.push(`dismount ${+V.dismount()}`);
  V.mount('atv');
  pr('atv1', V.simulate(['KeyW', 'ShiftLeft'], 8));
  pr('atv2', V.simulate(['KeyW', 'KeyD'], 5));
  pr('atv3', V.simulate(['KeyS'], 3));
  V.place(60, 30, -Math.PI / 2);
  pr('atv4', V.simulate(['KeyW', 'Space'], 6));
  pr('atv5', V.simulate([], 2));
  return out;
});
console.log(lines.join('\n'));
await browser.close();
