// Dev-only: writes three.js PMREMGenerator's shader templates (file-local functions in
// extras/PMREMGenerator.js) to c/shaders/custom/ exactly as three builds them.
//   node c/tools/ref/extract-pmrem-glsl.mjs
import fs from 'node:fs';
import path from 'node:path';
const dir = path.resolve('node_modules/three/src/extras');
const src = fs.readFileSync(path.join(dir, 'PMREMGenerator.js'), 'utf8')
  + '\nexport { _getGGXShader, _getCommonVertexShader };\n';
const tmp = path.join(dir, '__od_pmrem_extract.mjs');
fs.writeFileSync(tmp, src);
try {
  const m = await import(tmp);
  const ggx = m._getGGXShader(8, 768, 1024);
  const out = path.resolve('c/shaders/custom');
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'pmrem_common.vert'), m._getCommonVertexShader());
  fs.writeFileSync(path.join(out, 'pmrem_ggx.frag'), ggx.fragmentShader);
  console.log('defines', JSON.stringify(ggx.defines));
} finally {
  fs.rmSync(tmp);
}
