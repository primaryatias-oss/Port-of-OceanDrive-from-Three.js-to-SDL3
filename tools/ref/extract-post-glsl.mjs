// Dev-only: writes the post chain's shader templates to c/shaders/custom/ exactly as three.js
// and src/renderer/post.js define them (UnrealBloomPass materials, CopyShader, OutputShader,
// FXAAShader, post.js GradeShader / FinishShader).
//   node c/tools/ref/extract-post-glsl.mjs
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const repo = path.resolve('.');
const three = path.join(repo, 'node_modules/three/build/three.module.js');
const out = path.join(repo, 'c/shaders/custom');
fs.mkdirSync(out, { recursive: true });
const put = (name, text) => fs.writeFileSync(path.join(out, name), text);

// a copy of examples/jsm where 'three' resolves to the build file (node has no import map)
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'od-post-'));
const jsm = path.join(tmp, 'jsm');
fs.cpSync(path.join(repo, 'node_modules/three/examples/jsm'), jsm, { recursive: true });
const fix = (f) => fs.writeFileSync(f, fs.readFileSync(f, 'utf8').replace(/from 'three'/g, `from '${three}'`));
for (const d of ['postprocessing', 'shaders']) for (const f of fs.readdirSync(path.join(jsm, d))) if (f.endsWith('.js')) fix(path.join(jsm, d, f));

const { CopyShader } = await import(path.join(jsm, 'shaders/CopyShader.js'));
const { LuminosityHighPassShader } = await import(path.join(jsm, 'shaders/LuminosityHighPassShader.js'));
const { OutputShader } = await import(path.join(jsm, 'shaders/OutputShader.js'));
const { FXAAShader } = await import(path.join(jsm, 'shaders/FXAAShader.js'));
const { UnrealBloomPass } = await import(path.join(jsm, 'postprocessing/UnrealBloomPass.js'));
const THREE = await import(three);

put('copy.vert', CopyShader.vertexShader);
put('copy.frag', CopyShader.fragmentShader);
put('luminosity_high_pass.vert', LuminosityHighPassShader.vertexShader);
put('luminosity_high_pass.frag', LuminosityHighPassShader.fragmentShader);
put('output.vert', OutputShader.vertexShader);
put('output.frag', OutputShader.fragmentShader);
put('fxaa.vert', FXAAShader.vertexShader);
put('fxaa.frag', FXAAShader.fragmentShader);
const bloom = new UnrealBloomPass(new THREE.Vector2(1024, 576), 0.16, 0.25, 4.0);
put('bloom_blur.vert', bloom.separableBlurMaterials[0].vertexShader);
put('bloom_blur.frag', bloom.separableBlurMaterials[0].fragmentShader);
const blur = bloom.separableBlurMaterials.map((m) => ({
  pairs: m.defines.KERNEL_PAIRS, center: m.uniforms.centerWeight.value,
  offsets: m.uniforms.gaussianOffsets.value, weights: m.uniforms.gaussianWeights.value,
}));
put('bloom_composite.vert', bloom.compositeMaterial.vertexShader);
put('bloom_composite.frag', bloom.compositeMaterial.fragmentShader);
fs.writeFileSync(path.join(repo, 'c/tools/ref/out/bloom-kernels.json'), JSON.stringify(blur, null, 1));

// post.js: GradeShader / FinishShader (module-private); sky.js is stubbed with sunDir
let post = fs.readFileSync(path.join(repo, 'src/renderer/post.js'), 'utf8')
  .replace("import * as THREE from 'three';", `import * as THREE from '${three}';`)
  .replace(/from 'three\/addons\/([\w/]+)\.js'/g, (m, n) => `from '${path.join(jsm, n + '.js')}'`)
  .replace("import { sunDir } from '../sky.js';", 'const sunDir = new THREE.Vector3();');
fs.writeFileSync(path.join(tmp, 'post.js'), post + '\nexport { GradeShader, FinishShader };\n');
const P = await import(path.join(tmp, 'post.js'));
put('grade.vert', P.GradeShader.vertexShader);
put('grade.frag', P.GradeShader.fragmentShader);
put('finish.vert', P.FinishShader.vertexShader);
put('finish.frag', P.FinishShader.fragmentShader);
fs.rmSync(tmp, { recursive: true, force: true });
console.log('post shaders written; bloom kernel pairs:', blur.map((b) => b.pairs).join(','));
