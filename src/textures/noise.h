// Procedural, tileable canvas textures (no image files): port of src/textures/noise.js.
#pragma once

#include "canvas/canvas.h"
#include "gfx/texture.h"

// Uint8ClampedArray store (ToUint8Clamp: round half to even, clamp to 0..255)
static inline uint8_t js_u8clamp(double v) {
  if (!(v > 0)) return 0;   // NaN and <= 0
  if (v >= 255) return 255;
  double f = floor(v), d = v - f;
  if (d < 0.5) return (uint8_t)f;
  if (d > 0.5) return (uint8_t)(f + 1);
  return (uint8_t)((long)f % 2 == 0 ? f : f + 1);
}
// Uint8Array store (ToUint8: truncate, modulo 256)
static inline uint8_t js_u8(double v) {
  if (!isfinite(v)) return 0;
  double m = fmod(trunc(v), 256.0);
  if (m < 0) m += 256;
  return (uint8_t)m;
}

// Tileable value-noise fbm field in [0,1], size x size (Float32Array); caller frees.
float *fbm_field(int size, double seed, int base_cells, int octaves, double gain);

typedef struct NoiseColorOpts {
  int size;                 // 512
  double seed;              // 1
  double colorA[3], colorB[3];   // sRGB 0-255 endpoints
  int base_cells;           // 4
  double speckle;           // 0.08
  double contrast;          // 1.0
  void (*draw)(Canvas *c, int size, void *user);
  void *user;
} NoiseColorOpts;
NoiseColorOpts noise_color_defaults(void);

// Albedo: base colour modulated by fbm and fine speckle (CanvasTexture: sRGB, repeat, aniso 8).
// repeat: toTexture's `repeat` (1).
Texture *noise_color_texture(const NoiseColorOpts *o);
Canvas *noise_color_canvas(const NoiseColorOpts *o);        // the canvas it is made from

// the canvas the texture was made from (for callers that keep drawing) is not kept

typedef struct NoiseNormalOpts { int size; double seed; int base_cells; double strength; int octaves; } NoiseNormalOpts;
NoiseNormalOpts noise_normal_defaults(void);   // 256, 5, 8, 2.0, 4
Texture *noise_normal_texture(const NoiseNormalOpts *o);
uint8_t *noise_normal_data(const NoiseNormalOpts *o);       // its RGBA bytes (caller frees)

// CanvasTexture(canvas) with sRGB colour space (flipY, mipmaps) and the given wrapping
Texture *canvas_texture(const Canvas *c, bool srgb, Wrap wrap, int anisotropy);
