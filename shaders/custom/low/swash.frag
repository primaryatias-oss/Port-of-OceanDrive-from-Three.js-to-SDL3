
      varying vec3 vWorld;
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
  for (int i = 0; i < 3; i++) { s += a * odNoise(p); p = r * p * 2.03 + 17.1; a *= 0.5; }
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
        float t = uSurfT;
        vec2 p = vWorld.xz;
        vec3 fr = surfFront(p.y, t);
        float s = p.x - fr.x;
        if (fr.x > 1e3 || s < -0.02) discard;
        vec3 toCam = cameraPosition - vWorld;
        float dist = length(toCam);
        vec3 V = toCam / dist;
        float fresh = fr.y;
        float depth = min(0.12, 0.012 + max(s, 0.0) * 0.022) * (0.35 + 0.65 * fresh);
        // flowing ripples: uprush toward the land, backwash seaward
        float dir = fresh > 0.999 ? -1.0 : 1.0;
        vec2 q = p * 1.2 + vec2(odNoise(p * 0.7 + 1.0), odNoise(p * 0.7 + 5.0)) * 1.8 + vec2(dir * t * 1.2, 0.0);
        float e = 0.05;
        float h0 = odNoise(q * 2.0), hx = odNoise((q + vec2(e, 0.0)) * 2.0), hz = odNoise((q + vec2(0.0, e)) * 2.0);
        vec3 n = normalize(vec3(-(hx - h0) / e * 0.03, 1.0, -(hz - h0) / e * 0.03));
        float nv = max(dot(n, V), 0.01);
        float F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
        vec3 R = reflect(-V, n);
        R.y = abs(R.y) + 0.02;
        vec3 sky = odSky(normalize(R), 2.0);
        // glint of the low sun on the sheet
        vec3 H = normalize(OD_SUN + V);
        float nh = max(dot(n, H), 0.0);
        float spec = pow(nh, 400.0) * 5.0 * smoothstep(0.1, 0.5, s);
        vec3 col = sky * F * 0.8 + vec3(0.012, 0.02, 0.02) * (1.0 - F);
        float alpha = clamp(0.12 + 0.25 * smoothstep(0.0, 0.08, depth) + F * 0.6, 0.0, 0.9);
        // foam: bright lace at the leading edge, bubble trails behind, fading as it drains
        float lace = odFoam(p, t);
        float se = s + (odNoise(p * vec2(0.9, 2.3) + 3.0) - 0.5) * 0.3 + (odNoise(p * 6.0) - 0.5) * 0.08;
        float edge = exp(-max(se, 0.0) / (0.12 + 0.2 * fresh)) * smoothstep(-0.1, 0.06, se);
        // lacy leading edge, thin bubble trails behind it
        vec2 tq = p * 1.4 + vec2(odNoise(p * 0.6 + 2.0), odNoise(p * 0.6 + 6.0)) * 2.2;
        float trail = smoothstep(0.62, 0.9, odNoise(tq));
        float foam = max(edge * mix(0.3, 1.0, clamp(lace * 1.5, 0.0, 1.0)) * 0.9, max(lace, trail * 0.6) * exp(-max(s, 0.0) / 0.9) * 0.55);
        foam *= 0.35 + 0.65 * fresh;
        vec3 skyUp = odSky(vec3(0.0, 1.0, 0.0), 2.0);
        float foamL = dot(OD_SUNCOL * OD_SUN_I * 0.318 * 0.35 + skyUp * 1.1, vec3(0.2126, 0.7152, 0.0722));
        vec3 foamCol = vec3(1.0, 0.97, 0.92) * 0.85 * foamL * 1.6;
        col = mix(col, foamCol, foam);
        col += OD_SUNCOL * OD_SUN_I * vec3(1.0, 0.7, 0.4) * spec * (1.0 - foam) * 0.02;
        alpha = max(alpha * (1.0 - foam), foam);
        // a draining sheet thins out to nothing at its edge (the uprush keeps its foam line)
        alpha *= mix(smoothstep(-0.02, 0.18, s), 1.0, fresh) * smoothstep(-0.1, 0.04, se);
        // hand over to the sea past the shoreline
        alpha *= 1.0 - smoothstep(SURF_SHORE_X + 0.4, SURF_SHORE_X + 1.6, p.x);
        #ifdef USE_FOG
          col = odApplyFog(col, vFogOffset, fogDensity * 0.3);
        #endif
        gl_FragColor = vec4(col, alpha);
      }