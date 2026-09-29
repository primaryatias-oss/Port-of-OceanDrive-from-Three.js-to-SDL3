// Dev-only: dumps a checksum of every mesh under a top-level scene object of the JS app (vertex
// count, sums of positions / normals / colours, instance matrices and colours), in scene order,
// for the C port's matching OD_DUMP output.
//   node c/tools/ref/dump-group.mjs NAME > out.txt
import { chromium } from 'playwright';

const [name, query = 'shot=1'] = process.argv.slice(2);   // query: e.g. 'quality=low'
const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--use-angle=swiftshader'] });
const page = await browser.newPage({ viewport: { width: 320, height: 200 } });
// keep the CPU arrays (src/renderer/memory.js drops them after upload)
await page.route('**/src/renderer/memory.js*', async (route) => {
  const res = await route.fetch();
  const body = (await res.text()).replace('this.array = null;', '');
  await route.fulfill({ response: res, body });
});
// ?shot=1 forces 'high': let an explicit ?quality= win, so the other tiers can be checked with
// the frozen, deterministic shot schedules ('shot=1&quality=low')
await page.route('**/src/quality.js*', async (route) => {
  const res = await route.fetch();
  const body = (await res.text()).replace("params.get('shot') === '1' ? 'high'", "params.get('shot') === '1' && !params.get('quality') ? 'high'");
  await route.fulfill({ response: res, body });
});
await page.goto(`http://localhost:5173/?${query}`);
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
const lines = await page.evaluate((name) => {
  const out = [];
  // '@unnamed': every unnamed top-level object, in scene order (the cars)
  const kids = window.__scene.children, st = kids.findIndex((o) => o.name === 'street');
  // '@beach': what buildBeach adds (after the 22 car objects, up to the ocean)
  const groups = name === '@unnamed' ? kids.filter((o) => o.name === '' && !o.isLight)
    : name === '@beach' ? kids.slice(st + 23, kids.findIndex((o) => o.name === 'ocean'))
      : name === '@skinned' ? kids.filter((o) => o.isSkinnedMesh)
      // '@vehicles': the 5 objects after the last person: bike + blob, ATV + blob, spray points
      : name === '@vehicles' ? kids.slice(kids.findLastIndex((o) => o.isSkinnedMesh) + 1).slice(0, 5)
      : name.startsWith('*') ? kids.filter((o) => o.name === name.slice(1))   // '*NAME': every top-level NAME
      : [kids.find((o) => o.name === name)];
  const sum = (a) => { let s = 0; if (a) for (let i = 0; i < a.length; i++) s += a[i]; return s; };
  // FNV-1a over the 32-bit words (float bits / index values): order- and sign-sensitive
  const fnv = (a) => { let h = 0x811c9dc5; if (a) for (let i = 0; i < a.length; i++) h = Math.imul(h ^ a[i], 16777619) >>> 0; return h; };
  const bits = (a) => a && new Uint32Array(Float32Array.from(a).buffer);
  let depth = 0;
  const walk = (o, path) => {
    if (o.isMesh) {
      const geo = o.geometry, at = geo.attributes;
      const idx = geo.index ? geo.index.count : 0;
      let s = `${path} ${o.isInstancedMesh ? 'inst' : o.isBatchedMesh ? 'batch' : 'mesh'} v=${at.position.count} i=${idx} pos=${sum(at.position.array)} nrm=${sum(at.normal?.array)}`;
      s += ` col=${sum(at.color?.array)} uv=${sum(at.uv?.array)} cast=${+o.castShadow} recv=${+o.receiveShadow} ro=${o.renderOrder}`;
      for (const k of ['aW', 'aE', 'aB', 'aWin']) if (at[k]) s += ` ${k}=${sum(at[k].array)}`;
      for (const k of Object.keys(at)) if (at[k].isInstancedBufferAttribute) s += ` ${k}=${sum(at[k].array)}`;
      if (o.isSkinnedMesh) s += ` bm=${sum(o.skeleton.boneMatrices)}`;
      if (o.isInstancedMesh) s += ` n=${o.count} m=${sum(o.instanceMatrix.array)} ic=${sum(o.instanceColor?.array)}`;
      if (o.isBatchedMesh) s += ` n=${o.instanceCount} m=${sum(o._matricesTexture.image.data)} ic=${sum(o._colorsTexture?.image.data)}`;
      s += ` ph=${fnv(bits(at.position.array))} ih=${fnv(geo.index && geo.index.array)}`;
      out.push(s);
    }
    o.children.forEach((c, i) => walk(c, `${path}/${i}`));
  };
  groups.forEach((g, i) => walk(g, name.startsWith('@') ? `u/${i}` : name));
  out.push(`stats ${JSON.stringify(window.__hotelStats ?? null)} ${JSON.stringify(window.__palmStats ?? null)}`);
  return out;
}, name);
console.log(lines.join('\n'));
await browser.close();
