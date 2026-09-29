#include "gfx/texture.h"

#include <stdlib.h>

#include "core/vec.h"
#include "gfx/gpu.h"

SamplerDesc sampler_default(void) {
  return (SamplerDesc){ .wrap_s = WRAP_CLAMP, .wrap_t = WRAP_CLAMP, .mag = FILTER_LINEAR, .min = FILTER_LINEAR,
                        .mipmaps = true, .mip = FILTER_LINEAR, .anisotropy = 1 };
}

static SDL_GPUSamplerAddressMode addr(Wrap w) {
  return w == WRAP_REPEAT ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
       : w == WRAP_MIRROR ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                          : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

typedef struct SamplerEntry { SamplerDesc d; SDL_GPUSampler *s; } SamplerEntry;
static Vec(SamplerEntry) g_samplers;

SDL_GPUSampler *sampler_get(const SamplerDesc *d) {
  for (size_t i = 0; i < g_samplers.len; i++)
    if (!memcmp(&g_samplers.data[i].d, d, sizeof *d)) return g_samplers.data[i].s;
  SDL_GPUSamplerCreateInfo ci = {
    .min_filter = d->min == FILTER_LINEAR ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
    .mag_filter = d->mag == FILTER_LINEAR ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
    .mipmap_mode = d->mip == FILTER_LINEAR ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
    .address_mode_u = addr(d->wrap_s),
    .address_mode_v = addr(d->wrap_t),
    .address_mode_w = addr(d->wrap_t),
    .min_lod = 0,
    .max_lod = d->mipmaps ? 1000.0f : 0.0f,   // no mips: sample level 0 only (GL_LINEAR / GL_NEAREST)
    // (the JS runs on ANGLE, which leaves anisotropic filtering off unless both filters are
    // linear: a Vulkan sampler with anisotropy would also blend the NEAREST magnification)
    .enable_anisotropy = d->anisotropy > 1 && d->mag == FILTER_LINEAR && d->min == FILTER_LINEAR,
    .max_anisotropy = (float)(d->anisotropy > 1 && d->mag == FILTER_LINEAR && d->min == FILTER_LINEAR ? d->anisotropy : 1),
    .enable_compare = d->compare,
    .compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,   // three: LessEqualCompare for shadow maps
  };
  SDL_GPUSampler *s = SDL_CreateGPUSampler(g_gpu.dev, &ci);
  if (!s) FATAL("sampler: %s", SDL_GetError());
  vec_push(&g_samplers, (SamplerEntry){ *d, s });
  return s;
}

void samplers_shutdown(void) {
  for (size_t i = 0; i < g_samplers.len; i++) SDL_ReleaseGPUSampler(g_gpu.dev, g_samplers.data[i].s);
  vec_free(&g_samplers);
}

static int mip_count(int w, int h) {
  int n = 1;
  while (w > 1 || h > 1) { w = imax(1, w / 2); h = imax(1, h / 2); n++; }
  return n;
}

Texture *tex_create_rgba8(const TexUpload *u) {
  CHECK(u->w > 0 && u->h > 0 && u->rgba);
  static int dump = -1;   // OD_TEXHASH=1: print a hash of each uploaded image (reference diffs)
  if (dump < 0) dump = getenv("OD_TEXHASH") != nullptr;
  if (dump) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)u->w * u->h * 4; i++) h = (h ^ u->rgba[i]) * 16777619u;
    printf("tex %dx%d %u\n", u->w, u->h, h);
    static int seq;
    const char *dir = getenv("OD_TEXDUMP");   // also write the raw RGBA bytes there
    if (dir) {
      char path[1024];
      snprintf(path, sizeof path, "%s/tex%03d_%dx%d.raw", dir, seq++, u->w, u->h);
      FILE *f = fopen(path, "wb");
      if (f) { fwrite(u->rgba, 1, (size_t)u->w * u->h * 4, f); fclose(f); }
    }
  }
  Texture *t = xcalloc(1, sizeof *t);
  t->w = u->w;
  t->h = u->h;
  t->layers = 1;
  t->levels = u->mipmaps ? mip_count(u->w, u->h) : 1;
  t->format = u->srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  t->sampler = u->sampler;
  t->sampler.mipmaps = u->mipmaps && u->sampler.mipmaps;
  t->repeat = v2(1, 1);
  t->size_bytes = (int)((double)u->w * u->h * 4 * (u->mipmaps ? 4.0 / 3.0 : 1.0));
  SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (u->mipmaps ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
  t->gpu = SDL_CreateGPUTexture(g_gpu.dev, &(SDL_GPUTextureCreateInfo){
    .type = SDL_GPU_TEXTURETYPE_2D, .format = t->format, .usage = usage,
    .width = (uint32_t)u->w, .height = (uint32_t)u->h, .layer_count_or_depth = 1, .num_levels = (uint32_t)t->levels });
  if (!t->gpu) FATAL("texture %dx%d: %s", u->w, u->h, SDL_GetError());

  uint32_t bytes = (uint32_t)u->w * (uint32_t)u->h * 4u;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes });
  if (!tb) FATAL("upload buffer: %s", SDL_GetError());
  uint8_t *dst = SDL_MapGPUTransferBuffer(g_gpu.dev, tb, false);
  // GPU memory is in OpenGL orientation (row 0 = v 0). UNPACK_FLIP_Y puts the image's top
  // row at v = 1, i.e. the rows go in bottom-first.
  size_t row = (size_t)u->w * 4;
  for (int y = 0; y < u->h; y++) {
    int src_y = u->flip_y ? u->h - 1 - y : y;
    memcpy(dst + (size_t)y * row, u->rgba + (size_t)src_y * row, row);
  }
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, tb);
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  SDL_UploadToGPUTexture(cp, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb },
    &(SDL_GPUTextureRegion){ .texture = t->gpu, .w = (uint32_t)u->w, .h = (uint32_t)u->h, .d = 1 }, false);
  SDL_EndGPUCopyPass(cp);
  if (t->levels > 1) SDL_GenerateMipmapsForGPUTexture(cb, t->gpu);
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));
  SDL_ReleaseGPUTransferBuffer(g_gpu.dev, tb);
  return t;
}

