// Dev-only: runs src/sky.js's own shader installers under node (no browser) for each quality
// tier and writes the exact GLSL they produce into the port's shader tree:
//   c/shaders/chunk/[tier/]*.glsl   three chunks that sky.js replaces or extends globally
//   c/shaders/vars/[tier/]*.glsl    SKY_BASE_GLSL / FOG_FN_GLSL / SKY_FULL_GLSL (${...} values)
// Text identical across tiers goes to the shared directory.
//   node c/tools/ref/extract-sky-glsl.mjs
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const repo = path.resolve('.');
const three = path.join(repo, 'node_modules/three/build/three.module.js');
const TIERS = {
  high: { shadowTaps: 5, shadowFilter: 'lite', cloudOctaves: 5, shadowMap: [4096, 1024] },
  medium: { shadowTaps: 5, shadowFilter: 'lite', cloudOctaves: 4, shadowMap: [4096, 1024] },
  low: { shadowTaps: 4, shadowFilter: 'lite', cloudOctaves: 3, shadowMap: [2048, 1024] },
  ultra: { shadowTaps: 8, shadowFilter: 'full', cloudOctaves: 5, shadowMap: [8192, 2048] },
};

const THREE = await import(three);
const pristine = { ...THREE.ShaderChunk };
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'od-sky-'));
const out = {};   // key -> { tier: text }

for (const [tier, Q] of Object.entries(TIERS)) {
  Object.assign(THREE.ShaderChunk, pristine);
  let src = fs.readFileSync(path.join(repo, 'src/sky.js'), 'utf8');
  src = src.replace("import * as THREE from 'three';", `import * as THREE from '${three}';`)
    .replace("from './world/layout.js'", `from '${path.join(repo, 'src/world/layout.js')}'`)
    .replace("import { QUALITY } from './quality.js';", `const QUALITY = ${JSON.stringify(Q)};`);
  src += '\nexport { installAerialPerspective, installGroundBounce, installSmoothShadows, fitShadow, makeSkyMaterial };\n';
  // layout.js imports nothing; three is resolved by absolute path
  const file = path.join(tmp, `sky-${tier}.mjs`);
  fs.writeFileSync(file, src);
  const sky = await import(file);
  sky.installAerialPerspective();
  sky.installGroundBounce();
  // the same shadow-camera setup as createSky()
  const sun = new THREE.DirectionalLight(sky.SUN_COLOR, sky.SUN_INTENSITY);
  sun.shadow.mapSize.set(Q.shadowMap[0], Q.shadowMap[1]);
  const SPAN = 80;
  sky.fitShadow(sun, new THREE.Box3(new THREE.Vector3(-62, -1.5, -SPAN), new THREE.Vector3(96, 30, SPAN)));
  sky.installSmoothShadows(sun.shadow.camera, sun.shadow.mapSize);
  for (const name of ['fog_pars_vertex', 'fog_vertex', 'fog_pars_fragment', 'fog_fragment', 'aomap_fragment', 'shadowmap_pars_fragment']) {
    (out[`chunk/${name}`] ??= {})[tier] = THREE.ShaderChunk[name];
  }
  (out['vars/SKY_BASE_GLSL'] ??= {})[tier] = sky.SKY_BASE_GLSL;
  (out['vars/FOG_FN_GLSL'] ??= {})[tier] = sky.FOG_FN_GLSL;
  (out['vars/SKY_FULL_GLSL'] ??= {})[tier] = sky.SKY_FULL_GLSL;
  // sky materials: templates with the tier-dependent SKY_FULL_GLSL put back as ${SKY_FULL_GLSL}
  for (const mode of [0, 1]) {
    const m = sky.makeSkyMaterial(mode);
    if (!m.fragmentShader.includes(sky.SKY_FULL_GLSL)) throw new Error('sky template: SKY_FULL_GLSL not found');
    (out[`custom/sky${mode}.frag`] ??= {})[tier] = m.fragmentShader.replace(sky.SKY_FULL_GLSL, '${SKY_FULL_GLSL}');
    (out[`custom/sky${mode}.vert`] ??= {})[tier] = m.vertexShader;
  }
  const sc = sun.shadow.camera;
  (out['vars/SHADOW_CAMERA'] ??= {})[tier] = JSON.stringify({ left: sc.left, right: sc.right, top: sc.top, bottom: sc.bottom, near: sc.near, far: sc.far });
}

const root = path.join(repo, 'c/shaders');
for (const [key, byTier] of Object.entries(out)) {
  const texts = Object.values(byTier);
  const [dir, name0] = key.split('/');
  const name = name0.includes('.') ? name0.replace(/\.(\w+)$/, '') : name0;
  const ext = name0.includes('.') ? name0.slice(name0.lastIndexOf('.') + 1) : 'glsl';
  if (texts.every((t) => t === texts[0])) {
    fs.mkdirSync(path.join(root, dir), { recursive: true });
    fs.writeFileSync(path.join(root, dir, `${name}.${ext}`), texts[0]);
    console.log(`${key}: shared`);
  } else {
    for (const [tier, t] of Object.entries(byTier)) {
      fs.mkdirSync(path.join(root, dir, tier), { recursive: true });
      fs.writeFileSync(path.join(root, dir, tier, `${name}.${ext}`), t);
    }
    console.log(`${key}: per tier`);
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
