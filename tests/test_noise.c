// Checksums of the procedural noise textures (textures/noise.c) for comparison with the JS
// (tools/ref/noise-ref.mjs prints the same lines from src/textures/noise.js in the browser).
#include <stdlib.h>

#include "core/vec.h"
#include "textures/noise.h"

static void report(const char *name, const uint8_t *px, size_t n) {
  uint32_t h = 2166136261u;
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i++) { h = (h ^ px[i]) * 16777619u; sum += px[i]; }
  printf("%s %llu %u\n", name, (unsigned long long)sum, h);
}

int main(void) {
  // the hotels' stucco albedo and grain
  NoiseColorOpts co = noise_color_defaults();
  co.size = 512; co.seed = 91; co.base_cells = 3; co.speckle = 0.004; co.contrast = 1.0;
  co.colorA[0] = 242; co.colorA[1] = 240; co.colorA[2] = 236;
  co.colorB[0] = 255; co.colorB[1] = 255; co.colorB[2] = 255;
  Canvas *c = noise_color_canvas(&co);
  uint8_t *px = xmalloc(512 * 512 * 4);
  cv_get_image_data(c, 0, 0, 512, 512, px);
  report("color91", px, 512 * 512 * 4);
  free(px);
  canvas_free(c);
  NoiseNormalOpts no = { 512, 17, 48, 0.11, 3 };
  uint8_t *d = noise_normal_data(&no);
  report("normal17", d, 512 * 512 * 4);
  free(d);
  NoiseNormalOpts nd = noise_normal_defaults();
  d = noise_normal_data(&nd);
  report("normal_default", d, (size_t)nd.size * nd.size * 4);
  free(d);
  return 0;
}
