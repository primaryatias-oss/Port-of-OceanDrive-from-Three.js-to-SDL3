// Dev-only: records the onBeforeCompile patches of every material in the running JS app.
// Each material's onBeforeCompile is run against a recording shader object (the ShaderLib
// template of its type); every String.prototype.replace / replaceAll call on the shader text
// is logged with its exact arguments, whole-source assignments are captured, and the uniforms
// it adds are listed. Materials are grouped by the program they compiled to (capture ids from
// capture-shaders.mjs, matched by the program's vertex+fragment text).
//   node c/tools/ref/extract-patches.mjs <tier> '<query>'
// -> c/tools/ref/out/patches-<tier>.json
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';

const tier = process.argv[2] || 'high';
const query = process.argv[3] || 'shot=1';
const libDir = path.resolve('c/shaders/three/lib');
const libs = {};
for (const f of fs.readdirSync(libDir)) libs[f] = fs.readFileSync(path.join(libDir, f), 'utf8');

const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--ignore-gpu-blocklist', '--enable-gpu', '--use-angle=vulkan', '--enable-features=Vulkan'] });
const page = await browser.newPage({ viewport: { width: 1024, height: 576 } });
const errors = [];
page.on('pageerror', (e) => errors.push(String(e)));
await page.addInitScript(() => {
  const P = WebGL2RenderingContext.prototype;
  const src = new WeakMap(), type = new WeakMap(), attached = new WeakMap();
  const programs = [];
  window.__capturedPrograms = programs;
  const oSource = P.shaderSource, oCreate = P.createShader, oAttach = P.attachShader, oLink = P.linkProgram;
  P.createShader = function (t) { const s = oCreate.call(this, t); type.set(s, t); return s; };
  P.shaderSource = function (s, text) { src.set(s, text); return oSource.call(this, s, text); };
  P.attachShader = function (p, s) { if (!attached.has(p)) attached.set(p, []); attached.get(p).push(s); return oAttach.call(this, p, s); };
  P.linkProgram = function (p) {
    const rec = { vs: '', fs: '' };
    for (const s of attached.get(p) || []) { if (type.get(s) === this.VERTEX_SHADER) rec.vs = src.get(s); else rec.fs = src.get(s); }
    p.__rec = rec;
    programs.push(rec);
    return oLink.call(this, p);
  };
});
await page.route('**/src/main.js*', async (route) => {
  const resp = await route.fetch();
  const body = (await resp.text()).replace('document.body.appendChild(renderer.domElement);',
    'document.body.appendChild(renderer.domElement); window.__renderer = renderer;');
  await route.fulfill({ response: resp, body });
});
await page.goto(`http://localhost:5173/?${query}`);
await page.waitForFunction(() => window.__sceneReady === true, null, { timeout: 600000 });
await page.waitForTimeout(500);

