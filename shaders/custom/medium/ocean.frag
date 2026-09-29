
      uniform float uTime;
      varying vec3 vWorld;
      varying vec2 vBase;
      varying float vCrest;
      varying float vWhite;
      #ifdef USE_FOG
        varying vec3 vFogOffset;
        uniform float fogDensity;
      #endif
      

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

      
vec3 odApplyFog(vec3 col, vec3 offs, float density) {
  float fDist = length(offs);
  vec3 fDir = offs / max(fDist, 1e-4);
  const float fH = 120.0;
  float fDy = offs.y / fH;
  float fK = abs(fDy) > 1e-3 ? (1.0 - exp(-fDy)) / fDy : 1.0;
  // the first ~50 m stay crisp; humid haze builds beyond that
  float fOd = density * exp(-max(cameraPosition.y, 0.0) / fH) * max(fDist - 90.0, 0.0) * mix(0.18, 1.0, smoothstep(250.0, 500.0, fDist)) * fK;
  // The haze colour is the horizon sky, i.e. km of air. Toward the sun that is the
  // blazing glow, so a short slab of it would light up backlit sand as bright as the
  // sky; there the haze is thinned to keep near backlit ground dark as in photos.
  fOd *= mix(1.0, 0.3, smoothstep(0.3, 0.95, dot(fDir, OD_SUN)));
  // down-sun the near air is barely visible: facades 50-150 m away stay crisp and warm
  // (and the sunlit towers behind them, 150-400 m, stay warm rather than lilac boxes)
  fOd *= mix(1.0, 0.45, smoothstep(0.2, 0.8, -dot(normalize(fDir.xz + 1e-5), normalize(OD_SUN.xz))) * (1.0 - smoothstep(100.0, 180.0, fDist)));
  fOd *= mix(1.0, 1.0, smoothstep(0.2, 0.8, -dot(normalize(fDir.xz + 1e-5), normalize(OD_SUN.xz))) * smoothstep(120.0, 220.0, fDist) * (1.0 - smoothstep(350.0, 700.0, fDist)));
  return mix(col, odHaze(fDir), 1.0 - exp(-fOd));
}

      
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

      
float odSandY(float x) {
  float x1 = 86.00;
  if (x < x1) { float t = (x - 12.00) / (x1 - 12.00); return 0.55 - (0.55 - -0.900) * t; }
  float t = (x - x1) / 26.0;
  return -0.900 - 1.6 * min(1.0, t) * min(1.0, t) - 0.2 * max(0.0, t - 1.0);
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

      
vec2 odVor(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  float m1 = 8.0, m2 = 8.0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 o = vec2(float(x), float(y));
    vec2 r = o + vec2(odHash(i + o), odHash(i + o + 19.7)) * 0.9 - f;
    float d = dot(r, r);
    if (d < m1) { m2 = m1; m1 = d; } else if (d < m2) m2 = d;
  }
  return sqrt(vec2(m1, m2));
}
float odFoam(vec2 p, float t) {
  vec2 w = vec2(odNoise(p * 0.9 + 2.3), odNoise(p * 0.9 + 7.9)) - 0.5;
  vec2 w2 = vec2(odNoise(p * 3.1 + 4.1), odNoise(p * 3.1 + 1.3)) - 0.5;
  vec2 pw = p + w * 1.1 + w2 * 0.2;
  vec2 A = odVor(pw * 2.6 + vec2(t * 0.15, 0.0));
  vec2 B = odVor(pw * 6.5 + w * 1.3 - vec2(0.0, t * 0.1) + 3.1);
  float fine = 1.0 - smoothstep(0.015, 0.04, fwidth(p.x) + fwidth(p.y));
  float wa = 0.05 + 0.12 * odNoise(p * 1.7 + 5.0);
  float la = 1.0 - smoothstep(0.0, wa, A.y - A.x);
  float lb = 1.0 - smoothstep(0.0, wa * 0.8, B.y - B.x);
  float brk = smoothstep(0.28, 0.62, odNoise(pw * 1.9 + 11.0));
  float brk2 = smoothstep(0.25, 0.6, odNoise(pw * 4.3 + 17.0));
  float grain = fine > 0.0 ? smoothstep(0.62, 0.85, odNoise(p * 26.0 + w * 3.0)) * fine : 0.0;
  float dens = odNoise(p * 0.6 + t * 0.05);
  // worm-like strands (isolines of warped value noise) so the net is never all polygons
  float r1 = 1.0 - abs(odNoise(pw * 3.3 + w2 * 1.5 + vec2(t * 0.05, 0.0)) * 2.0 - 1.0);
  float r2 = 1.0 - abs(odNoise(pw * 7.9 - w * 2.0 + 3.7) * 2.0 - 1.0);
  float lr = smoothstep(0.86, 0.97, r1) * 0.75 + smoothstep(0.88, 0.98, r2) * 0.45 * brk2;
  // bubbly clots where the foam gathers
  float clot = smoothstep(0.64, 0.82, odNoise(pw * 2.2 + 9.0)) * smoothstep(0.35, 0.7, odNoise(pw * 9.0 + 1.0));
  float lace = max(max(la * brk * 0.7, lr), clot * 0.65) + lb * brk2 * brk * (0.25 + 0.2 * dens) + grain * 0.22;
  float fPatch = smoothstep(0.3, 0.75, odNoise(pw * vec2(0.45, 0.2) + vec2(0.0, t * 0.03)));
  float film = 0.2 * fPatch * smoothstep(0.2, 0.8, odNoise(pw * 1.3 + 3.0));
  return clamp(lace * (0.3 + 0.7 * fPatch) + film, 0.0, 1.0);
}


      void main() {
        vec3 toCam = cameraPosition - vWorld;
        float dist = length(toCam);
        vec3 V = toCam / dist;
        float fp = dist * 0.0018 / sqrt(max(abs(V.y), 0.02));
        vec2 p = vBase;
        float t = uTime;
        if (p.x < 91.05) discard;

        float groundY = odSandY(p.x);
        float depth = vWorld.y - groundY;
        if (depth < -0.02) discard;
        float shore = smoothstep(91.55, 104.55, p.x);
        float chopAmp = mix(0.45, 1.0, smoothstep(0.0, 1.5, depth));

        // normal: swell (analytic) + crest (finite difference) + sub-pixel chop
        vec2 swSl;
        float swAmp = shore * (1.0 - smoothstep(120.0, 500.0, length(p - cameraPosition.xz)));
        if (swAmp > 0.0) odSwell(p, t, swAmp, 0.0, swSl); else swSl = vec2(0.0);
        vec2 crSl = vec2(0.0);
        // whitewater evaluated per pixel (per vertex it smears into grid-aligned dots)
        float whiteF = 0.0;
        float crestP = vCrest;
        if (p.x < 135.05 && dist < 400.0) {
          float w0, w1, w2;
          float e = max(0.35, fp * 1.5);
          // crest height per pixel too: the vertex value is faded where the grid is coarse
          float h0 = surfCrest(p.x, p.y, t, w0);
          float hx = surfCrest(p.x + e, p.y, t, w1), hz = surfCrest(p.x, p.y + e, t, w2);
          crestP = h0 * smoothstep(92.05, 94.55, p.x);
          crSl = vec2(hx - h0, hz - h0) / e;
          crSl *= min(1.0, 0.6 / (length(crSl) + 1e-4));   // no needle-steep facets flashing the sky
          whiteF = 0.5 * (w1 + w2);
        }
        vec2 s = vec2(0.0);
        float lost = 0.0;
        
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 10.269);
      float ph = dot(p, vec2(-0.9984, -0.0560)) * 0.6118 - t * 2.4499 + 3.187;
      s += vec2(-0.9984, -0.0560) * (0.0466 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.001086 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 10.300);
      float ph = dot(p, vec2(-0.9796, -0.2012)) * 0.6100 - t * 2.4463 + 2.522;
      s += vec2(-0.9796, -0.2012) * (0.0337 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000569 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 7.492);
      float ph = dot(p, vec2(-0.9776, 0.2104)) * 0.8386 - t * 2.8683 + 4.740;
      s += vec2(-0.9776, 0.2104) * (0.0409 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000838 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 8.382);
      float ph = dot(p, vec2(-0.9893, -0.1458)) * 0.7496 - t * 2.7117 + 4.720;
      s += vec2(-0.9893, -0.1458) * (0.0337 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000568 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 4.642);
      float ph = dot(p, vec2(-0.8541, -0.5201)) * 1.3536 - t * 3.6440 + 0.672;
      s += vec2(-0.8541, -0.5201) * (0.0375 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000705 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 4.677);
      float ph = dot(p, vec2(-0.8890, 0.4579)) * 1.3435 - t * 3.6303 + 1.322;
      s += vec2(-0.8890, 0.4579) * (0.0285 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000405 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 3.273);
      float ph = dot(p, vec2(-0.8082, 0.5890)) * 1.9199 - t * 4.3398 + 2.697;
      s += vec2(-0.8082, 0.5890) * (0.0326 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000532 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 3.591);
      float ph = dot(p, vec2(-0.9939, -0.1104)) * 1.7496 - t * 4.1429 + 2.269;
      s += vec2(-0.9939, -0.1104) * (0.0405 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000820 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 2.766);
      float ph = dot(p, vec2(-0.7710, 0.6368)) * 2.2718 - t * 4.7208 + 4.147;
      s += vec2(-0.7710, 0.6368) * (0.0205 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000210 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 2.360);
      float ph = dot(p, vec2(-0.9432, -0.3323)) * 2.6623 - t * 5.1105 + 0.149;
      s += vec2(-0.9432, -0.3323) * (0.0305 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000464 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 2.201);
      float ph = dot(p, vec2(-0.6126, -0.7904)) * 2.8551 - t * 5.2923 + 3.774;
      s += vec2(-0.6126, -0.7904) * (0.0258 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000333 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 1.736);
      float ph = dot(p, vec2(-0.8521, -0.5233)) * 3.6186 - t * 5.9581 + 3.847;
      s += vec2(-0.8521, -0.5233) * (0.0273 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000371 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 1.605);
      float ph = dot(p, vec2(-0.6286, -0.7778)) * 3.9151 - t * 6.1973 + 6.240;
      s += vec2(-0.6286, -0.7778) * (0.0298 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000445 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 1.680);
      float ph = dot(p, vec2(-0.6794, 0.7337)) * 3.7400 - t * 6.0572 + 1.177;
      s += vec2(-0.6794, 0.7337) * (0.0279 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000389 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 1.266);
      float ph = dot(p, vec2(-0.5935, -0.8048)) * 4.9630 - t * 6.9776 + 1.271;
      s += vec2(-0.5935, -0.8048) * (0.0249 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000309 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 1.331);
      float ph = dot(p, vec2(-0.7945, -0.6072)) * 4.7215 - t * 6.8057 + 4.452;
      s += vec2(-0.7945, -0.6072) * (0.0262 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000343 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 1.299);
      float ph = dot(p, vec2(-0.6655, 0.7464)) * 4.8361 - t * 6.8878 + 2.653;
      s += vec2(-0.6655, 0.7464) * (0.0272 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000371 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.974);
      float ph = dot(p, vec2(-0.6680, -0.7442)) * 6.4524 - t * 7.9560 + 4.955;
      s += vec2(-0.6680, -0.7442) * (0.0184 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000168 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.897);
      float ph = dot(p, vec2(-0.5129, -0.8584)) * 7.0008 - t * 8.2872 + 3.771;
      s += vec2(-0.5129, -0.8584) * (0.0247 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000306 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.930);
      float ph = dot(p, vec2(-0.9806, 0.1958)) * 6.7537 - t * 8.1396 + 2.101;
      s += vec2(-0.9806, 0.1958) * (0.0167 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000140 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.645);
      float ph = dot(p, vec2(-0.9037, -0.4282)) * 9.7397 - t * 9.7748 + 2.649;
      s += vec2(-0.9037, -0.4282) * (0.0172 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000147 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.585);
      float ph = dot(p, vec2(-0.9220, -0.3872)) * 10.7362 - t * 10.2627 + 4.377;
      s += vec2(-0.9220, -0.3872) * (0.0171 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000147 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.518);
      float ph = dot(p, vec2(-0.9851, -0.1721)) * 12.1268 - t * 10.9071 + 6.274;
      s += vec2(-0.9851, -0.1721) * (0.0219 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000239 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.426);
      float ph = dot(p, vec2(-0.8266, 0.5627)) * 14.7447 - t * 12.0269 + 4.004;
      s += vec2(-0.8266, 0.5627) * (0.0230 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000264 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.437);
      float ph = dot(p, vec2(-0.2812, 0.9597)) * 14.3624 - t * 11.8699 + 4.706;
      s += vec2(-0.2812, 0.9597) * (0.0175 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000153 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.446);
      float ph = dot(p, vec2(-0.2713, 0.9625)) * 14.1010 - t * 11.7614 + 5.418;
      s += vec2(-0.2713, 0.9625) * (0.0225 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000254 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.303);
      float ph = dot(p, vec2(-0.8595, -0.5112)) * 20.7596 - t * 14.2707 + 5.371;
      s += vec2(-0.8595, -0.5112) * (0.0138 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000095 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.324);
      float ph = dot(p, vec2(-0.9828, 0.1847)) * 19.4174 - t * 13.8016 + 5.450;
      s += vec2(-0.9828, 0.1847) * (0.0209 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000218 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.276);
      float ph = dot(p, vec2(-0.2853, -0.9584)) * 22.7442 - t * 14.9372 + 3.080;
      s += vec2(-0.2853, -0.9584) * (0.0137 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000094 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.233);
      float ph = dot(p, vec2(-0.9979, 0.0654)) * 26.9165 - t * 16.2496 + 2.650;
      s += vec2(-0.9979, 0.0654) * (0.0185 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000172 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.189);
      float ph = dot(p, vec2(-0.8926, 0.4508)) * 33.1703 - t * 18.0389 + 6.065;
      s += vec2(-0.8926, 0.4508) * (0.0187 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000174 * chopAmp; }
    { float wg = smoothstep(fp * 2.0, fp * 6.0, 0.203);
      float ph = dot(p, vec2(-0.3645, 0.9312)) * 30.9730 - t * 17.4312 + 3.198;
      s += vec2(-0.3645, 0.9312) * (0.0169 * cos(ph) * wg * chopAmp);
      lost += (1.0 - wg) * 0.000143 * chopAmp; }
        s += swSl + crSl;
        vec3 n = normalize(vec3(-s.x, 1.0, -s.y));
        float nv = max(dot(n, V), 0.002);

        float sig = sqrt(lost + 0.0004);
        vec3 R = reflect(-V, n);
        float gz = 1.0 - smoothstep(0.02, 0.35, V.y);
        vec3 Ra = vec3(R.x, abs(R.y) + (1.2 + 1.6 * gz) * sig + 0.004, R.z);
        vec3 Rb = vec3(R.x, abs(R.y) + (2.2 + 2.8 * gz) * sig + 0.004, R.z);
        vec3 sky = 0.5 * (odSky(normalize(Ra), 2.0) + odSky(normalize(Rb), 2.0));
        float F = 0.02 + 0.98 * pow(1.0 - clamp(nv + (1.0 + 1.2 * gz) * sig, 0.0, 1.0), 5.0);

        // water body: turquoise over the pale sand shallows, steel-blue offshore
        // body: a hint of turquoise only in the very shallow water, silver-blue beyond
        // clear green-turquoise over the pale sand (absorption leaves green), slate offshore
        // a subtle turquoise only over the very shallow sand, grey-blue beyond
        vec3 turq = vec3(0.03, 0.08, 0.072);
        vec3 deep = vec3(0.018, 0.026, 0.036);
        vec3 body = mix(turq, deep, smoothstep(0.15, 0.9, depth));
        vec3 col = body * (1.0 - F) + sky * F;

        // backlit wave faces: sun through the thin crest, green-turquoise
        float toSun = max(dot(-V, OD_SUN), 0.0);
        float face = clamp(dot(normalize(n.xz + 1e-5), normalize(V.xz + 1e-5)), 0.0, 1.0) * length(n.xz) * 3.0;
        float thick = clamp(crestP / 0.35, 0.0, 1.0);
        // grey-silver / gold through the crest, only the thinnest lip a faint green
        vec3 thru = mix(vec3(0.36, 0.33, 0.27), vec3(0.3, 0.33, 0.29), smoothstep(0.6, 0.15, thick));
        col += OD_SUNCOL * OD_SUN_I * thru * 0.03 * thick * clamp(face, 0.0, 1.0) * (0.25 + toSun * toSun);

        // sun glitter: Beckmann lobe, roughness = sub-pixel wave slopes
        vec3 L = OD_SUN;
        vec3 H = normalize(L + V);
        float nh = max(dot(n, H), 1e-4);
        // wide glitter field far out (unresolved chop tilts facets toward the sun)
        float m2 = 2.0 * (0.0034 + 0.6 * lost + 0.003 * pow(1.0 - nv, 8.0) + 0.009 * smoothstep(10.0, 500.0, dist));
        float nh2 = nh * nh;
        float D = min(exp(-(1.0 - nh2) / (nh2 * m2)) / (3.14159 * m2 * nh2 * nh2), 3000.0);
        float Fh = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        vec3 spec = OD_SUNCOL * OD_SUN_I * D * Fh / (4.0 * nv) * smoothstep(-0.06, 0.06, dot(n, L));
        spec *= vec3(1.0, 0.74, 0.38);
        float gl = odNoise(p / max(fp * 0.6, 0.01) * vec2(1.0, 0.45) + vec2(t * 1.7, -t * 0.6)) * odNoise(p / max(fp * 0.35, 0.006) + vec2(-t * 1.1, t * 0.9) + 5.0) * 1.6;
        float gw = smoothstep(0.0, 0.0012, lost);
        // always broken into sparkles: resolved facets near by, twinkling glints far out
        float gl2 = odNoise(p * vec2(4.5, 11.0) + vec2(t * 1.3, t * 0.4)) * odNoise(p * vec2(9.0, 19.0) - vec2(t * 0.9, 0.0) + 3.0) * 1.8;
        spec *= mix(0.12 + 3.2 * smoothstep(0.3, 0.7, gl2), 0.2 + 3.0 * smoothstep(0.35, 0.8, gl), gw);
        spec *= mix(vec3(1.0), vec3(1.0, 0.85, 0.7), smoothstep(40.0, 600.0, dist));
        float sl = dot(spec, vec3(0.2126, 0.7152, 0.0722));
        spec /= 1.0 + sl / mix(1.8, 4.0, gw);

        // ---- foam ----
        float foamAmt = 0.0;
        float lace = fp < 0.25 ? odFoam(p, t) : 0.55;
        // whitewater bore running in ahead of the swash, and dissolving patches behind
        vec3 fr = surfFront(p.y, t);
        if (fr.x < 1e3) {
          float behind = p.x - fr.x;
          float roller = fr.y * exp(-max(behind, 0.0) / 0.35) * step(-0.05, behind);
          float bore = fr.y * exp(-max(behind, 0.0) / (0.8 + 1.0 * fr.z)) * step(-0.05, behind);
          foamAmt = max(foamAmt, max(roller * mix(0.55, 0.95, clamp(lace * 1.4, 0.0, 1.0)), bore * lace));
          foamAmt = max(foamAmt, 0.35 * (1.0 - fr.y) * smoothstep(0.3, 0.7, lace) * step(0.0, behind) * exp(-behind / 3.0));
        }
        // lingering foam streaks over the surf zone
        float zone = smoothstep(92.05, 93.55, p.x) * (1.0 - smoothstep(97.05, 104.05, p.x));
        vec2 sw = p * 0.32 + vec2(odNoise(p * 0.21 + 4.0), odNoise(p * 0.21 + 9.0)) * 2.4;
        float streak = smoothstep(0.55, 0.8, odNoise(sw + vec2(0.0, t * 0.03)));
        foamAmt = max(foamAmt, zone * streak * smoothstep(0.35, 0.7, lace) * 0.3);
        // crest lip / roller whitewater
        foamAmt = max(foamAmt, smoothstep(0.12, 0.5, whiteF) * mix(0.75, 1.0, lace));
        // thin intersection line where the water meets the sand
        foamAmt = max(foamAmt, (1.0 - smoothstep(0.0, 0.04, depth)) * lace * 0.8);
        // sparse whitecaps far out
        float far = smoothstep(152.5, 212.5, p.x);
        float cap = smoothstep(0.86, 0.93, odNoise(p * 0.045 + vec2(t * 0.02, 0.0))) * smoothstep(0.5, 0.75, odNoise(p * 0.5 - t * 0.1));
        foamAmt = max(foamAmt, far * cap * 0.8 * (1.0 - smoothstep(200.0, 1500.0, dist)));

        // lit foam: sun on bubbly (all-facing) foam + sky fill; very shallow sun, so modest
        vec3 skyUp = odSky(vec3(0.0, 1.0, 0.0), 2.0);
        // white foam (albedo ~0.8): its light is taken as luminance so it stays cream-white
        float foamL = dot(OD_SUNCOL * OD_SUN_I * 0.318 * (0.3 + 0.35 * toSun) + skyUp * 1.1, vec3(0.2126, 0.7152, 0.0722));
        vec3 foamCol = vec3(1.0, 0.97, 0.92) * 0.85 * foamL * 1.6;
        col = mix(col, foamCol, foamAmt);
        spec *= 1.0 - foamAmt;

        // shallow water is clear: the wet sand shows through the last few centimetres
        float alpha = smoothstep(-0.02, 0.03, depth) * mix(0.24, 1.0, smoothstep(0.04, 0.9, depth));
        alpha = mix(alpha, 1.0, F);
        alpha = max(alpha, foamAmt * smoothstep(-0.02, 0.02, depth));
        col += spec;

        // warm marine haze toward the horizon: the far sea melts into the glowing sky
        vec3 hzd = normalize(vec3(-V.x, 0.012, -V.z));
        vec3 hzc = odSky(hzd, 2.0) * vec3(1.0, 0.94, 0.86);
        col = mix(col, hzc, smoothstep(700.0, 9000.0, dist) * 0.85);
        #ifdef USE_FOG
          col = odApplyFog(col, vFogOffset, fogDensity * 0.18);
        #endif
        gl_FragColor = vec4(col, alpha);
      }