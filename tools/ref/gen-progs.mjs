// Dev-only: writes c/shaders/programs/*.prog for the scene's material programs from what the
// JS app itself produced:
//   - material parameters: reverse-mapped from each captured program's prefix #defines
//   - patches: the onBeforeCompile replace calls recorded by extract-patches.mjs
//   - templates: three's ShaderLib (by material type), or the ShaderMaterial's / patched
//     whole-source text (written to c/shaders/custom/)
// Output is verified afterwards by tools/ref/verify-progs.sh (assembled text == capture).
//   node c/tools/ref/gen-progs.mjs
import fs from 'node:fs';
import path from 'node:path';

const TIERS = ['high', 'medium', 'low', 'ultra'];
const ref = path.resolve('c/tools/ref/out');
const progDir = path.resolve('c/shaders/programs');
const customDir = path.resolve('c/shaders/custom');
const HAND = new Set(['sky', 'sky_env', 'pmrem_background', 'pmrem_ggx']);   // hand-written .prog files

const patches = {}, caps = {};
for (const t of TIERS) {
  patches[t] = JSON.parse(fs.readFileSync(path.join(ref, `patches-${t}.json`), 'utf8')).materials;
  const dir = path.join(ref, `shaders-${t}`);
  const idx = JSON.parse(fs.readFileSync(path.join(dir, 'index.json'), 'utf8')).programs;
  caps[t] = idx.map((p) => ({ id: p.id, vs: fs.readFileSync(path.join(dir, `${p.id}.vert`), 'utf8'), fs: fs.readFileSync(path.join(dir, `${p.id}.frag`), 'utf8') }));
}
for (const t of TIERS) if (patches[t].length !== patches.high.length) throw new Error(`material count differs in ${t}`);
// materials are matched across tiers by cache key, else by (path, type, role, n-th occurrence)
const keyOf = (list) => {
  const seen = new Map();
  return list.map((e) => {
    const base = e.cacheKey ? `k:${e.cacheKey}` : `p:${e.path}|${e.type}|${e.role}|${e.name}`;
    const n = (seen.get(base) ?? 0) + 1;
    seen.set(base, n);
    return `${base}#${n}`;
  });
};
const keys = Object.fromEntries(TIERS.map((t) => [t, keyOf(patches[t])]));
const indexIn = Object.fromEntries(TIERS.map((t) => [t, new Map(keys[t].map((k, i) => [k, i]))]));

// ---- prefix -> parameters -------------------------------------------------------------------
const UV = { MAP_UV: 'mapUv', ALPHAMAP_UV: 'alphaMapUv', LIGHTMAP_UV: 'lightMapUv', AOMAP_UV: 'aoMapUv', EMISSIVEMAP_UV: 'emissiveMapUv',
  BUMPMAP_UV: 'bumpMapUv', NORMALMAP_UV: 'normalMapUv', DISPLACEMENTMAP_UV: 'displacementMapUv', METALNESSMAP_UV: 'metalnessMapUv',
  ROUGHNESSMAP_UV: 'roughnessMapUv', ANISOTROPYMAP_UV: 'anisotropyMapUv', CLEARCOATMAP_UV: 'clearcoatMapUv',
  CLEARCOAT_NORMALMAP_UV: 'clearcoatNormalMapUv', CLEARCOAT_ROUGHNESSMAP_UV: 'clearcoatRoughnessMapUv',
  IRIDESCENCEMAP_UV: 'iridescenceMapUv', IRIDESCENCE_THICKNESSMAP_UV: 'iridescenceThicknessMapUv', SHEEN_COLORMAP_UV: 'sheenColorMapUv',
  SHEEN_ROUGHNESSMAP_UV: 'sheenRoughnessMapUv', SPECULARMAP_UV: 'specularMapUv', SPECULAR_COLORMAP_UV: 'specularColorMapUv',
  SPECULAR_INTENSITYMAP_UV: 'specularIntensityMapUv', TRANSMISSIONMAP_UV: 'transmissionMapUv', THICKNESSMAP_UV: 'thicknessMapUv' };
