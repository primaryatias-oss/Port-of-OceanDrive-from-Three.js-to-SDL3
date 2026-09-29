// PMREMGenerator.fromScene(scene, sigma = 0, near, far), three.js r186: the scene is rendered
// into the six faces of a cube-UV atlas (768 x 1024 RGBA16F for a 256 cube), then each lower
// LOD is GGX-prefiltered from the one above it through a ping-pong target.
#include "gfx/pmrem.h"

enum { LOD_MIN = 4, EXTRA_LODS = 6 };

typedef struct Pmrem {
  int lod_max, cube_size;
  int nlods;
  int size_lods[16];
  GpuGeometry *lod_mesh[16];
} Pmrem;

// _createPlanes(lodMax)
static void create_planes(Pmrem *p) {
  int lod = p->lod_max;
  p->nlods = p->lod_max - LOD_MIN + 1 + EXTRA_LODS;
  CHECK(p->nlods <= 16);
  for (int i = 0; i < p->nlods; i++) {
    int size_lod = 1 << lod;
    p->size_lods[i] = size_lod;
    double texel = 1.0 / (size_lod - 2);
    double mn = -texel, mx = 1 + texel;
    const double uv1[12] = { mn, mn, mx, mn, mx, mx, mn, mn, mx, mx, mn, mx };
    float pos[3 * 6 * 6], dir[3 * 6 * 6];
    for (int face = 0; face < 6; face++) {
      double x = (face % 3) * 2.0 / 3 - 1;
      double y = face > 2 ? 0 : -1;
      const double c[18] = { x, y, 0, x + 2.0 / 3, y, 0, x + 2.0 / 3, y + 1, 0, x, y, 0, x + 2.0 / 3, y + 1, 0, x, y + 1, 0 };
      for (int k = 0; k < 18; k++) pos[face * 18 + k] = (float)c[k];
      for (int v = 0; v < 6; v++) {
        double u = uv1[v * 2] * 2 - 1, w = uv1[v * 2 + 1] * 2 - 1;
        V3 d;
        if (face == 0) d = v3(1, w, u);
        else if (face == 1) d = v3(-u, 1, -w);
        else if (face == 2) d = v3(-u, w, 1);
        else if (face == 3) d = v3(-1, w, -u);
        else if (face == 4) d = v3(-u, -1, w);
        else d = v3(u, w, -1);
        float *o = &dir[(face * 6 + v) * 3];
        o[0] = (float)d.x; o[1] = (float)d.y; o[2] = (float)d.z;
      }
    }
    Geometry *g = geo_new();
    geo_set_attr_copy(g, "position", 3, 36, pos);
    geo_set_attr_copy(g, "outputDirection", 3, 36, dir);
    p->lod_mesh[i] = gpu_geometry(g);
    geo_free(g);
    if (lod > LOD_MIN) lod--;
  }
}

static Texture *cube_uv_target(int w, int h) {
  SamplerDesc s = sampler_default();   // LinearFilter, no mipmaps, clamp
  s.mipmaps = false;
  return tex_create_target(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, SDL_GPU_SAMPLECOUNT_1, s);
}

// WebGL initializes new textures to zero; SDL_GPU leaves them undefined
static void clear_target(SDL_GPUCommandBuffer *cb, Texture *t) {
  SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &(SDL_GPUColorTargetInfo){
    .texture = t->gpu, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE }, 1, nullptr);
  SDL_EndGPURenderPass(rp);
}

static RenderTargetDesc target_desc(Texture *t, Texture *depth, int x, int y, int w, int h) {
  return (RenderTargetDesc){
    .color = t->gpu, .color_format = t->format, .depth = depth ? depth->gpu : nullptr,
    .depth_format = depth ? depth->format : SDL_GPU_TEXTUREFORMAT_INVALID, .samples = SDL_GPU_SAMPLECOUNT_1,
    .w = t->w, .h = t->h, .depth_store = SDL_GPU_STOREOP_STORE,
    .viewport_set = true, .vp_x = x, .vp_y = y, .vp_w = w, .vp_h = h,
    .scissor_set = true, .sc_x = x, .sc_y = y, .sc_w = w, .sc_h = h,
  };
}

