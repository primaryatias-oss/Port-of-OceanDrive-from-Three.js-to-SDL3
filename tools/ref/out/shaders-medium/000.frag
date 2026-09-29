#version 300 es
#define varying in
layout(location = 0) out highp vec4 pc_fragColor;
#define gl_FragColor pc_fragColor
#define gl_FragDepthEXT gl_FragDepth
#define texture2D texture
#define textureCube texture
#define texture2DProj textureProj
#define texture2DLodEXT textureLod
#define texture2DProjLodEXT textureProjLod
#define textureCubeLodEXT textureLod
#define texture2DGradEXT textureGrad
#define texture2DProjGradEXT textureProjGrad
#define textureCubeGradEXT textureGrad
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
#define SHADER_NAME SkyEnv
#define FLIP_SIDED
uniform mat4 viewMatrix;
uniform vec3 cameraPosition;
uniform bool isOrthographic;
#define OPAQUE
vec4 LinearTransferOETF( in vec4 value ) {
	return value;
}
vec4 sRGBTransferEOTF( in vec4 value ) {
	return vec4( mix( pow( value.rgb * 0.9478672986 + vec3( 0.0521327014 ), vec3( 2.4 ) ), value.rgb * 0.0773993808, vec3( lessThanEqual( value.rgb, vec3( 0.04045 ) ) ) ), value.a );
}
vec4 sRGBTransferOETF( in vec4 value ) {
	return vec4( mix( pow( value.rgb, vec3( 0.41666 ) ) * 1.055 - vec3( 0.055 ), value.rgb * 12.92, vec3( lessThanEqual( value.rgb, vec3( 0.0031308 ) ) ) ), value.a );
}
vec4 linearToOutputTexel( vec4 value ) {
	return LinearTransferOETF( vec4( value.rgb * mat3( 1.0000,-0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000 ), value.a ) );
}
float luminance( const in vec3 rgb ) {
	const vec3 weights = vec3( 0.2126, 0.7152, 0.0722 );
	return dot( weights, rgb );
}


      varying vec3 vDir;
      

const vec3 OD_SUN = vec3(0.97747, 0.12187, 0.17235);
const vec3 OD_SUNCOL = vec3(1.0000, 0.6000, 0.4000);
const float OD_SUN_I = 5.100;

float odSunSide(vec3 d) {
  vec2 hd = d.xz / max(length(d.xz), 1e-4);
  vec2 hs = normalize(OD_SUN.xz);
  return clamp(dot(hd, hs) * 0.5 + 0.5, 0.0, 1.0); // 1 toward the sun, 0 opposite
}

// Sun glow: Henyey-Greenstein forward-scattering lobe (g = 0.86) for the broad warm
// hotspot, plus the photographed sun: a blown white-yellow core ~4-5 deg across (lens
// flare of an over-exposed sun) fading through cream into gold; the true disc sits
// hidden inside the clipped core.
vec3 odSunGlow(float mu, float lobe, float core) {
  const float g = 0.86;
  float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * mu, 1.5) * 0.0796;
  float th = sqrt(max(2.0 * (1.0 - mu), 0.0));          // angle from the sun (rad)
  vec3 c = vec3(1.0, 0.50, 0.14) * 0.26 * hg * lobe;
  float disc = 1.0 - smoothstep(0.0095, 0.0125, th);
  c += core * (vec3(1.0, 0.80, 0.46) * 5.5 * exp(-pow(th / 0.028, 1.5))    // clipped core -> cream
             + vec3(1.0, 0.58, 0.20) * 1.2 * exp(-th / 0.07)                // cream -> gold
             + vec3(12.0, 9.0, 5.0) * disc);
  return c;
}

