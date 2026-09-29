#version 450
// UI layer: a premultiplied RGBA8 canvas composited source-over (blend ONE, ONE_MINUS_SRC_ALPHA)
// in sRGB values like the browser, scaled by the layer opacity; `gain` brightens (CSS
// filter: brightness). Mode 1 draws the overlay's elliptical vignette instead
// (radial-gradient(ellipse at cx cy, transparent a0, color a1)).
layout(set = 2, binding = 0) uniform sampler2D map;
layout(set = 3, binding = 0, std140) uniform FragBlock {
  vec4 params;   // opacity, gain, mode, unused
  vec4 ell;      // vignette centre (px, y down), radii (px)
  vec4 color;    // vignette colour (straight alpha)
  vec4 stops;    // vignette stops a0, a1
};
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;
void main() {
  if (params.z > 0.5) {
    float t = length((gl_FragCoord.xy - ell.xy) / ell.zw);
    float a = color.a * clamp((t - stops.x) / (stops.y - stops.x), 0.0, 1.0) * params.x;
    fragColor = vec4(color.rgb * a, a);
    return;
  }
  vec4 c = texture(map, vUv);
  fragColor = vec4(min(c.rgb * params.y, vec3(c.a)), c.a) * params.x;
}
