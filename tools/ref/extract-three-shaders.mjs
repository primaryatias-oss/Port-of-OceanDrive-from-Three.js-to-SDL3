// Dev-only, run once: copies three.js's GLSL shader chunks and ShaderLib templates (MIT) into
// c/shaders/three/ so the C build assembles programs from committed sources.
//   node c/tools/ref/extract-three-shaders.mjs
import fs from 'node:fs';
import path from 'node:path';
import { ShaderChunk, ShaderLib, REVISION } from '../../../node_modules/three/build/three.module.js';

const root = path.resolve('c/shaders/three');
for (const [name, src] of Object.entries(ShaderChunk)) {
  if (typeof src !== 'string') continue;
  fs.writeFileSync(path.join(root, 'chunk', `${name}.glsl`), src);
}
for (const [name, lib] of Object.entries(ShaderLib)) {
  fs.writeFileSync(path.join(root, 'lib', `${name}.vert`), lib.vertexShader);
  fs.writeFileSync(path.join(root, 'lib', `${name}.frag`), lib.fragmentShader);
}
fs.writeFileSync(path.join(root, 'REVISION'), `three.js r${REVISION}\n`);
console.log(`three r${REVISION}: ${Object.keys(ShaderChunk).length} chunks, ${Object.keys(ShaderLib).length} libs`);
