
    uniform sampler2D tDiffuse;
    uniform float uVignette, uContrast;
    uniform vec2 uResolution;
    varying vec2 vUv;
    float hash(vec2 p) {
      vec3 q = fract(vec3(p.xyx) * 0.1031);
      q += dot(q, q.yzx + 33.33);
      return fract((q.x + q.y) * q.z);
    }
    void main() {
      vec3 c = texture2D(tDiffuse, vUv).rgb;
      // gentle print S-curve: deeper blacks in backlit areas, a little more mid contrast
      c = max(c - 0.012, 0.0) / 0.988;
      c = mix(c, c * c * (3.0 - 2.0 * c), uContrast);
      vec2 p = (vUv - 0.5) * vec2(uResolution.x / uResolution.y, 1.0) / 1.02;
      c *= 1.0 - uVignette * smoothstep(0.1, 1.0, dot(p, p));
      vec2 px = floor(vUv * uResolution);
      c += (hash(px) + hash(px + 71.0) - 1.0) / 255.0;
      gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
    }