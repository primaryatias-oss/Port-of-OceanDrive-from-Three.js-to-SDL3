#include "textures/noise.h"

#include <stdlib.h>

#include "core/vec.h"

float *fbm_field(int size, double seed, int base_cells, int octaves, double gain) {
  Rng rnd = rng_make(seed);
  float *out = xcalloc((size_t)size * (size_t)size, sizeof(float));
  double amp = 1, total = 0;
  for (int o = 0; o < octaves; o++) {
    int cells = base_cells << o;
    float *lat = xmalloc((size_t)cells * (size_t)cells * sizeof(float));
    for (int i = 0; i < cells * cells; i++) lat[i] = (float)rng_next(&rnd);
    for (int y = 0; y < size; y++) {
      double fy = ((double)y / size) * cells;
      double y0 = floor(fy), ty = fy - y0;
      double sy = ty * ty * (3 - 2 * ty);
      int r0 = ((int)y0 % cells) * cells, r1 = (((int)y0 + 1) % cells) * cells;
      for (int x = 0; x < size; x++) {
        double fx = ((double)x / size) * cells;
        double x0 = floor(fx), tx = fx - x0;
        double sx = tx * tx * (3 - 2 * tx);
        int c0 = (int)x0 % cells, c1 = ((int)x0 + 1) % cells;
        double a = lat[r0 + c0] + ((double)lat[r0 + c1] - lat[r0 + c0]) * sx;
        double b = lat[r1 + c0] + ((double)lat[r1 + c1] - lat[r1 + c0]) * sx;
        size_t k = (size_t)y * size + x;
        out[k] = (float)(out[k] + (a + (b - a) * sy) * amp);
      }
    }
    free(lat);
    total += amp;
    amp *= gain;
  }
  for (size_t i = 0; i < (size_t)size * size; i++) out[i] = (float)(out[i] / total);
  return out;
}

NoiseColorOpts noise_color_defaults(void) {
  return (NoiseColorOpts){ .size = 512, .seed = 1, .base_cells = 4, .speckle = 0.08, .contrast = 1.0 };
}

Texture *canvas_texture(const Canvas *c, bool srgb, Wrap wrap, int anisotropy) {
  int w = canvas_width(c), h = canvas_height(c);
  uint8_t *px = xmalloc((size_t)w * h * 4);
  cv_get_image_data(c, 0, 0, w, h, px);
  SamplerDesc s = sampler_default();
  s.wrap_s = s.wrap_t = wrap;
  s.anisotropy = anisotropy;
  TexUpload u = { .rgba = px, .w = w, .h = h, .srgb = srgb, .flip_y = true, .mipmaps = true, .sampler = s };
  Texture *t = tex_create_rgba8(&u);
  free(px);
  return t;
}

Canvas *noise_color_canvas(const NoiseColorOpts *o) {
  int size = o->size;
  float *field = fbm_field(size, o->seed, o->base_cells, 5, 0.5);
  Rng rnd = rng_make(o->seed * 7 + 3);
  Canvas *c = canvas_new(size, size);
  uint8_t *img = xmalloc((size_t)size * size * 4);   // createImageData: a Uint8ClampedArray
  for (int i = 0; i < size * size; i++) {
    double t = (field[i] - 0.5) * o->contrast + 0.5;
    t = fmin(1, fmax(0, t));
    double s = 1 + (rng_next(&rnd) - 0.5) * o->speckle * 2;
    for (int ch = 0; ch < 3; ch++) img[i * 4 + ch] = js_u8clamp(fmin(255, (o->colorA[ch] + (o->colorB[ch] - o->colorA[ch]) * t) * s));
    img[i * 4 + 3] = 255;
  }
  cv_put_image_data(c, img, 0, 0, size, size);
  free(img);
  free(field);
  if (o->draw) o->draw(c, size, o->user);
  return c;
}

Texture *noise_color_texture(const NoiseColorOpts *o) {
  Canvas *c = noise_color_canvas(o);
  Texture *tex = canvas_texture(c, true, WRAP_REPEAT, 8);
  canvas_free(c);
  return tex;
}

NoiseNormalOpts noise_normal_defaults(void) { return (NoiseNormalOpts){ 256, 5, 8, 2.0, 4 }; }

uint8_t *noise_normal_data(const NoiseNormalOpts *o) {
  int size = o->size;
  float *h = fbm_field(size, o->seed, o->base_cells, o->octaves, 0.5);
  uint8_t *data = xmalloc((size_t)size * size * 4);   // Uint8Array
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      double l = h[y * size + ((x - 1 + size) % size)], r = h[y * size + ((x + 1) % size)];
      double d = h[((y - 1 + size) % size) * size + x], u = h[((y + 1) % size) * size + x];
      double nx = (l - r) * o->strength * size * 0.05, ny = (d - u) * o->strength * size * 0.05, nz = 1;
      double len = js_hypot3(nx, ny, nz);
      size_t i = ((size_t)y * size + x) * 4;
      data[i] = js_u8(((nx / len) * 0.5 + 0.5) * 255);
      data[i + 1] = js_u8(((ny / len) * 0.5 + 0.5) * 255);
      data[i + 2] = js_u8(((nz / len) * 0.5 + 0.5) * 255);
      data[i + 3] = 255;
    }
  free(h);
  return data;
}

Texture *noise_normal_texture(const NoiseNormalOpts *o) {
  int size = o->size;
  uint8_t *data = noise_normal_data(o);
  // DataTexture: flipY false, linear, RepeatWrapping, LinearMipmapLinear, anisotropy 8
  SamplerDesc s = sampler_default();
  s.wrap_s = s.wrap_t = WRAP_REPEAT;
  s.anisotropy = 8;
  TexUpload up = { .rgba = data, .w = size, .h = size, .srgb = false, .flip_y = false, .mipmaps = true, .sampler = s };
  Texture *t = tex_create_rgba8(&up);
  free(data);
  return t;
}