const FLAG = { USE_CLIP_DISTANCE: 'extensionClipCullDistance', USE_BATCHING: 'batching', USE_BATCHING_COLOR: 'batchingColor',
  USE_INSTANCING: 'instancing', USE_INSTANCING_COLOR: 'instancingColor', USE_INSTANCING_MORPH: 'instancingMorph',
  FOG_EXP2: 'fogExp2', USE_MAP: 'map', USE_ENVMAP: 'envMap', USE_LIGHTMAP: 'lightMap', USE_AOMAP: 'aoMap', USE_BUMPMAP: 'bumpMap',
  USE_NORMALMAP: 'normalMap', USE_NORMALMAP_OBJECTSPACE: 'normalMapObjectSpace', USE_NORMALMAP_TANGENTSPACE: 'normalMapTangentSpace',
  USE_DISPLACEMENTMAP: 'displacementMap', USE_EMISSIVEMAP: 'emissiveMap', USE_ANISOTROPY: 'anisotropy', USE_ANISOTROPYMAP: 'anisotropyMap',
  USE_CLEARCOATMAP: 'clearcoatMap', USE_CLEARCOAT_ROUGHNESSMAP: 'clearcoatRoughnessMap', USE_CLEARCOAT_NORMALMAP: 'clearcoatNormalMap',
  USE_IRIDESCENCEMAP: 'iridescenceMap', USE_IRIDESCENCE_THICKNESSMAP: 'iridescenceThicknessMap', USE_SPECULARMAP: 'specularMap',
  USE_SPECULAR_COLORMAP: 'specularColorMap', USE_SPECULAR_INTENSITYMAP: 'specularIntensityMap', USE_ROUGHNESSMAP: 'roughnessMap',
  USE_METALNESSMAP: 'metalnessMap', USE_ALPHAMAP: 'alphaMap', USE_ALPHAHASH: 'alphaHash', USE_TRANSMISSION: 'transmission',
  USE_TRANSMISSIONMAP: 'transmissionMap', USE_THICKNESSMAP: 'thicknessMap', USE_SHEEN_COLORMAP: 'sheenColorMap',
  USE_SHEEN_ROUGHNESSMAP: 'sheenRoughnessMap', USE_TANGENT: 'vertexTangents', HAS_NORMAL: 'vertexNormals', USE_COLOR: 'vertexColors',
  USE_COLOR_ALPHA: 'vertexAlphas', USE_UV1: 'vertexUv1s', USE_UV2: 'vertexUv2s', USE_UV3: 'vertexUv3s', USE_POINTS_UV: 'pointsUvs',
  FLAT_SHADED: 'flatShading', USE_SKINNING: 'skinning', USE_MORPHTARGETS: 'morphTargets', USE_MORPHNORMALS: 'morphNormals',
  USE_MORPHCOLORS: 'morphColors', DOUBLE_SIDED: 'doubleSided', FLIP_SIDED: 'flipSided', USE_SHADOWMAP: 'shadowMapEnabled',
  USE_SIZEATTENUATION: 'sizeAttenuation', USE_LIGHT_PROBES: 'numLightProbes', USE_LOGARITHMIC_DEPTH_BUFFER: 'logarithmicDepthBuffer',
  USE_REVERSED_DEPTH_BUFFER: 'reversedDepthBuffer',
  // fragment-only
  ALPHA_TO_COVERAGE: 'alphaToCoverage', USE_MATCAP: 'matcap', USE_PACKED_NORMALMAP: 'packedNormalMap', USE_CLEARCOAT: 'clearcoat',
  USE_DISPERSION: 'dispersion', USE_RETROREFLECTION: 'retroreflection', USE_IRIDESCENCE: 'iridescence', USE_ALPHATEST: 'alphaTest',
  USE_SHEEN: 'sheen', USE_GRADIENTMAP: 'gradientMap', PREMULTIPLIED_ALPHA: 'premultipliedAlpha', USE_LIGHT_PROBES_GRID: 'numLightProbeGrids',
  DECODE_VIDEO_TEXTURE: 'decodeVideoTexture', DECODE_VIDEO_TEXTURE_EMISSIVE: 'decodeVideoTextureEmissive', DITHERING: 'dithering',
  OPAQUE: 'opaque', TONE_MAPPING: null };
