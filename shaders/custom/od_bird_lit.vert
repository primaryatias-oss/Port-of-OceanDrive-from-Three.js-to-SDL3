
attribute float aPart;
attribute vec3 cA;
attribute vec3 cB;
attribute vec3 cU;
attribute vec3 aFeather;
attribute vec4 aPoseA;   // shoulder elevation, wrist dihedral, fold 0..1, hand sweep
attribute vec4 aPoseB;   // head pitch, head thrust, leg phase, leg tuck 0..1
attribute vec4 aPoseC;   // head yaw, tail pitch, -, palette B mix
uniform vec3 uShoulder;
uniform vec3 uWrist;
uniform vec3 uNeck;
uniform vec3 uHip;
uniform vec3 uTailP;
uniform float uLegSwing;
mat3 bRotX(float a) { float c = cos(a), s = sin(a); return mat3(1., 0., 0., 0., c, s, 0., -s, c); }
mat3 bRotY(float a) { float c = cos(a), s = sin(a); return mat3(c, 0., -s, 0., 1., 0., s, 0., c); }
mat3 bRotZ(float a) { float c = cos(a), s = sin(a); return mat3(c, s, 0., -s, c, 0., 0., 0., 1.); }
void birdDeform(inout vec3 p, inout vec3 n) {
  float part = aPart;
  float s = p.x < 0.0 ? -1.0 : 1.0;
  if (part > 1.5 && part < 2.5) {             // hand: dihedral at the wrist, swept by a shear
    vec3 W = vec3(uWrist.x * s, uWrist.yz);   // (keeps the wrist line joined to the arm)
    vec3 q = p - W;
    q.z -= aPoseA.w * abs(q.x);
    mat3 R = bRotZ(aPoseA.y * s);
    p = W + R * q; n = R * n;
  }
  if (part > 0.5 && part < 2.5) {             // whole spread wing about the shoulder
    vec3 S = vec3(uShoulder.x * s, uShoulder.yz);
    float f = aPoseA.z;
    mat3 R = bRotZ(aPoseA.x * s) * bRotY(f * 1.3 * s);
    p = S + R * ((p - S) * (1.0 - f)); n = R * n;
  } else if (part > 2.5 && part < 3.5) {      // folded wing, grows in as the wing folds
    vec3 S = vec3(uShoulder.x * s, uShoulder.yz);
    p = S + (p - S) * aPoseA.z;
  } else if (part > 3.5 && part < 4.5) {      // head
    mat3 R = bRotY(aPoseC.x) * bRotX(aPoseB.x);
    p = uNeck + R * (p - uNeck) + vec3(0.0, 0.0, aPoseB.y); n = R * n;
  } else if (part > 4.5 && part < 5.5) {      // legs: alternate swing, tucked in flight
    vec3 H = vec3(uHip.x * s, uHip.yz);
    float a = sin(aPoseB.z * 6.2832 + (s > 0.0 ? 0.0 : 3.1416)) * uLegSwing + aPoseB.w * 1.4;
    mat3 R = bRotX(a);
    p = H + R * ((p - H) * (1.0 - 0.92 * aPoseB.w)); n = R * n;
  } else if (part > 5.5) {                    // tail
    mat3 R = bRotX(aPoseC.y);
    p = uTailP + R * (p - uTailP); n = R * n;
  }
}
varying vec3 bCol; varying vec3 bColU; varying vec3 bFeather; varying float bThin;
#define STANDARD
varying vec3 vViewPosition;
#ifdef USE_TRANSMISSION
	varying vec3 vWorldPosition;
#endif
#include <common>
#include <batching_pars_vertex>
#include <uv_pars_vertex>
#include <displacementmap_pars_vertex>
#include <color_pars_vertex>
#include <fog_pars_vertex>
#include <normal_pars_vertex>
#include <morphtarget_pars_vertex>
#include <skinning_pars_vertex>
#include <shadowmap_pars_vertex>
#include <logdepthbuf_pars_vertex>
#include <clipping_planes_pars_vertex>
void main() {
	#include <uv_vertex>
	#include <color_vertex>
	#include <morphinstance_vertex>
	#include <morphcolor_vertex>
	#include <batching_vertex>
	vec3 objectNormal = normal; vec3 bPos = position; birdDeform(bPos, objectNormal);
        bCol = mix(cA, cB, aPoseC.w); bColU = aPart > 0.5 && aPart < 2.5 ? cU : bCol; bFeather = aFeather;
        bThin = (aPart > 0.5 && aPart < 2.5) || aPart > 5.5 ? 1.0 : 0.0;
	#include <morphnormal_vertex>
	#include <skinbase_vertex>
	#include <skinnormal_vertex>
	#include <defaultnormal_vertex>
	#include <normal_vertex>
	vec3 transformed = bPos;
	#include <morphtarget_vertex>
	#include <skinning_vertex>
	#include <displacementmap_vertex>
	#include <project_vertex>
	#include <logdepthbuf_vertex>
	#include <clipping_planes_vertex>
	vViewPosition = - mvPosition.xyz;
	#include <worldpos_vertex>
	#include <shadowmap_vertex>
	#include <fog_vertex>
#ifdef USE_TRANSMISSION
	vWorldPosition = worldPosition.xyz;
#endif
}