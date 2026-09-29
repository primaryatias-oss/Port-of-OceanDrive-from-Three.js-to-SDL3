// Dev-only: dumps reference results from the real three.js for tests/test_geom.c.
//   node tools/ref/geom-ref.mjs > tests/data/geom-ref.txt
// Format: "case NAME", then "attr NAME SIZE COUNT" / "index COUNT" / "nums COUNT" lines, each
// followed by one line of numbers, then "end".
import * as THREE from '../../../node_modules/three/build/three.module.js';
import { RoundedBoxGeometry } from '../../../node_modules/three/examples/jsm/geometries/RoundedBoxGeometry.js';
import { mergeGeometries, mergeVertices } from '../../../node_modules/three/examples/jsm/utils/BufferGeometryUtils.js';

const out = [];
const num = (v) => (Object.is(v, -0) ? '0' : String(v));
function geo(name, g) {
  out.push(`case ${name}`);
  for (const [k, a] of Object.entries(g.attributes)) {
    out.push(`attr ${k} ${a.itemSize} ${a.count}`);
    out.push(Array.from(a.array, num).join(' '));
  }
  if (g.index) {
    out.push(`index ${g.index.count}`);
    out.push(Array.from(g.index.array, num).join(' '));
  }
  out.push('end');
}
function nums(name, arr) {
  out.push(`case ${name}`);
  out.push(`nums ${arr.length}`);
  out.push(arr.map(num).join(' '));
  out.push('end');
}

// --- primitives
geo('box', new THREE.BoxGeometry(1.3, 0.7, 2.1, 2, 3, 1));
geo('plane', new THREE.PlaneGeometry(3, 2, 4, 3));
geo('cylinder', new THREE.CylinderGeometry(0.2, 0.5, 1.5, 12, 2));
geo('cylinder_open_arc', new THREE.CylinderGeometry(0.3, 0.3, 1, 7, 1, true, 0.4, 2.5));
geo('cone', new THREE.ConeGeometry(0.4, 1.2, 9));
geo('sphere', new THREE.SphereGeometry(1.5, 16, 9));
geo('sphere_part', new THREE.SphereGeometry(1, 10, 6, 0.3, 4, 0.2, 1.3));
geo('torus', new THREE.TorusGeometry(1, 0.25, 8, 20));
geo('torus_arc', new THREE.TorusGeometry(0.8, 0.1, 6, 12, 3.5));
geo('circle', new THREE.CircleGeometry(0.7, 11));
geo('lathe', new THREE.LatheGeometry([[0, -0.5], [0.3, -0.4], [0.5, 0], [0.2, 0.4], [0, 0.5]].map(([a, b]) => new THREE.Vector2(a, b)), 14));
geo('capsule', new THREE.CapsuleGeometry(0.3, 1.1, 4, 10, 2));
geo('ico0', new THREE.IcosahedronGeometry(0.5, 0));
geo('ico2', new THREE.IcosahedronGeometry(0.4, 2));
geo('rbox', new RoundedBoxGeometry(1.62, 0.18, 0.56, 3, 0.06));
geo('rbox2', new RoundedBoxGeometry(0.1, 0.36, 1.9, 2, 0.04));

// --- operations
geo('box_ops', new THREE.BoxGeometry(1, 2, 3).rotateX(0.3).rotateY(-1.1).rotateZ(2.2).translate(1, 2, 3).scale(2, 0.5, 1.5));
geo('box_nonidx', new THREE.BoxGeometry(1, 1, 1, 2, 1, 1).toNonIndexed());
{
  const g = new THREE.SphereGeometry(1, 8, 6);
  g.computeVertexNormals();
  geo('sphere_vnormals', g);
  const n = new THREE.SphereGeometry(1, 8, 6).toNonIndexed();
  n.computeVertexNormals();
  geo('sphere_flat', n);
}
geo('merge', mergeGeometries([new THREE.BoxGeometry(1, 1, 1), new THREE.CylinderGeometry(0.2, 0.2, 1, 6).translate(2, 0, 0), new THREE.SphereGeometry(0.5, 6, 4).translate(0, 3, 0)]));
geo('mergeverts', mergeVertices(new THREE.IcosahedronGeometry(0.4, 2).deleteAttribute('normal').deleteAttribute('uv')));
geo('applym4', new THREE.BoxGeometry(1, 1, 1).applyMatrix4(new THREE.Matrix4().compose(new THREE.Vector3(1, -2, 0.5), new THREE.Quaternion().setFromEuler(new THREE.Euler(0.3, 1.2, -0.7, 'YXZ')), new THREE.Vector3(1, -2, 0.5))));

