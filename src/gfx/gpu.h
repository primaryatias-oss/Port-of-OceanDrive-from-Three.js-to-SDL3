// SDL_GPU device ownership and small helpers shared by every render module.
#pragma once

#include "core/common.h"
#include "shaders.h"

typedef struct Gpu {
  SDL_GPUDevice *dev;
  SDL_Window *win;                   // null in headless (--shot) runs
  SDL_GPUTextureFormat swap_format;  // swapchain format, or the offscreen format when headless
  const char *driver;
} Gpu;

extern Gpu g_gpu;

// headless: no window or swapchain; everything renders to offscreen textures.
void gpu_init(const char *title, int w, int h, bool headless);
// after a device loss: a new device for the same window (the old one is abandoned)
void gpu_recreate(void);
void gpu_shutdown(void);

// Creates the SDL shader for a packed SPIR-V blob; resource counts come from build-time reflection.
SDL_GPUShader *gpu_shader(ShaderId id);

// Copies an RGBA8 texture back to the CPU (blocking) into `out` (w*h*4 bytes).
void gpu_read_rgba8(SDL_GPUTexture *tex, int w, int h, uint8_t *out);

// Writes w*h RGBA8 pixels to a .bmp file.
bool save_bmp_rgba8(const char *path, const uint8_t *px, int w, int h);
