
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
attribute vec4 aGround; uniform vec3 uSunDir; uniform float uBirdH; varying float bShade;
#include <common>
#include <batching_pars_vertex>
#include <uv_pars_vertex>
#include <envmap_pars_vertex>
#include <color_pars_vertex>
#include <fog_pars_vertex>
#include <morphtarget_pars_vertex>
#include <skinning_pars_vertex>
#include <logdepthbuf_pars_vertex>
#include <clipping_planes_pars_vertex>
void main() {
	#include <uv_vertex>
	#include <color_vertex>
	#include <morphinstance_vertex>
	#include <morphcolor_vertex>
	#include <batching_vertex>
	#if defined ( USE_ENVMAP ) || defined ( USE_SKINNING )
		#include <beginnormal_vertex>
		#include <morphnormal_vertex>
		#include <skinbase_vertex>
		#include <skinnormal_vertex>
		#include <defaultnormal_vertex>
	#endif
	vec3 bPos = position; vec3 bN = normal; birdDeform(bPos, bN); vec3 transformed = bPos;
	#include <morphtarget_vertex>
	#include <skinning_vertex>
	
        // project along the sun onto the ground plane under the bird (so overlapping parts
        // land at equal depth and the LessDepth test keeps them from darkening twice)
        vec4 bW = modelMatrix * instanceMatrix * vec4(transformed, 1.0);
        vec3 bO = (modelMatrix * vec4(instanceMatrix[3].xyz, 1.0)).xyz;
        float bPlane = aGround.x + aGround.y * (bW.x - bO.x) + aGround.z * (bW.z - bO.z);
        float bDen = max(0.02, uSunDir.y - aGround.y * uSunDir.x - aGround.z * uSunDir.z);
        vec3 bP = bW.xyz - uSunDir * max(0.0, (bW.y - bPlane) / bDen);
        bP.y += 0.015;
        vec4 mvPosition = viewMatrix * vec4(bP, 1.0);
        gl_Position = projectionMatrix * mvPosition;
        // one flat depth per bird (the nearer of its feet and its shadow tip), so parts that
        // overlap in the shadow fail the LessDepth test exactly instead of streaking
        float bScale = length(instanceMatrix[0].xyz);
        vec2 bH = normalize(uSunDir.xz) * (uBirdH * bScale * sqrt(1.0 - uSunDir.y * uSunDir.y) / uSunDir.y);
        vec4 bC0 = projectionMatrix * viewMatrix * vec4(bO.x, aGround.x, bO.z, 1.0);
        vec4 bC1 = projectionMatrix * viewMatrix * vec4(bO.x - bH.x, aGround.x - aGround.y * bH.x - aGround.z * bH.y, bO.z - bH.y, 1.0);
        float bZ = min(bC0.z / max(bC0.w, 1e-3), bC1.z / max(bC1.w, 1e-3));
        gl_Position.z = bZ * gl_Position.w;
        bShade = aGround.w;
	#include <logdepthbuf_vertex>
	#include <clipping_planes_vertex>
	#include <worldpos_vertex>
	#include <envmap_vertex>
	#include <fog_vertex>
}