Texture *tex_create_raw(int w, int h, SDL_GPUTextureFormat fmt, const void *data, size_t bytes, SamplerDesc sampler) {
  Texture *t = tex_create_target(w, h, fmt, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, SDL_GPU_SAMPLECOUNT_1, sampler);
  t->size_bytes = (int)bytes;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (uint32_t)bytes });
  if (!tb) FATAL("upload buffer: %s", SDL_GetError());
  memcpy(SDL_MapGPUTransferBuffer(g_gpu.dev, tb, false), data, bytes);
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, tb);
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  SDL_UploadToGPUTexture(cp, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb },
    &(SDL_GPUTextureRegion){ .texture = t->gpu, .w = (uint32_t)w, .h = (uint32_t)h, .d = 1 }, false);
  SDL_EndGPUCopyPass(cp);
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));
  SDL_ReleaseGPUTransferBuffer(g_gpu.dev, tb);
  return t;
}

void tex_upload(SDL_GPUCommandBuffer *cb, Texture *t, const void *data, size_t bytes) {
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (uint32_t)bytes });
  if (!tb) FATAL("upload buffer: %s", SDL_GetError());
  memcpy(SDL_MapGPUTransferBuffer(g_gpu.dev, tb, false), data, bytes);
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, tb);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  SDL_UploadToGPUTexture(cp, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb },
    &(SDL_GPUTextureRegion){ .texture = t->gpu, .w = (uint32_t)t->w, .h = (uint32_t)t->h, .d = 1 }, true);
  SDL_EndGPUCopyPass(cp);
  SDL_ReleaseGPUTransferBuffer(g_gpu.dev, tb);   // (released once the command buffer is done with it)
}

Texture *tex_create_target(int w, int h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage,
                           int levels, SDL_GPUSampleCount samples, SamplerDesc sampler) {
  Texture *t = xcalloc(1, sizeof *t);
  t->w = w;
  t->h = h;
  t->layers = 1;
  t->levels = levels;
  t->format = fmt;
  t->sampler = sampler;
  t->repeat = v2(1, 1);
  t->gpu = SDL_CreateGPUTexture(g_gpu.dev, &(SDL_GPUTextureCreateInfo){
    .type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = usage, .width = (uint32_t)w, .height = (uint32_t)h,
    .layer_count_or_depth = 1, .num_levels = (uint32_t)levels, .sample_count = samples });
  if (!t->gpu) FATAL("render target %dx%d: %s", w, h, SDL_GetError());
  return t;
}

void tex_destroy(Texture *t) {
  if (!t) return;
  SDL_ReleaseGPUTexture(g_gpu.dev, t->gpu);
  free(t);
}

M3 tex_uv_transform(const Texture *t) {
  double c = cos(t->rotation), s = sin(t->rotation);
  double sx = t->repeat.x, sy = t->repeat.y, cx = t->center.x, cy = t->center.y;
  // Matrix3.set() is row-major; stored column-major
  return (M3){ {
    sx * c, -sy * s, 0,
    sx * s, sy * c, 0,
    -sx * (c * cx + s * cy) + cx + t->offset.x, -sy * (-s * cx + c * cy) + cy + t->offset.y, 1,
  } };
}
