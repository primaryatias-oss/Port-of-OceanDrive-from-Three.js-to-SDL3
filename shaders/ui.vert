#version 450
// UI layer quad (ui.c): a triangle strip over a rectangle given in NDC (y up), sampling a
// sub-rectangle of the layer's texture (row 0 = top).
layout(set = 1, binding = 0, std140) uniform VertBlock {
  vec4 rect;   // x0, y0 (top left), x1, y1 (bottom right), NDC
  vec4 uv;     // u0, v0, u1, v1
};
layout(location = 0) out vec2 vUv;
void main() {
  vec2 k = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
  vUv = mix(uv.xy, uv.zw, k);
  gl_Position = vec4(mix(rect.xy, rect.zw, k), 0.0, 1.0);
}
