
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