// --- curves / tubes
const pts = [[0, 0, 0], [0.3, 0.5, 0.1], [0.8, 0.6, -0.2], [1.2, 0.2, 0.3], [1.5, -0.1, 0.2]].map((p) => new THREE.Vector3(...p));
geo('tube_centripetal', new THREE.TubeGeometry(new THREE.CatmullRomCurve3(pts), 16, 0.05, 6));
geo('tube_closed', new THREE.TubeGeometry(new THREE.CatmullRomCurve3(pts, true, 'centripetal'), 20, 0.03, 5, true));
geo('tube_catmull', new THREE.TubeGeometry(new THREE.CatmullRomCurve3(pts, false, 'catmullrom', 0.3), 10, 0.1, 4));
{
  const c = new THREE.CatmullRomCurve3(pts);
  nums('curve_points', c.getPoints(12).flatMap((p) => [p.x, p.y, p.z]).concat([c.getLength()]));
}

// --- shapes
{
  const shape = new THREE.Shape([new THREE.Vector2(-1, 0), new THREE.Vector2(3, 0), new THREE.Vector2(3, 2.5), new THREE.Vector2(-1, 2.5)]);
  const h1 = new THREE.Path();
  h1.absarc(0.2, 1.2, 0.4, 0, Math.PI * 2, true);
  shape.holes.push(h1);
  const h2 = new THREE.Path();
  h2.moveTo(1.2, 0.4); h2.lineTo(1.2, 2.0); h2.lineTo(2.4, 2.0); h2.lineTo(2.4, 0.4);
  shape.holes.push(h2);
  geo('shape_holes', new THREE.ShapeGeometry(shape, 16));
  geo('shape_holes_ni', new THREE.ShapeGeometry(shape, 16).toNonIndexed());
}
{
  const sh = new THREE.Shape(), rc = 0.118;
  for (let i = 0; i <= 14; i++) { const a = Math.PI / 2 + (i / 14) * Math.PI; const x = rc * Math.cos(a), y = rc * Math.sin(a); if (i) sh.lineTo(x, y); else sh.moveTo(x, y); }
  sh.lineTo(0.47, -0.005);
  for (let i = 0; i <= 8; i++) { const a = -Math.PI / 2 + (i / 8) * Math.PI; sh.lineTo(0.47 + 0.045 * Math.cos(a), 0.04 + 0.045 * Math.sin(a)); }
  sh.lineTo(0.0, rc);
  geo('shape_guard', new THREE.ShapeGeometry(sh, 6));
}
{
  const shape = new THREE.Shape();
  const ex = 1.3, z0 = 4.1, cw = 3.1, cp = 1.9, rr = Math.min(0.6, cp * 0.5);
  shape.moveTo(ex - 0.05, -(z0 - cw / 2));
  shape.lineTo(ex + cp - rr, -(z0 - cw / 2));
  shape.absarc(ex + cp - rr, -(z0 - cw / 2) - rr, rr, Math.PI / 2, 0, true);
  shape.lineTo(ex + cp, -(z0 + cw / 2) + rr);
  shape.absarc(ex + cp - rr, -(z0 + cw / 2) + rr, rr, 0, -Math.PI / 2, true);
  shape.lineTo(ex - 0.05, -(z0 + cw / 2));
  geo('extrude', new THREE.ExtrudeGeometry(shape, { depth: 0.16, bevelEnabled: false, curveSegments: 8 }));
}
{
  // many points: exercises earcut's z-order hashed path (> 80 vertices)
  const s = new THREE.Shape();
  for (let i = 0; i < 120; i++) {
    const a = (i / 120) * Math.PI * 2, r = 1 + 0.3 * Math.sin(a * 7) + 0.1 * Math.cos(a * 13);
    if (i) s.lineTo(r * Math.cos(a), r * Math.sin(a)); else s.moveTo(r * Math.cos(a), r * Math.sin(a));
  }
  const h = new THREE.Path();
  h.absarc(0.1, -0.05, 0.25, 0, Math.PI * 2, false);
  s.holes.push(h);
  geo('shape_big', new THREE.ShapeGeometry(s, 12));
}