const PREFIX_KNOWN = (name) => name in FLAG || name in UV || name === 'USE_FOG' || /^(ENVMAP_(MODE|TYPE|BLENDING)_|SHADOWMAP_TYPE_|CUBEUV_|DEPTH_PACKING$)/.test(name);

function prefixLines(src, endMarker) {
  const lines = src.split('\n');
  const a = lines.findIndex((l) => l.startsWith('#define SHADER_NAME'));
  const b = lines.findIndex((l, i) => i > a && l === endMarker);
  return { a, lines: lines.slice(a + 1, b), shaderType: lines[a - 1].replace('#define SHADER_TYPE ', ''), shaderName: lines[a].replace(/^#define SHADER_NAME ?/, '') };
}

function paramsOf(cap) {
  const params = new Map(), defines = [];
  const v = prefixLines(cap.vs, 'uniform mat4 modelMatrix;');
  // (OPAQUE, DITHERING, TONE_MAPPING, DEPTH_PACKING follow the fragment's uniform lines)
  const f = prefixLines(cap.fs, 'float luminance( const in vec3 rgb ) {');
  // DEPTH_PACKING is the last prefix line, after the luminance function
  const dp = /\n}\n#define DEPTH_PACKING (\d+)\n/.exec(cap.fs);
  if (dp) f.lines.push(`#define DEPTH_PACKING ${dp[1]}`);
  params.set('shaderType', v.shaderType);
  params.set('shaderName', v.shaderName);
  let customDone = false;
  for (const [stage, lines] of [['v', v.lines], ['f', f.lines]]) {
    customDone = false;
    for (const l of lines) {
      const m = /^#define (\w+)(?: (.*))?$/.exec(l);
      if (!m) { customDone = true; continue; }
      if (stage === 'f' && !(m[1] in FLAG || m[1] in UV || PREFIX_KNOWN(m[1])) && customDone) continue;   // chunk-internal defines
      const [, name, val = ''] = m;
      if (!customDone && !PREFIX_KNOWN(name)) {
        if (stage === 'v') defines.push([name, val]);
        continue;
      }
      customDone = true;
      if (name === 'USE_FOG') { params.set('useFog', '1'); params.set('fog', '1'); }
      else if (name in UV) params.set(UV[name], val);
      else if (/^ENVMAP_MODE_/.test(name)) params.set('envMapModeDefine', name);
      else if (/^ENVMAP_TYPE_/.test(name)) params.set('envMapTypeDefine', name);
      else if (/^ENVMAP_BLENDING_/.test(name)) params.set('envMapBlendingDefine', name);
      else if (/^SHADOWMAP_TYPE_/.test(name)) params.set('shadowMapTypeDefine', name);
      else if (name === 'CUBEUV_TEXEL_WIDTH') { params.set('envMapCubeUVHeight', '1024'); params.set('cubeUVTexelWidth', val); }
      else if (name === 'CUBEUV_TEXEL_HEIGHT') params.set('cubeUVTexelHeight', val);
      else if (name === 'CUBEUV_MAX_MIP') params.set('cubeUVMaxMip', val.replace(/\.0$/, ''));
      else if (name === 'DEPTH_PACKING') { params.set('useDepthPacking', '1'); params.set('depthPacking', val); }
      else if (name === 'TONE_MAPPING') {
        const tm = /vec3 toneMapping\( vec3 color \) \{ return (\w+)ToneMapping\( color \); \}/.exec(cap.fs);
        params.set('toneMapping', tm[1]);
      } else if (name === 'USE_COLOR' && stage === 'f') {
        // the fragment USE_COLOR also comes from instancingColor
        if (!params.has('vertexColors') && !params.has('instancingColor')) params.set('vertexColors', '1');
      } else if (name === 'USE_COLOR_ALPHA' && stage === 'f') {
        if (!params.has('vertexAlphas') && !params.has('batchingColor')) params.set('vertexAlphas', '1');
      } else if (FLAG[name]) params.set(FLAG[name], '1');
    }
  }
  if (/linearToOutputTexel[\s\S]{0,120}sRGBTransferOETF/.test(cap.fs)) params.set('outputColorSpace', 'srgb');
  if (cap.vs.includes('#extension GL_ANGLE_multi_draw : require')) params.set('extensionMultiDraw', '1');
  // light counts after replaceLightNums
  const dl = /uniform DirectionalLight directionalLights\[ (\d+) \]/.exec(cap.fs) ?? /uniform DirectionalLight directionalLights\[ (\d+) \]/.exec(cap.vs);
  if (dl && dl[1] !== '0') params.set('numDirLights', dl[1]);
  const ds = /directionalShadowMatrix\[ (\d+) \]/.exec(cap.vs);
  if (ds && ds[1] !== '0') params.set('numDirLightShadows', ds[1]);
  return { params, defines };
}

const LIB = { MeshStandardMaterial: 'standard', MeshPhysicalMaterial: 'physical', MeshBasicMaterial: 'basic', MeshDepthMaterial: 'depth',
  MeshLambertMaterial: 'lambert', MeshPhongMaterial: 'phong', PointsMaterial: 'points' };

// ---- program groups (one .prog per high-tier program used by scene materials) ---------------
const groups = new Map();   // high program id -> { mats: [index...] }
patches.high.forEach((e, i) => {
  if (!groups.has(e.program)) groups.set(e.program, { pid: e.program, mats: [] });
  groups.get(e.program).mats.push(i);
});

function baseName(e, cap) {
  if (e.cacheKey) return e.cacheKey.replace(/-v\d+$/, '').replace(/[^a-zA-Z0-9]+/g, '_').toLowerCase();
  if (e.type === 'ShaderMaterial') return (e.name || 'shader').toLowerCase().replace(/[^a-z0-9]+/g, '_');
  const { params } = paramsOf(cap);
  const has = (k) => params.has(k);
  const parts = [{ MeshStandardMaterial: 'std', MeshPhysicalMaterial: 'phys', MeshBasicMaterial: 'basic', MeshDepthMaterial: 'depth', PointsMaterial: 'points' }[e.type] ?? 'mat'];
  if (has('map')) parts.push('map');
  if (has('vertexColors')) parts.push('vcol');
  if (has('instancing')) parts.push('inst');
  if (has('instancingColor')) parts.push('icol');
  if (has('skinning')) parts.push('skin');
  if (has('alphaTest')) parts.push('atest');
  if (has('alphaToCoverage')) parts.push('a2c');
  if (has('doubleSided')) parts.push('dbl');
  if (has('flipSided')) parts.push('back');
  if (has('emissiveMap')) parts.push('emap');
  if (has('clearcoat')) parts.push('cc');
  if (has('sheen')) parts.push('sheen');
  if (!has('opaque')) parts.push('transp');
  if (!has('useFog')) parts.push('nofog');
  return parts.join('_');
}

const used = new Set(HAND);
const written = [];
function unique(n) { let k = n, i = 2; while (used.has(k)) k = `${n}_${i++}`; used.add(k); return k; }

// Emits the patches; a replace whose search string is not in the text at that point was a no-op
// in the JS (String.replace leaves the text unchanged) and is kept only as a comment.
function patchBlock(list, templates) {
  let t = '';
  const cur = { ...templates };
  for (const p of list) {
    if (p.kind !== 'first') throw new Error('unsupported patch kind ' + p.kind);
    if (!cur[p.stage].includes(p.find)) {
      t += `# no-op in the JS (not in the ${p.stage} text at this point): .replace(${JSON.stringify(p.find)}, ...)\n`;
      continue;
    }
    const i = cur[p.stage].indexOf(p.find);
    cur[p.stage] = cur[p.stage].slice(0, i) + p.with + cur[p.stage].slice(i + p.find.length);
    t += `@replace ${p.stage} ${p.find}\n${p.with}\n@end\n`;
  }
  return t;
}

function sectionByTier(fn) {
  // fn(tier) -> text; emits it once if all tiers agree, else @if blocks per distinct text
  const texts = TIERS.map(fn);
  if (texts.every((x) => x === texts[0])) return texts[0];
  const byText = new Map();
  TIERS.forEach((t, i) => { if (!byText.has(texts[i])) byText.set(texts[i], []); byText.get(texts[i]).push(t); });
  let out = '';
  for (const [text, tiers] of byText) out += `@if ${tiers.join(' ')}\n${text}@endif\n`;
  return out;
}

for (const g of groups.values()) {
  const i0 = g.mats[0];
  const e0 = patches.high[i0];
  const capHigh = caps.high[g.pid];
  if (capHigh && ['Sky', 'SkyEnv'].includes(e0.name)) continue;   // hand-written
  const name = unique(baseName(e0, capHigh));
  // per tier: the program id and material entry
  const per = Object.fromEntries(TIERS.map((t) => {
    const j = indexIn[t].get(keys.high[i0]);
    if (j === undefined) throw new Error(`material ${keys.high[i0]} missing in ${t}`);
    return [t, { e: patches[t][j], cap: caps[t][patches[t][j].program] }];
  }));
  for (const t of TIERS) if (!per[t].cap) throw new Error(`${name}: no capture in ${t}`);
  // template
  let tpl;
  // custom template for a stage: shared when all tiers agree, else one copy per tier
  const custom = (stage, pick) => {
    const file = `${name}.${stage === 'vertex' ? 'vert' : 'frag'}`;
    const texts = TIERS.map((t) => pick(per[t].e));
    if (texts.every((x) => x === texts[0])) fs.writeFileSync(path.join(customDir, file), texts[0]);
    else TIERS.forEach((t, k) => { fs.mkdirSync(path.join(customDir, t), { recursive: true }); fs.writeFileSync(path.join(customDir, t, file), texts[k]); });
    return `custom/${file}`;
  };
  const e = per.high.e;
  if (e.type === 'ShaderMaterial' || e.type === 'RawShaderMaterial') {
    tpl = `vertex ${custom('vertex', (x) => x.sets.vertex ?? x.shader.vertex)}\nfragment ${custom('fragment', (x) => x.sets.fragment ?? x.shader.fragment)}\n`;
  } else if (e.sets.vertex || e.sets.fragment) {
    const lib = LIB[e.type];
    const vt = e.sets.vertex ? custom('vertex', (x) => x.sets.vertex) : `three/lib/${lib}.vert`;
    const ft = e.sets.fragment ? custom('fragment', (x) => x.sets.fragment) : `three/lib/${lib}.frag`;
    tpl = `vertex ${vt.startsWith('three/') ? '../' + vt : vt}\nfragment ${ft.startsWith('three/') ? '../' + ft : ft}\n`;
    tpl = tpl.replace(/\.\.\/three\//g, 'three/');
  } else {
    tpl = `lib ${LIB[e.type]}\n`;
  }
  const paramsText = sectionByTier((t) => {
    const { params, defines } = paramsOf(per[t].cap);
    let s = '';
    for (const [k, v] of defines) s += `define ${k}${v !== '' ? ' ' + v : ''}\n`;
    for (const [k, v] of params) s += v === '1' && !['shaderType', 'shaderName'].includes(k) ? `param ${k}\n` : `param ${k}${v !== '' ? ' ' + v : ''}\n`;
    return s;
  });
  const hasSets = !!(e.sets.vertex || e.sets.fragment);
  const libText = (st) => fs.readFileSync(path.resolve('c/shaders/three/lib', `${LIB[e.type]}.${st}`), 'utf8');
  const tplText = (x) => (x.type === 'ShaderMaterial' || x.type === 'RawShaderMaterial')
    ? { vertex: x.shader.vertex, fragment: x.shader.fragment } : { vertex: libText('vert'), fragment: libText('frag') };
  const patchText = hasSets ? '' : sectionByTier((t) => patchBlock(per[t].e.patches, tplText(per[t].e)));
  const users = g.mats.map((i) => patches.high[i].path).filter((p, k, a) => a.indexOf(p) === k).slice(0, 4);
  const head = `# ${e.type}${e.cacheKey ? ` (${e.cacheKey})` : ''} - ${g.mats.length} material(s): ${users.join(', ')}\n` +
    `# generated by tools/ref/gen-progs.mjs from the JS app's own program + onBeforeCompile patches\n`;
  const text = `${head}name ${name}\n${tpl}${paramsText}${patchText}`;
  fs.writeFileSync(path.join(progDir, `${name}.prog`), text);
  written.push({ name, pid: g.pid, mats: g.mats.length });
  // a transparent DoubleSide material is drawn as a BackSide pass, then a FrontSide pass
  // (WebGLRenderer.renderObject); the recorded program is the front one
  if (e.transparent && e.side === 2 && !e.forceSinglePass) {
    const back = unique(`${name}_back`);
    const btext = text.replace(`name ${name}\n`, `name ${back}\n`).replace(/^(# .*\n)(# generated.*\n)/, '$1# (BackSide pass)\n$2')
      .replace(/(name .*\n(?:vertex .*\n|fragment .*\n|lib .*\n)+)/, '$1param flipSided\n');
    fs.writeFileSync(path.join(progDir, `${back}.prog`), btext);
    written.push({ name: back, pid: null, mats: g.mats.length });
  }
}

// three's internal programs that no scene material records (the shadow-map depth variants):
// parameters straight from the prefix, ShaderLib template by SHADER_TYPE
const covered = new Set([...groups.keys()].map(String));
const SKIP = new Set(['000', '001', '002', '003', '062']);   // sky / PMREM (hand-written), loader warm-up probe
for (const cap of caps.high) {
  if (covered.has(String(+cap.id)) || SKIP.has(cap.id)) continue;
  const { params } = paramsOf(cap);
  const type = params.get('shaderType');
  if (type !== 'MeshDepthMaterial') continue;
  const has = (k) => params.has(k);
  const parts = ['depth'];
  if (has('batching')) parts.push('batch');
  if (has('instancing')) parts.push('inst');
  if (has('instancingColor')) parts.push('icol');
  if (has('map')) parts.push('map');
  if (has('alphaTest')) parts.push('atest');
  if (has('doubleSided')) parts.push('dbl');
  if (has('flipSided')) parts.push('back');
  if (!has('flipSided') && !has('doubleSided')) parts.push('front');
  if (!has('useDepthPacking')) parts.push('nopack');
  const name = unique(parts.join('_'));
  let t = `# MeshDepthMaterial (WebGLShadowMap caster variant, capture ${cap.id})\n# generated by tools/ref/gen-progs.mjs from the JS app's program prefix\nname ${name}\nlib depth\n`;
  for (const [k, v] of params) t += v === '1' && !['shaderType', 'shaderName'].includes(k) ? `param ${k}\n` : `param ${k}${v !== '' ? ' ' + v : ''}\n`;
  fs.writeFileSync(path.join(progDir, `${name}.prog`), t);
  written.push({ name, pid: +cap.id, mats: 0 });
}
fs.writeFileSync(path.join(ref, 'prog-names.json'), JSON.stringify(written, null, 1));
console.log(`${written.length} programs written:`, written.map((w) => `${w.pid}:${w.name}`).join(' '));