const result = await page.evaluate((libs) => {
  const r = window.__renderer;
  const progIndex = new Map(window.__capturedPrograms.map((p, i) => [p, i]));
  const libOf = { MeshStandardMaterial: 'standard', MeshPhysicalMaterial: 'physical', MeshBasicMaterial: 'basic',
    MeshDepthMaterial: 'depth', MeshLambertMaterial: 'lambert', MeshPhongMaterial: 'phong', PointsMaterial: 'points' };
  const pathOf = (o) => { const a = []; for (let q = o; q; q = q.parent) a.unshift(q.name || q.type); return a.join('/'); };
  const out = [];
  const seen = new Set();
  const record = (m, obj, role) => {
    if (seen.has(m)) return;
    seen.add(m);
    const prog = r.properties.get(m)?.currentProgram;
    const pid = prog ? progIndex.get(prog.program.__rec) : undefined;
    const lib = libOf[m.type];
    const entry = { program: pid, path: pathOf(obj), role, type: m.type, name: m.name, patches: [], sets: {}, uniforms: [], hasPatch: false,
      cacheKey: (m.customProgramCacheKey && !/^customProgramCacheKey\(\)\s*{\s*return this\.onBeforeCompile\.toString\(\)/.test(m.customProgramCacheKey.toString())) ? String(m.customProgramCacheKey()) : null,
      side: m.side, transparent: !!m.transparent, forceSinglePass: !!m.forceSinglePass, alphaTest: m.alphaTest, hasMap: !!m.map,
      materialDefines: m.defines ?? null, glslVersion: m.glslVersion ?? null,
      shader: m.isShaderMaterial ? { vertex: m.vertexShader, fragment: m.fragmentShader } : null,
      isInstanced: !!obj.isInstancedMesh, isSkinned: !!obj.isSkinnedMesh, isBatched: !!obj.isBatchedMesh };
    if (m.onBeforeCompile && m.onBeforeCompile.toString() !== 'onBeforeCompile() {}' && !/^onBeforeCompile\(\s*\/\*/.test(m.onBeforeCompile.toString())) {
      const V = m.isShaderMaterial ? m.vertexShader : libs[lib + '.vert'];
      const F = m.isShaderMaterial ? m.fragmentShader : libs[lib + '.frag'];
      const shader = { vertexShader: V, fragmentShader: F, uniforms: {}, defines: {} };
      const log = [];
      const stageOf = (s) => (s === shader.vertexShader ? 'vertex' : s === shader.fragmentShader ? 'fragment' : null);
      // track the evolving text of each stage through the replace chain
      let vCur = V, fCur = F;
      const oRep = String.prototype.replace, oAll = String.prototype.replaceAll;
      String.prototype.replace = function (a, b) {
        const self = String(this);
        const res = oRep.call(self, a, b);
        const st = self === vCur ? 'vertex' : self === fCur ? 'fragment' : null;
        if (st) {
          log.push({ stage: st, kind: typeof a === 'string' ? 'first' : 'regex', find: typeof a === 'string' ? a : { source: a.source, flags: a.flags }, with: typeof b === 'string' ? b : '<function>' });
          if (st === 'vertex') vCur = res; else fCur = res;
        }
        return res;
      };
      String.prototype.replaceAll = function (a, b) {
        const self = String(this);
        const res = oAll.call(self, a, b);
        const st = self === vCur ? 'vertex' : self === fCur ? 'fragment' : null;
        if (st) {
          log.push({ stage: st, kind: 'all', find: typeof a === 'string' ? a : { source: a.source, flags: a.flags }, with: b });
          if (st === 'vertex') vCur = res; else fCur = res;
        }
        return res;
      };
      try {
        m.onBeforeCompile(shader, r);
      } finally {
        String.prototype.replace = oRep;
        String.prototype.replaceAll = oAll;
      }
      entry.hasPatch = true;
      entry.patches = log;
      // whole-source assignments (not explained by the logged replaces)
      if (shader.vertexShader !== vCur) entry.sets.vertex = shader.vertexShader;
      if (shader.fragmentShader !== fCur) entry.sets.fragment = shader.fragmentShader;
      for (const [k, v] of Object.entries(shader.uniforms)) {
        const val = v?.value;
        entry.uniforms.push({ name: k, kind: val?.isTexture ? 'texture' : val?.isVector2 ? 'vec2' : val?.isVector3 ? 'vec3' : val?.isColor ? 'color' : val?.isMatrix4 ? 'mat4' : Array.isArray(val) ? 'array' : typeof val });
      }
      entry.defines = shader.defines;
    }
    out.push(entry);
  };
  window.__scene.traverse((o) => {
    for (const m of [].concat(o.material ?? [])) record(m, o, 'material');
    if (o.customDepthMaterial) record(o.customDepthMaterial, o, 'customDepth');
  });
  return out;
}, libs);
const outFile = path.resolve('c/tools/ref/out', `patches-${tier}.json`);
fs.writeFileSync(outFile, JSON.stringify({ query, errors, materials: result }, null, 1));
const patched = result.filter((e) => e.hasPatch);
console.log(`${result.length} materials, ${patched.length} patched -> ${outFile}`);
for (const e of patched) console.log(String(e.program).padStart(4), e.type.padEnd(22), e.path.slice(-50).padEnd(50), `${e.patches.length} replaces`, Object.keys(e.sets).join(',') ? `sets:${Object.keys(e.sets).join(',')}` : '');
await browser.close();