// --- math
{
  const r = [];
  const q = new THREE.Quaternion();
  for (const o of ['XYZ', 'YXZ', 'ZXY', 'ZYX', 'YZX', 'XZY']) {
    q.setFromEuler(new THREE.Euler(0.4, -1.3, 2.1, o));
    r.push(q.x, q.y, q.z, q.w);
    const m = new THREE.Matrix4().makeRotationFromQuaternion(q);
    const e = new THREE.Euler().setFromRotationMatrix(m, o);
    r.push(e.x, e.y, e.z);
  }
  const m = new THREE.Matrix4().compose(new THREE.Vector3(1, 2, 3), q, new THREE.Vector3(-1.5, 0.5, 2));
  r.push(...m.elements);
  r.push(...m.clone().invert().elements);
  const p = new THREE.Vector3(), qq = new THREE.Quaternion(), s = new THREE.Vector3();
  m.decompose(p, qq, s);
  r.push(p.x, p.y, p.z, qq.x, qq.y, qq.z, qq.w, s.x, s.y, s.z);
  r.push(...new THREE.Matrix4().lookAt(new THREE.Vector3(1, 2, 3), new THREE.Vector3(-2, 0.5, 1), new THREE.Vector3(0, 1, 0)).elements);
  const cam = new THREE.PerspectiveCamera(50, 16 / 9, 0.1, 30000);
  cam.updateProjectionMatrix();
  r.push(...cam.projectionMatrix.elements);
  const oc = new THREE.OrthographicCamera(-30, 40, 20, -15, 0.5, 700);
  oc.updateProjectionMatrix();
  r.push(...oc.projectionMatrix.elements);
  const u = new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 1, 0), new THREE.Vector3(0.3, 0.2, -0.9).normalize());
  r.push(u.x, u.y, u.z, u.w);
  const sl = new THREE.Quaternion().setFromEuler(new THREE.Euler(0.1, 0.2, 0.3)).slerp(u, 0.37);
  r.push(sl.x, sl.y, sl.z, sl.w);
  const v = new THREE.Vector3(0.3, -1.2, 2.5).applyQuaternion(u);
  r.push(v.x, v.y, v.z);
  r.push(...new THREE.Matrix4().makeRotationAxis(new THREE.Vector3(0.2, 0.9, -0.3).normalize(), 1.1).elements);
  const nm = new THREE.Matrix3().getNormalMatrix(m);
  r.push(...nm.elements);
  // colours
  for (const hex of [0x8d908c, 0xf4f2ec, 0x102030, 0xffffff, 0x0a0b0c]) {
    const c = new THREE.Color(hex);
    r.push(c.r, c.g, c.b, c.getHex());
  }
  const c2 = new THREE.Color().setHSL(0.93, 0.6, 0.4);
  r.push(c2.r, c2.g, c2.b);
  const c3 = new THREE.Color(0xd6c6a0).offsetHSL(0.05, -0.1, 0.07);
  r.push(c3.r, c3.g, c3.b, c3.getHex());
  const hsl = {};
  new THREE.Color(0x3a7fb2).getHSL(hsl);
  r.push(hsl.h, hsl.s, hsl.l);
  nums('math', r);
}
{
  // mulberry32 (textures/noise.js)
  function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }
  const r = [];
  for (const seed of [1, 4242, 9121, 7 * 13 + 3, -5, 2 ** 33 + 17]) {
    const f = mulberry32(seed);
    for (let i = 0; i < 6; i++) r.push(f());
  }
  r.push(Math.round(2.5), Math.round(-2.5), Math.round(0.49999999999999994), Math.round(-0.5), (-7.9) | 0, (3e9) | 0, (-1) >>> 0);
  nums('rng', r);
}

process.stdout.write(out.join('\n') + '\n');