// The glow of a sun on the horizon is flattened: a warm band hugging the horizon,
// much wider than tall (low-level haze layering and refraction).
vec3 odSunBand(vec3 d) {
  float dA = acos(clamp(dot(normalize(d.xz + 1e-5), normalize(OD_SUN.xz)), -1.0, 1.0));
  float dE = max(d.y, 0.0) - OD_SUN.y;
  return vec3(1.0, 0.40, 0.09) * 0.5 * exp(-pow(dA / 0.32, 2.0) - pow(dE / (dE < 0.0 ? 0.06 : 0.045), 2.0));
}

vec3 odSkyBase(vec3 d, float glowScale) {
  float e = max(d.y, 0.0);
  float mu = dot(d, OD_SUN);
  float az = odSunSide(d);

  // Anti-solar side: dusty blue-grey dome, pink "belt" above a blue-grey earth-shadow band.
  // clear pale blue dome, a soft pink band just above the horizon
  vec3 away = mix(vec3(0.400, 0.540, 0.780), vec3(0.250, 0.380, 0.650), smoothstep(0.35, 0.95, e));
  away = mix(vec3(0.600, 0.620, 0.760), away, smoothstep(0.1, 0.32, e));
  away = mix(vec3(0.860, 0.640, 0.660), away, smoothstep(0.02, 0.14, e));
  away = mix(vec3(0.720, 0.640, 0.700), away, smoothstep(0.0, 0.03, e));

  // Solar side, a broad graded band: red-orange at the horizon -> deep orange (~6 deg)
  // -> orange-gold (~15 deg) -> pale yellow -> clean blue-grey.
  vec3 sun = mix(vec3(0.270, 0.420, 0.690), vec3(0.200, 0.300, 0.520), smoothstep(0.35, 0.95, e));
  sun = mix(vec3(0.780, 0.640, 0.420), sun, smoothstep(0.14, 0.50, e));
  sun = mix(vec3(1.050, 0.540, 0.160), sun, smoothstep(0.05, 0.27, e));
  sun = mix(vec3(0.950, 0.300, 0.070), sun, smoothstep(0.0, 0.11, e));

  float sw = pow(az, 1.5 + 4.0 * e);
  vec3 col = mix(away, sun, sw);
  return col + odSunGlow(mu, 0.8, glowScale) + odSunBand(d) * glowScale;
}

// Aerial-perspective colour: the sky just above the horizon in that direction.
vec3 odHaze(vec3 d) {
  vec3 hd = normalize(vec3(d.x, max(d.y, 0.0) * 0.5 + 0.004, d.z));
  // only part of the forward-scattering lobe: a 40 m slab of air toward the sun is not the whole sky
  return odSkyBase(hd, 0.0) - odSunGlow(dot(hd, OD_SUN), 0.7, 0.0);
}


float odHash(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float odNoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(odHash(i), odHash(i + vec2(1, 0)), u.x),
             mix(odHash(i + vec2(0, 1)), odHash(i + vec2(1, 1)), u.x), u.y);
}
float odFbm(vec2 p) {
  float a = 0.5, s = 0.0;
  mat2 r = mat2(0.8, 0.6, -0.6, 0.8);
  for (int i = 0; i < 4; i++) { s += a * odNoise(p); p = r * p * 2.03 + 17.1; a *= 0.5; }
  return s;
}

float odWorley(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  float m = 1.0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 o = vec2(float(x), float(y));
    vec2 r = o + vec2(odHash(i + o), odHash(i + o + 19.7)) * 0.9 - f;
    m = min(m, dot(r, r));
  }
  return sqrt(m);
}

// Low altocumulus / small cumulus patches near the horizon: large-scale coverage
// times packed round puffs (Worley), so they read as clumps rather than smears.
float odCloudField(vec2 q) {
  vec2 w = vec2(odFbm(q * 0.35 + 3.3), odFbm(q * 0.35 + 7.9)) - 0.5;
  vec2 qs = q + w * 1.8;
  float cov = odFbm(qs * 0.26 + 1.7);
  float torn = odFbm(qs * 1.3 + w * 2.0 + 4.0);
  float pf = 1.0 - odWorley(qs * 1.6 + 5.0);
  return cov * 0.78 + torn * 0.34 + pf * 0.1 - 0.06;
}

