#version 300 es

#define attribute in
#define varying out
#define texture2D texture
precision highp float;
	precision highp int;
	precision highp sampler2D;
	precision highp samplerCube;
	precision highp sampler3D;
	precision highp sampler2DArray;
	precision highp sampler2DShadow;
	precision highp samplerCubeShadow;
	precision highp sampler2DArrayShadow;
	precision highp isampler2D;
	precision highp isampler3D;
	precision highp isamplerCube;
	precision highp isampler2DArray;
	precision highp usampler2D;
	precision highp usampler3D;
	precision highp usamplerCube;
	precision highp usampler2DArray;
	
#define HIGH_PRECISION
#define SHADER_TYPE ShaderMaterial
#define SHADER_NAME Ocean
#define USE_FOG
#define FOG_EXP2
#define USE_SHADOWMAP
#define SHADOWMAP_TYPE_PCF
uniform mat4 modelMatrix;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
uniform mat4 viewMatrix;
uniform mat3 normalMatrix;
uniform vec3 cameraPosition;
uniform bool isOrthographic;
#ifdef USE_INSTANCING
	attribute mat4 instanceMatrix;
#endif
#ifdef USE_INSTANCING_COLOR
	attribute vec3 instanceColor;
#endif
#ifdef USE_INSTANCING_MORPH
	uniform sampler2D morphTexture;
#endif
attribute vec3 position;
attribute vec3 normal;
attribute vec2 uv;
#ifdef USE_UV1
	attribute vec2 uv1;
#endif
#ifdef USE_UV2
	attribute vec2 uv2;
#endif
#ifdef USE_UV3
	attribute vec2 uv3;
#endif
#ifdef USE_TANGENT
	attribute vec4 tangent;
#endif
#if defined( USE_COLOR_ALPHA )
	attribute vec4 color;
#elif defined( USE_COLOR )
	attribute vec3 color;
#endif
#ifdef USE_SKINNING
	attribute vec4 skinIndex;
	attribute vec4 skinWeight;
#endif



#ifdef USE_FOG
  varying vec3 vFogOffset;
#endif
      uniform float uTime;
      uniform vec2 uCam;
      varying vec3 vWorld;
      varying vec2 vBase;
      varying float vCrest;
      varying float vWhite;
      
uniform vec4 uSurfA[4];   // t0, k, size, runup
uniform vec4 uSurfB[4];   // zc, phase
uniform float uSurfT;
const float SURF_BREAK_X = 95.050;
const float SURF_SHORE_X = 92.550;
float surfHash(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float surfN(vec2 p) {
  vec2 i = floor(p), f = fract(p); vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(surfHash(i), surfHash(i + vec2(1, 0)), u.x), mix(surfHash(i + vec2(0, 1)), surfHash(i + vec2(1, 1)), u.x), u.y);
}

