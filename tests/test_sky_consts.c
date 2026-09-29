// sky.js fitShadow() numbers vs the JS values extracted by tools/ref/extract-sky-glsl.mjs.
#include <stdio.h>

#include "world/layout.h"
#include "world/sky.h"

int main(void) {
  layout_init();
  gpu_init("t", 64, 64, true);
  renderer_init();
  frame_set_texture(GT_DFG_LUT, nullptr);
  int ms[2] = { 4096, 1024 };
  Node *scene = node_new(NODE_GROUP, "scene");
  Sky *s = sky_create(scene, ms, 0.5);
  const Camera *c = &s->shadow_cam;
  const double js[6] = { -92.50282627666418, 92.5028262766641, 26.80700510496417, -26.80700510496417, 247.072346919396, 497.9276530806037 };
  const double cv[6] = { c->left, c->right, c->top, c->bottom, c->near, c->far };
  int bad = 0;
  for (int i = 0; i < 6; i++) {
    printf("%.17g %.17g\n", cv[i], js[i]);
    if (fabs(cv[i] - js[i]) > 1e-9 * fabs(js[i])) bad++;
  }
  printf("SHORE_X %.17g BREAK_X %.17g WET_LINE_X %.17g\n", SHORE_X, BREAK_X, WET_LINE_X);
  printf(bad ? "sky constants MISMATCH\n" : "sky constants match JS\n");
  return bad != 0;
}