// Cloud plane projection, less flattened toward the horizon than a true plane
// (radial/tangential ratio (y + 0.1) / 0.55) so distant puffs keep some height.
vec2 odCloudUv(vec3 d) { return d.xz / pow(max(d.y, 0.0) + 0.1, 0.55) * 1.1; }

vec4 odClouds(vec3 d, float detail) {
  if (d.y <= 0.0) return vec4(0.0);
  vec2 q = odCloudUv(d);
  float n = odCloudField(q);
  float fine = odNoise(q * 9.0) * 0.6 + odNoise(q * 19.0) * 0.4;
  vec2 toSun = normalize(odCloudUv(normalize(OD_SUN + vec3(0.0, 0.02, 0.0))) - q + 1e-4);
  float n2 = detail > 0.5 ? odCloudField(q + toSun * 0.12) : n - 0.02;
  // density just below: puffs are flat-bottomed, their undersides in shade
  float nb = detail > 0.5 ? odCloudField(odCloudUv(normalize(d - vec3(0.0, 0.012, 0.0)))) : n;

  float low = smoothstep(0.30, 0.03, d.y);
  float th = 0.53 - 0.04 * low;
  float nn = n + (fine - 0.5) * 0.07;
  // soft, feathered puff edges (no hard sticker outlines near the horizon)
  float dens = smoothstep(th - 0.008 - 0.012 * low, th + 0.03, nn) * low;
  dens *= smoothstep(0.0, 0.02, d.y);                    // melt into horizon haze
  float thick = smoothstep(th + 0.01, th + 0.1, nn);
  float lit = clamp((n - n2) * 14.0 + 0.5, 0.0, 1.0);    // sun-facing side of the puff
  float under = clamp((n - nb) * 9.0 + 0.45, 0.0, 1.0) * thick;   // bottom of a thick puff
  float wv = 0.35 + 0.65 * odNoise(q * 3.3 + 2.0);       // rim width varies along the edge

  float mu = max(dot(d, OD_SUN), 0.0);
  float near = pow(mu, 4.0);                             // ~0.94 at 10 deg, 0.78 at 20, 0.56 at 30, 0.25 at 45
  const float g = 0.8;
  float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * mu, 1.5) * 0.0796;
  float thin = 1.0 - thick;
  // Away from the sun: sunlit pink-peach tops over shaded lavender-grey bases.
  // Away from the sun: peach-orange sun-facing sides, lavender-grey shaded bodies and
  // darker undersides.
  vec3 cA = mix(vec3(0.33, 0.30, 0.43), vec3(1.15, 0.78, 0.48), smoothstep(0.35, 0.75, lit) * (0.55 + 0.45 * thin));
  cA *= 1.0 - 0.6 * under;
  cA += vec3(1.0, 0.7, 0.38) * 0.28 * smoothstep(0.55, 0.9, lit) * (1.0 - thick) * (1.0 - under);   // bright sun-facing rim
  // Backlit, near the sun: thick bodies slate-purple with soft internal gradients;
  // only the sun-facing edge forward-scatters, orange-gold, in a rim of varying width.
  vec3 edge = vec3(1.0, 0.52, 0.17) * (1.2 + 2.2 * hg);
  vec3 body = mix(vec3(0.28, 0.23, 0.32), vec3(0.13, 0.11, 0.18), thick) * (1.0 - 0.5 * under);
  float rim = smoothstep(0.5, 0.85, lit) * pow(thin, 1.4 * (1.6 - wv));
  vec3 cS = mix(body, edge, rim) + vec3(0.9, 0.35, 0.08) * 0.35 * hg * thin * (1.0 - rim);
  vec3 col = mix(cA, cS, near);

  // thin cirrus higher up: faint wisps, gold toward the sun, pink elsewhere
  vec2 cq = d.xz / (d.y + 0.12);
  float ci = odFbm(vec2(cq.x * 0.8 + cq.y * 0.3, cq.y * 4.0 - cq.x * 0.4) + 11.0);
  float ciD = smoothstep(0.6, 0.82, ci) * smoothstep(0.08, 0.2, d.y) * smoothstep(0.75, 0.3, d.y) * 0.22 * (1.0 - 0.7 * near);
  vec3 ciC = mix(vec3(0.95, 0.66, 0.58), vec3(1.0, 0.55, 0.18) * (0.9 + 0.4 * hg), near);
  float a = clamp(dens, 0.0, 1.0) * 0.97;
  col = mix(ciC, col, a / max(a + ciD * (1.0 - a), 1e-4));
  a = a + ciD * (1.0 - a);
  return vec4(col, a);
}