Texture *pmrem_from_scene(Node *scene, double near, double far, int size) {
  Pmrem p = {};
  p.lod_max = (int)floor(log2(size));
  p.cube_size = 1 << p.lod_max;
  int width = 3 * imax(p.cube_size, 16 * 7), height = 4 * p.cube_size;
  create_planes(&p);
  Texture *cube_uv = cube_uv_target(width, height);
  Texture *ping = cube_uv_target(width, height);
  // cubeUVRenderTarget.depthBuffer = true (the faces are rendered with depth testing)
  Texture *depth = tex_create_target(width, height, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
    SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, 1, SDL_GPU_SAMPLECOUNT_1, sampler_default());

  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  clear_target(cb, cube_uv);
  clear_target(cb, ping);
  {
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, nullptr, 0, &(SDL_GPUDepthStencilTargetInfo){
      .texture = depth->gpu, .clear_depth = 1, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE,
      .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE, .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE });
    SDL_EndGPURenderPass(rp);
  }

  // _sceneToCubeUV: background box (renderer clear colour: black) then the scene, per face
  Material *bg = mat_new(PROG_PMREM_BACKGROUND, "PMREM.Background");
  bg->side = SIDE_BACK;
  bg->depth_write = bg->depth_test = false;
  mat_set_color(bg, "diffuse", color_rgb(0, 0, 0));
  mat_set_float(bg, "opacity", 1);
  Geometry *bg_geo = geo_box1(1, 1, 1);
  Node *box = node_mesh(gpu_geometry(bg_geo), bg);
  geo_free(bg_geo);
  Node *box_scene = node_new(NODE_GROUP, "pmrem-bg");   // (renderer.render(backgroundBox, cam))
  node_add(box_scene, box);
  Camera cam = { .node = node_new(NODE_GROUP, "cubeCamera"), .fov = 90, .aspect = 1, .near = near, .far = far, .zoom = 1 };
  static const int up_sign[6] = { 1, -1, 1, 1, 1, 1 }, fwd_sign[6] = { 1, 1, 1, -1, -1, -1 };
  for (int i = 0; i < 6; i++) {
    int col = i % 3;
    cam.node->position = v3s(0);
    if (col == 0) { cam.up = v3(0, up_sign[i], 0); camera_look_at(&cam, v3(fwd_sign[i], 0, 0)); }
    else if (col == 1) { cam.up = v3(0, 0, up_sign[i]); camera_look_at(&cam, v3(0, fwd_sign[i], 0)); }
    else { cam.up = v3(0, up_sign[i], 0); camera_look_at(&cam, v3(0, 0, fwd_sign[i])); }
    int s = p.cube_size;
    RenderTargetDesc rt = target_desc(cube_uv, depth, col * s, i > 2 ? s : 0, s, s);
    render_scene(cb, box_scene, &cam, &rt);
    render_scene(cb, scene, &cam, &rt);
  }

  // _applyPMREM: GGX filter from each LOD into the next
  Material *ggx = mat_new(PROG_PMREM_GGX, "PMREMGGXConvolution");
  ggx->blending = BLEND_NONE;
  ggx->depth_test = ggx->depth_write = false;
  Node *flat_scene = node_new(NODE_GROUP, "pmrem-flat");
  Node *mesh = node_mesh(p.lod_mesh[0], ggx);
  mesh->frustum_culled = true;
  node_add(flat_scene, mesh);
  Camera flat = { .node = node_new(NODE_GROUP, "flatCamera"), .ortho = true, .left = -1, .right = 1, .top = 1,
                  .bottom = -1, .near = 0.1, .far = 2000, .zoom = 1 };   // new OrthographicCamera()
  mesh->frustum_culled = false;   // (the planes' bounds lie in the camera's near plane at z = 0)
  for (int lod_out = 1; lod_out < p.nlods; lod_out++) {
    int lod_in = lod_out - 1;
    double target_r = (double)lod_out / (p.nlods - 1), source_r = (double)lod_in / (p.nlods - 1);
    double incremental = sqrt(target_r * target_r - source_r * source_r);
    double blur_strength = target_r * 1.25;
    double adjusted = incremental * blur_strength;
    int out_size = p.size_lods[lod_out];
    int x = 3 * out_size * (lod_out > p.lod_max - LOD_MIN ? lod_out - p.lod_max + LOD_MIN : 0);
    int y = 4 * (p.cube_size - out_size);
    mesh->geo = p.lod_mesh[lod_out];
    mat_set_texture(ggx, "envMap", cube_uv);
    mat_set_float(ggx, "roughness", adjusted);
    mat_set_float(ggx, "mipInt", p.lod_max - lod_in);
    RenderTargetDesc rt = target_desc(ping, nullptr, x, y, 3 * out_size, 2 * out_size);
    render_scene(cb, flat_scene, &flat, &rt);
    mat_set_texture(ggx, "envMap", ping);
    mat_set_float(ggx, "roughness", 0.0);
    mat_set_float(ggx, "mipInt", p.lod_max - lod_out);
    rt = target_desc(cube_uv, nullptr, x, y, 3 * out_size, 2 * out_size);
    render_scene(cb, flat_scene, &flat, &rt);
  }
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));
  tex_destroy(ping);
  tex_destroy(depth);
  for (int i = 0; i < p.nlods; i++) gpu_geometry_destroy(p.lod_mesh[i]);
  gpu_geometry_destroy(box->geo);
  return cube_uv;
}
