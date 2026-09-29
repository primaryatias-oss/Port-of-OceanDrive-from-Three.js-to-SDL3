
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