// mode 0: visible sky (sun disc). mode 1: skylight environment. mode 2: water reflection.
vec3 odSky(vec3 d, float mode) {
  vec3 hd = normalize(vec3(d.x, max(d.y, 0.0), d.z));
  vec3 col = odSkyBase(hd, mode < 0.5 ? 1.0 : 0.0);
  vec4 cl = odClouds(d, mode > 1.5 ? 0.0 : 1.0);
  col = mix(col, cl.rgb, cl.a);
  // water: the sun's aureole reaches it only through the glitter (spec lobe), not as a broad tint
  if (mode > 1.5) col -= odSunGlow(dot(hd, OD_SUN), 0.75, 0.0) * (1.0 - cl.a);
  // (and the gold horizon under the sun reaches the near water only in the glitter path:
  // off-path wave faces there read steel-grey rather than tinted orange)
  if (mode > 1.5) col = mix(col, dot(col, vec3(0.2126, 0.7152, 0.0722)) * vec3(0.93, 0.95, 1.02), 0.5 * pow(odSunSide(hd), 3.0));

  if (mode < 0.5) {
    // a little glare in the air in front of the clouds: only right at the sun do they burn out
    col += odSunGlow(dot(hd, OD_SUN), 0.12, 0.15) * cl.a + odSunBand(hd) * 0.4 * cl.a;
  } else if (mode < 1.5) {
    // Skylight: compress the bright solar horizon and bias cooler, so shade reads
    // blue-violet against the gold sun instead of being filled by sunrise glow.
    // The sun-side half of the dome is the bright, warm (peach) one; the anti-solar
    // half is dim and blue. Open horizontal ground sees both (warm-neutral fill,
    // fairly bright shade); faces turned away from the sun see only the dim blue half.
    float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float up = max(d.y, 0.0);
    float az = mix(odSunSide(d), 0.5, up * up);
    col = mix(col, l * vec3(0.66, 0.86, 1.42), 0.85 * pow(1.0 - az, 1.5));
    col = mix(col, l * vec3(1.06, 1.0, 0.92), 0.5 * up * az);   // overhead: near-neutral, the dim blue zenith offset by the peach sun-side dome
    col /= 1.0 + 1.5 * l;
    // Weighted to the open dome overhead; the low solar sky is compressed so a
    // sun-facing wall in shade gets modest, cooler fill (lit:shade ~3:1 on walls).
    col = mix(col, l * vec3(0.74, 0.90, 1.34), 0.55 * (1.0 - up) * az);   // low solar sky as fill: cooler, it is mostly the blue-grey dome around the glow
    col *= (0.3 + 3.2 * up) * (0.12 + 1.9 * az * az);
    // lower hemisphere: warm bounce from sunlit pavement, sand and walls
    float g = smoothstep(0.0, -0.2, d.y);
    // (toward the sun you see lit faces; away from it, mostly cast shadows)
    vec3 ground = mix(vec3(0.052, 0.06, 0.078), vec3(0.15, 0.115, 0.095), pow(odSunSide(d), 1.5));
    col = mix(col, ground, g);
  }
  return col;
}

      void main() {
        gl_FragColor = vec4(odSky(normalize(vDir), 1.0), 1.0);
      }