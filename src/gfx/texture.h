// GPU textures with three.js Texture semantics: wrap / filter / anisotropy, sRGB colour space
// (decoded to linear by the sampler), flipY on upload, mipmaps, and the uv transform
// (offset / repeat / rotation / center) that becomes the *Transform uniform.
#pragma once

#include "core/common.h"
#include "math/vmath.h"

typedef enum Wrap { WRAP_CLAMP, WRAP_REPEAT, WRAP_MIRROR } Wrap;
typedef enum Filter { FILTER_NEAREST, FILTER_LINEAR } Filter;

typedef struct SamplerDesc {
  Wrap wrap_s, wrap_t;
  Filter mag, min;
  bool mipmaps;          // min filter uses mips (LinearMipmapLinear / NearestMipmapNearest...)
  Filter mip;            // mip interpolation
  int anisotropy;        // 1 = off
  bool compare;          // depth comparison (sampler2DShadow)
} SamplerDesc;

typedef struct Texture {
  SDL_GPUTexture *gpu;
  int w, h, layers, levels;
  SDL_GPUTextureFormat format;
  bool cube;
  SamplerDesc sampler;
  // Texture.offset / repeat / rotation / center -> uv transform (updateMatrix)
  V2 offset, repeat, center;
  double rotation;
  int size_bytes;
} Texture;

// Linear-filtered, mipmapped, anisotropy 1, clamp: three's defaults for a CanvasTexture.
SamplerDesc sampler_default(void);

typedef struct TexUpload {
  const uint8_t *rgba;    // w*h*4 bytes, row 0 = top of the image (canvas order)
  int w, h;
  bool srgb;              // colorSpace = SRGBColorSpace
  bool flip_y;            // three: true for canvas/image textures, false for DataTexture
  bool mipmaps;           // generateMipmaps
  SamplerDesc sampler;
} TexUpload;

Texture *tex_create_rgba8(const TexUpload *u);
// raw texel data in GPU order (row 0 = v 0), no mipmaps: DataTextures, LUTs
Texture *tex_create_raw(int w, int h, SDL_GPUTextureFormat fmt, const void *data, size_t bytes, SamplerDesc sampler);
// render target / storage textures (no upload)
Texture *tex_create_target(int w, int h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage,
                           int levels, SDL_GPUSampleCount samples, SamplerDesc sampler);
void tex_destroy(Texture *t);
// replaces the whole (single-level) image from the command buffer's timeline: a copy pass on
// cb, outside any render pass; the texture is cycled if earlier passes still read it
void tex_upload(SDL_GPUCommandBuffer *cb, Texture *t, const void *data, size_t bytes);
// three's Matrix3.setUvTransform(offset, repeat, rotation, center)
M3 tex_uv_transform(const Texture *t);

SDL_GPUSampler *sampler_get(const SamplerDesc *d);   // cached
void samplers_shutdown(void);