float surfRunup(vec4 A, vec4 B, float z) {
  return A.w * (0.8 + 0.12 * sin(z * 0.061 + B.y) + 0.08 * sin(z * 0.17 + 2.0 * B.y))
    * (0.85 + 0.15 * exp(-pow((z - B.x) / 70.0, 2.0)));
}
// x = landward water edge (1e4 when dry), y = freshness, z = event size
vec3 surfFront(float z, float t) {
  vec3 best = vec3(1e4, 0.0, 0.0);
  for (int i = 0; i < 4; i++) {
    vec4 A = uSurfA[i]; vec4 B = uSurfB[i];
    float tau = t - A.x, k = A.y;
    float tb = 2.20 * k, tu = 2.30 * k, td = 4.00 * k;
    if (tau < tb || tau > tb + tu + td) continue;
    float r, fresh;
    if (tau < tb + tu) { float u = (tau - tb) / tu; r = 1.0 - (1.0 - u) * (1.0 - u); fresh = 1.0; }
    else { float d = (tau - tb - tu) / td; r = 1.0 - pow(d, 1.4); fresh = 1.0 - d; }
    float lobes = (0.22 * sin(z * 0.83 + B.y * 3.0) + 0.12 * sin(z * 2.1 + B.y * 5.0) + 0.3 * abs(sin(z * 1.9 + B.y * 2.0)) - 0.15) * r;   // scalloped
    float f = SURF_BREAK_X - surfRunup(A, B, z) * r + lobes;
    if (f < best.x) best = vec3(f, fresh, A.z);
  }
  return best;
}
// Approaching crests (height, m) and whitewater on them. x/z world.
float surfCrest(float x, float z, float t, out float white) {
  float h = 0.0; white = 0.0;
  for (int i = 0; i < 4; i++) {
    vec4 A = uSurfA[i]; vec4 B = uSurfB[i];
    float tau = t - A.x, k = A.y;
    // the set breaks in short overlapping segments: each stretch of shore has its own
    // height, lag and break time, so the crest is never one ruler-straight roller
    float sg = surfN(vec2(z * 0.055 + B.y * 7.0, B.y * 3.1));
    float sg2 = surfN(vec2(z * 0.13 + B.y * 2.0, 5.0 + B.y));
    float tb = 2.15 * k + (sg - 0.5) * 1.6 + (sg2 - 0.5) * 0.5;
    if (tau < -5.0 || tau > tb + 2.5) continue;
    float amp = (0.4 + 0.4 * A.z) * (0.3 + 0.9 * smoothstep(0.2, 0.75, sg)) * (0.75 + 0.5 * sg2);
    float xc = SURF_BREAK_X + (tb - tau) * 3.0 + 1.2 * sin(z * 0.031 + B.y) + 0.5 * sin(z * 0.11 + 2.0 * B.y) + (sg2 - 0.5) * 1.6;
    float d = x - xc;
    float pre = clamp((tau + 5.0) / (tb + 5.0), 0.0, 1.0);   // 0 far out .. 1 at the break
    float wf = mix(2.2, 0.8, pre * pre);                 // shoreward face steepens
    float prof = d < 0.0 ? exp(-d * d / (wf * wf)) : exp(-d * d / 7.0);
    float grow = smoothstep(0.0, 0.5, pre);
    float collapse = tau < tb ? 1.0 : exp(-(tau - tb) / 0.45);
    h += amp * prof * grow * mix(0.25, 1.0, collapse);
    // spilling lip: white on the crest top around the break, then the collapsing roller
    float lip = smoothstep(tb - 0.6, tb, tau) * (tau < tb ? 1.0 : exp(-(tau - tb) / 1.2));
    vec2 wq = vec2(z * 1.3, x * 1.9 - t * 2.4);
    wq += vec2(surfN(wq * 0.37 + 3.0), surfN(wq * 0.41 + 8.0)) * 2.6;
    float wn2 = surfN(wq) * 0.65 + surfN(wq * 2.7 + 1.3) * 0.35;
    white = max(white, lip * smoothstep(0.35, 0.9, prof) * smoothstep(0.25, 0.7, wn2) * smoothstep(0.15, 0.5, amp));
  }
  // between the sets: 2-3 rows of small spilling breakers, segmented along the shore,
  // growing as they shoal, white on the crest and a whitewater trail behind once broken
  float ph = (x - SURF_BREAK_X) / 8.5 + t * 3.0 / 8.5 + 0.35 * sin(z * 0.05) + 0.9 * surfN(vec2(z * 0.035, x * 0.04));
  float f = fract(ph), row = floor(ph);
  float seg = smoothstep(0.35, 0.7, surfN(vec2(z * 0.07 + row * 3.7, row * 1.3))) * (0.5 + 0.5 * surfN(vec2(z * 0.19, row * 2.1)));
  float grow = smoothstep(SURF_BREAK_X + 30.0, SURF_BREAK_X + 8.0, x) * smoothstep(SURF_BREAK_X - 1.5, SURF_BREAK_X + 1.5, x);
  float back = exp(-f * f / 0.06), face = exp(-(1.0 - f) * (1.0 - f) / 0.02);
  float rp = max(back, face);
  h += (0.12 + 0.3 * seg) * grow * rp;
  float brk = seg * grow * smoothstep(SURF_BREAK_X + 12.0, SURF_BREAK_X + 5.0, x);
  vec2 wq2 = vec2(z * 0.9, x * 1.1 - t * 1.4);
  wq2 += vec2(surfN(wq2 * 0.43 + 2.0), surfN(wq2 * 0.39 + 6.0)) * 3.0;
  float wn = surfN(wq2) * 0.6 + surfN(wq2 * 2.3 + 4.0) * 0.4;
  white = max(white, brk * (smoothstep(0.8, 0.98, rp) + 0.6 * exp(-f / 0.12)) * smoothstep(0.35, 0.75, wn));
  return h;
}

      
  // Gerstner swell: displacement (xyz) and slope (d h / d x, d h / d z). h = grid spacing
  // (0 per pixel): a component the mesh can't resolve is left to the per-pixel normal,
  // otherwise it swims and pops across the moving vertices.
  vec3 odSwell(vec2 p, float t, float amp, float h, out vec2 slope) {
    vec3 d = vec3(0.0); slope = vec2(0.0);
    { float ph = dot(p, vec2(-0.9928, -0.1197)) * 0.3696 - t * 1.9041 + 0.40;
      float a = 0.090 * amp * (1.0 - smoothstep(1.700, 3.740, h));
      d += vec3(-0.9928 * -0.6 * a * sin(ph), a * cos(ph), -0.1197 * -0.6 * a * sin(ph));
      slope += vec2(-0.9928, -0.1197) * (-a * 0.3696 * sin(ph)); }
    { float ph = dot(p, vec2(-0.9689, 0.2474)) * 0.5712 - t * 2.3672 + 2.10;
      float a = 0.055 * amp * (1.0 - smoothstep(1.100, 2.420, h));
      d += vec3(-0.9689 * -0.6 * a * sin(ph), a * cos(ph), 0.2474 * -0.6 * a * sin(ph));
      slope += vec2(-0.9689, 0.2474) * (-a * 0.5712 * sin(ph)); }
    { float ph = dot(p, vec2(-0.9004, -0.4350)) * 0.8976 - t * 2.9674 + 4.00;
      float a = 0.030 * amp * (1.0 - smoothstep(0.700, 1.540, h));
      d += vec3(-0.9004 * -0.6 * a * sin(ph), a * cos(ph), -0.4350 * -0.6 * a * sin(ph));
      slope += vec2(-0.9004, -0.4350) * (-a * 0.8976 * sin(ph)); }
    return d;
  }
      void main() {
        vec2 p = position.xz + uCam;
        float r = length(position.xz);
        float shore = smoothstep(91.55, 104.55, p.x);
        float amp = shore * (1.0 - smoothstep(120.0, 500.0, r));
        // the grid moves with the camera: displace only by what its spacing resolves
        float h = 0.02927 * max(r, 0.25);
        vec2 sl;
        vec3 d = amp > 0.0 ? odSwell(p, uTime, amp, h, sl) : vec3(0.0);
        float white = 0.0;
        float crest = 0.0;
        if (r < 400.0 && p.x > 90.55 && p.x < 135.05)
          crest = surfCrest(p.x, p.y, uTime, white) * smoothstep(92.05, 94.55, p.x);
        crest *= 1.0 - smoothstep(0.6, 1.4, h);   // breaker faces are ~1 m wide
        vec3 wp = vec3(p.x + d.x, -1.000 + d.y + crest, p.y + d.z);
        vWorld = wp;
        vBase = p;
        vCrest = crest;
        vWhite = white;
        vec4 mvPosition = viewMatrix * vec4(wp, 1.0);
        gl_Position = projectionMatrix * mvPosition;

#ifdef USE_FOG
  vFogOffset = (vec4(mvPosition.xyz, 0.0) * viewMatrix).xyz;
#endif
      }