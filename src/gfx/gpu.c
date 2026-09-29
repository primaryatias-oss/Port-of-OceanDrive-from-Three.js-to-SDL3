#include "gfx/gpu.h"

Gpu g_gpu;

void gpu_init(const char *title, int w, int h, bool headless) {
  SDL_CHECK(SDL_Init(SDL_INIT_VIDEO));   // SDL_GPU needs the video subsystem even headless
  bool debug = SDL_GetHintBoolean("OCEAN_GPU_DEBUG", false) || SDL_getenv("OCEAN_GPU_DEBUG") != nullptr;
  g_gpu.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, nullptr);
  if (!g_gpu.dev) FATAL("no SPIR-V capable GPU device (Vulkan): %s", SDL_GetError());
  g_gpu.driver = SDL_GetGPUDeviceDriver(g_gpu.dev);
  LOG("GPU driver: %s%s", g_gpu.driver, debug ? " (debug layers)" : "");

  if (headless) {
    g_gpu.swap_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    return;
  }
  g_gpu.win = SDL_CreateWindow(title, w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!g_gpu.win) FATAL("SDL_CreateWindow: %s", SDL_GetError());
  SDL_CHECK(SDL_ClaimWindowForGPUDevice(g_gpu.dev, g_gpu.win));
  // SDR sRGB-nonlinear swapchain: the post chain writes display-referred values itself
  g_gpu.swap_format = SDL_GetGPUSwapchainTextureFormat(g_gpu.dev, g_gpu.win);
}

void gpu_recreate(void) {
  // (the lost device is released from the window and left alone: its objects die with the process)
  bool debug = SDL_GetHintBoolean("OCEAN_GPU_DEBUG", false) || SDL_getenv("OCEAN_GPU_DEBUG") != nullptr;
  SDL_ReleaseWindowFromGPUDevice(g_gpu.dev, g_gpu.win);
  g_gpu.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, nullptr);
  if (!g_gpu.dev) FATAL("no GPU device after the reset: %s", SDL_GetError());
  SDL_CHECK(SDL_ClaimWindowForGPUDevice(g_gpu.dev, g_gpu.win));
  g_gpu.swap_format = SDL_GetGPUSwapchainTextureFormat(g_gpu.dev, g_gpu.win);
}

void gpu_shutdown(void) {
  if (g_gpu.win) {
    SDL_ReleaseWindowFromGPUDevice(g_gpu.dev, g_gpu.win);
    SDL_DestroyWindow(g_gpu.win);
  }
  SDL_DestroyGPUDevice(g_gpu.dev);
  SDL_Quit();
  g_gpu = (Gpu){};
}

SDL_GPUShader *gpu_shader(ShaderId id) {
  CHECK(id >= 0 && id < SH_COUNT);
  const ShaderBlob *b = &SHADERS[id];
  CHECK(b->stage != SHADER_COMPUTE);
  SDL_GPUShaderCreateInfo ci = {
    .code_size = b->size,
    .code = b->code,
    .entrypoint = "main",
    .format = SDL_GPU_SHADERFORMAT_SPIRV,
    .stage = b->stage == SHADER_VERTEX ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT,
    .num_samplers = b->samplers,
    .num_storage_textures = b->storage_textures,
    .num_storage_buffers = b->storage_buffers,
    .num_uniform_buffers = b->uniform_buffers,
  };
  SDL_GPUShader *s = SDL_CreateGPUShader(g_gpu.dev, &ci);
  if (!s) FATAL("shader %s: %s", b->name, SDL_GetError());
  return s;
}

void gpu_read_rgba8(SDL_GPUTexture *tex, int w, int h, uint8_t *out) {
  uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = bytes });
  if (!tb) FATAL("download buffer: %s", SDL_GetError());
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  if (!cb) FATAL("command buffer: %s", SDL_GetError());
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  SDL_DownloadFromGPUTexture(cp,
    &(SDL_GPUTextureRegion){ .texture = tex, .w = (uint32_t)w, .h = (uint32_t)h, .d = 1 },
    &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb, .pixels_per_row = (uint32_t)w, .rows_per_layer = (uint32_t)h });
  SDL_EndGPUCopyPass(cp);
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
  if (!fence) FATAL("submit: %s", SDL_GetError());
  SDL_CHECK(SDL_WaitForGPUFences(g_gpu.dev, true, &fence, 1));
  SDL_ReleaseGPUFence(g_gpu.dev, fence);
  const uint8_t *src = SDL_MapGPUTransferBuffer(g_gpu.dev, tb, false);
  if (!src) FATAL("map download buffer: %s", SDL_GetError());
  memcpy(out, src, bytes);
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, tb);
  SDL_ReleaseGPUTransferBuffer(g_gpu.dev, tb);
}

bool save_bmp_rgba8(const char *path, const uint8_t *px, int w, int h) {
  SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void *)px, w * 4);
  if (!s) return false;
  bool ok = SDL_SaveBMP(s, path);
  SDL_DestroySurface(s);
  return ok;
}
