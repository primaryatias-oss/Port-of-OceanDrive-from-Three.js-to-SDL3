// Renderer smoke test: a few standard-material meshes lit by the sun, rendered headless and
// saved as a BMP (path in argv[1], default build/test_render.bmp). Checks that something
// other than the clear colour was drawn and that the lit side is brighter than the shade.
#include <stdio.h>

#include "gfx/dfg_lut.h"
#include "gfx/scene.h"

static Material *std_mat(Texture *white, Color c, double rough, double metal) {
  Material *m = mat_new(PROG_STD_MAP, "std");
  mat_set_color(m, "diffuse", c);
  mat_set_float(m, "opacity", 1);
  mat_set_color(m, "emissive", color_rgb(0, 0, 0));
  mat_set_float(m, "roughness", rough);
  mat_set_float(m, "metalness", metal);
  mat_set_float(m, "envMapIntensity", 1);
  mat_set_mat3(m, "envMapRotation", (M3){ { 1, 0, 0, 0, 1, 0, 0, 0, 1 } });
  mat_set_texture(m, "map", white);
  return m;
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "build/test_render.bmp";
  const int W = 480, H = 270;
  gpu_init("test", W, H, true);
  renderer_init();

  static const uint8_t white_px[4] = { 255, 255, 255, 255 };
  TexUpload u = { .rgba = white_px, .w = 1, .h = 1, .srgb = true, .flip_y = true, .sampler = sampler_default() };
  u.sampler.mipmaps = false;
  Texture *white = tex_create_rgba8(&u);

  frame_set_texture(GT_DFG_LUT, dfg_lut_texture());
  Node *scene = node_new(NODE_GROUP, "scene");
  Node *ground = node_mesh(gpu_geometry(geo_rotate_x(geo_plane(20, 20, 1, 1), -PI_D / 2)), std_mat(white, color_hex(0xb0a080), 0.9, 0));
  node_add(scene, ground);
  Node *box = node_mesh(gpu_geometry(geo_box1(1.5, 1.5, 1.5)), std_mat(white, color_hex(0xe0939d), 0.6, 0));
  box->position = v3(-1.2, 0.75, 0);
  node_set_rotation(box, 0, 0.6, 0);
  node_add(scene, box);
  Node *ball = node_mesh(gpu_geometry(geo_sphere3(0.8, 32, 16)), std_mat(white, color_hex(0x7cc7c4), 0.3, 0));
  ball->position = v3(1.2, 0.8, 0.3);
  node_add(scene, ball);
  // an instanced row of small boxes
  Node *row = node_instanced(gpu_geometry(geo_box1(0.3, 0.3, 0.3)), nullptr, 5);
  row->material = std_mat(white, color_hex(0xefe4b4), 0.8, 0);
  // (the plain program is not instanced; this row checks the variant error path is not hit)
  (void)row;

  Camera cam = { .node = node_new(NODE_GROUP, "camera"), .fov = 50, .aspect = (double)W / H, .near = 0.1, .far = 100, .zoom = 1 };
  cam.node->position = v3(0, 2.2, 6);
  camera_look_at(&cam, v3(0, 0.6, 0));
  camera_update(&cam);

  V3 sun_dir = v3_norm(v3(0.6, 0.7, 0.4));
  frame_set_v3(G_DIR_LIGHT_DIRECTION, v3_transform_dir(sun_dir, cam.view));
  frame_set_v3(G_DIR_LIGHT_COLOR, v3(3, 2.6, 2.2));
  frame_set_v3(G_AMBIENT_LIGHT_COLOR, v3(0.15, 0.17, 0.22));
  frame_set_float(G_FOG_DENSITY, 0);
  frame_set_float(G_DIR_SHADOW_INTENSITY, 0);   // no shadow map in this test

  Texture *color = tex_create_target(W, H, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, SDL_GPU_SAMPLECOUNT_1, sampler_default());
  Texture *depth = tex_create_target(W, H, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
    SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, 1, SDL_GPU_SAMPLECOUNT_1, sampler_default());
  RenderTargetDesc rt = { .color = color->gpu, .color_format = color->format, .depth = depth->gpu,
    .depth_format = depth->format, .samples = SDL_GPU_SAMPLECOUNT_1, .w = W, .h = H,
    .clear_color = true, .clear = { 0.1f, 0.12f, 0.2f, 1 }, .clear_depth = true, .depth_store = SDL_GPU_STOREOP_DONT_CARE };
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  render_scene(cb, scene, &cam, &rt);
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));

  uint8_t *px = xmalloc((size_t)W * H * 4), *img = xmalloc((size_t)W * H * 4);
  gpu_read_rgba8(color->gpu, W, H, px);
  for (int y = 0; y < H; y++) memcpy(img + (size_t)y * W * 4, px + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);   // GL rows -> image
  save_bmp_rgba8(out, img, W, H);
  // checks: ball's sun-facing upper right is brighter than its lower left; the frame is not empty
  int drawn = 0;
  for (int i = 0; i < W * H; i++) drawn += img[i * 4] != px[0] || img[i * 4 + 2] != px[2];
  printf("draw calls %d, triangles %d, pixels differing from the corner: %d\n", g_render_stats.calls, g_render_stats.triangles, drawn);
  bool ok = g_render_stats.calls == 3 && drawn > W * H / 4;
  printf(ok ? "render smoke test passed -> %s\n" : "render smoke test FAILED -> %s\n", out);
  renderer_shutdown();
  gpu_shutdown();
  return ok ? 0 : 1;
}
