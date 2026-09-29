// Dev-only: runs the original three.js app in headless Chromium and records the exact GLSL
// of every program it links (after onBeforeCompile patches, defines and chunk expansion),
// so the C port's generated shaders can be diffed against ground truth.
//   (vite dev server on :5173)  node c/tools/ref/capture-shaders.mjs <label> [query]
// e.g. node c/tools/ref/capture-shaders.mjs high 'shot=1'
//      node c/tools/ref/capture-shaders.mjs low 'quality=low&autostart'
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';

const label = process.argv[2] || 'high';
const query = process.argv[3] || 'shot=1';
const outDir = path.resolve('c/tools/ref/out', `shaders-${label}`);
fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(outDir, { recursive: true });

const browser = await chromium.launch({
  headless: true,
  args: ['--mute-audio', '--ignore-gpu-blocklist', '--enable-unsafe-swiftshader', '--use-angle=swiftshader'],
});
const page = await browser.newPage({ viewport: { width: 1024, height: 576 }, deviceScaleFactor: 1 });
const errors = [];
page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
page.on('pageerror', (e) => errors.push(String(e)));

await page.addInitScript(() => {
  const P = WebGL2RenderingContext.prototype;
  const src = new WeakMap(), type = new WeakMap(), attached = new WeakMap();
  const programs = [];
  window.__capturedPrograms = programs;
  const oSource = P.shaderSource, oCreate = P.createShader, oAttach = P.attachShader, oLink = P.linkProgram, oUse = P.useProgram;
  const oDrawE = P.drawElements, oDrawA = P.drawArrays, oDrawEI = P.drawElementsInstanced, oDrawAI = P.drawArraysInstanced;
  P.createShader = function (t) { const s = oCreate.call(this, t); type.set(s, t); return s; };
  P.shaderSource = function (s, text) { src.set(s, text); return oSource.call(this, s, text); };
  P.attachShader = function (p, s) {
    if (!attached.has(p)) attached.set(p, []);
    attached.get(p).push(s);
    return oAttach.call(this, p, s);
  };
  P.linkProgram = function (p) {
    const rec = { vs: '', fs: '', draws: 0 };
    for (const s of attached.get(p) || []) {
      if (type.get(s) === this.VERTEX_SHADER) rec.vs = src.get(s); else rec.fs = src.get(s);
    }
    p.__rec = rec;
    programs.push(rec);
    return oLink.call(this, p);
  };
  let cur = null;
  P.useProgram = function (p) { cur = p; return oUse.call(this, p); };
  const count = function () { if (cur && cur.__rec) cur.__rec.draws++; };
  P.drawElements = function (...a) { count(); return oDrawE.apply(this, a); };
  P.drawArrays = function (...a) { count(); return oDrawA.apply(this, a); };
  P.drawElementsInstanced = function (...a) { count(); return oDrawEI.apply(this, a); };
  P.drawArraysInstanced = function (...a) { count(); return oDrawAI.apply(this, a); };
});

// expose the renderer (main.js keeps it module-private) so programs can be mapped to materials
await page.route('**/src/main.js*', async (route) => {
  const resp = await route.fetch();
  const body = (await resp.text()).replace('document.body.appendChild(renderer.domElement);',
    'document.body.appendChild(renderer.domElement); window.__renderer = renderer;');
  await route.fulfill({ response: resp, body });
});
await page.goto(`http://localhost:5173/?${query}`);
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
await page.waitForTimeout(1000);
const programs = await page.evaluate(() => window.__capturedPrograms);
// who uses which program: object path, material type/name, patch source head
const users = await page.evaluate(() => {
  const r = window.__renderer, out = {};
  const recIndex = new Map(window.__capturedPrograms.map((p, i) => [p, i]));
  const pathOf = (o) => { const a = []; for (let q = o; q; q = q.parent) a.unshift(q.name || q.type); return a.join('/'); };
  window.__scene.traverse((o) => {
    for (const m of [].concat(o.material ?? [], o.customDepthMaterial ?? [])) {
      const prog = r.properties.get(m)?.currentProgram;
      const i = prog ? recIndex.get(prog.program.__rec) : undefined;
      if (i === undefined) continue;
      (out[i] ??= []).push({
        path: pathOf(o), obj: o.type, mat: m.type, name: m.name,
        patch: m.onBeforeCompile && m.onBeforeCompile.toString().includes('replace') ? m.onBeforeCompile.toString().slice(0, 160).replace(/\s+/g, ' ') : '',
      });
    }
  });
  return out;
});
const index = [];
programs.forEach((p, i) => {
  const id = String(i).padStart(3, '0');
  const name = /#define SHADER_NAME (.*)/.exec(p.vs)?.[1]?.trim() ?? 'raw';
  fs.writeFileSync(path.join(outDir, `${id}.vert`), p.vs);
  fs.writeFileSync(path.join(outDir, `${id}.frag`), p.fs);
  const u = users[i] ?? [];
  index.push({ id, name, draws: p.draws, vsLines: p.vs.split('\n').length, fsLines: p.fs.split('\n').length, users: u.length, examples: u.slice(0, 3) });
});
fs.writeFileSync(path.join(outDir, 'index.json'), JSON.stringify({ query, errors, programs: index }, null, 1));
console.log(`${programs.length} programs -> ${outDir}`);
for (const p of index) console.log(p.id, p.name.padEnd(20), 'draws', String(p.draws).padStart(6), 'users', String(p.users).padStart(4), p.examples[0] ? `${p.examples[0].mat} ${p.examples[0].path.slice(-60)} | ${p.examples[0].patch.slice(0, 90)}` : '');
if (errors.length) console.log('ERRORS:\n' + errors.join('\n'));
await browser.close();
