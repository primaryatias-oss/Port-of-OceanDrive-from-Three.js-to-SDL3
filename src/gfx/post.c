// Port of src/renderer/post.js (EffectComposer + RenderPass + UnrealBloomPass + ShaderPass
// grade + OutputPass + ShaderPass finish [+ FXAA]).
#include "gfx/post.h"

enum { NMIPS = 5 };

struct Post {
  PostOptions o;
  int w, h;
  // composer targets: rt1 = readBuffer after the RenderPass (MSAA when samples > 0), rt2
  Texture *rt1_msaa, *rt1, *rt1_depth, *rt2;
  Texture *screen;
  // bloom
  Texture *bright, *horiz[NMIPS], *vert[NMIPS];
  int mip_w[NMIPS], mip_h[NMIPS];
  Material *highpass, *blur[NMIPS], *composite, *blend;
  Material *grade, *output, *finish, *fxaa;
  Node *quad_scene, *quad;
  Camera quad_cam;
};

static SamplerDesc rt_sampler(void) {   // WebGLRenderTarget: Linear, no mipmaps, clamp
  SamplerDesc s = sampler_default();
  s.mipmaps = false;
  return s;
}

static Texture *half_target(int w, int h) {
  return tex_create_target(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, SDL_GPU_SAMPLECOUNT_1, rt_sampler());
}

static SDL_GPUSampleCount sample_count(int n) {
  return n >= 8 ? SDL_GPU_SAMPLECOUNT_8 : n >= 4 ? SDL_GPU_SAMPLECOUNT_4 : n >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
}

static void destroy_targets(Post *p) {
  tex_destroy(p->rt1_msaa); tex_destroy(p->rt1); tex_destroy(p->rt1_depth); tex_destroy(p->rt2); tex_destroy(p->screen);
  tex_destroy(p->bright);
  for (int i = 0; i < NMIPS; i++) { tex_destroy(p->horiz[i]); tex_destroy(p->vert[i]); }
  p->rt1_msaa = p->rt1 = p->rt1_depth = p->rt2 = p->screen = p->bright = nullptr;
  for (int i = 0; i < NMIPS; i++) p->horiz[i] = p->vert[i] = nullptr;
}

static void create_targets(Post *p) {
  int w = p->w, h = p->h;
  SDL_GPUSampleCount sc = sample_count(p->o.samples);
  p->rt1 = half_target(w, h);
  p->rt2 = half_target(w, h);
  if (sc != SDL_GPU_SAMPLECOUNT_1)
    p->rt1_msaa = tex_create_target(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, sc, rt_sampler());
  SDL_GPUTextureFormat df = SDL_GPUTextureSupportsFormat(g_gpu.dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
    ? SDL_GPU_TEXTUREFORMAT_D24_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;   // WebGL's default depth renderbuffer is 24-bit
  p->rt1_depth = tex_create_target(w, h, df, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, 1, sc, rt_sampler());
  p->screen = tex_create_target(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, SDL_GPU_SAMPLECOUNT_1, rt_sampler());
  if (p->o.bloom) {
    // UnrealBloomPass: resolution = drawing buffer; mips at round(w / 2), round(w / 4), ...
    int rx = (int)js_round(w / 2.0), ry = (int)js_round(h / 2.0);
    p->bright = half_target(rx, ry);
    for (int i = 0; i < NMIPS; i++) {
      p->mip_w[i] = rx;
      p->mip_h[i] = ry;
      p->horiz[i] = half_target(rx, ry);
      p->vert[i] = half_target(rx, ry);
      mat_set_vec2(p->blur[i], "invSize", v2(1.0 / rx, 1.0 / ry));
      rx = (int)js_round(rx / 2.0);
      ry = (int)js_round(ry / 2.0);
    }
    for (int i = 0; i < NMIPS; i++) {
      char nm[32];
      snprintf(nm, sizeof nm, "blurTexture%d", i + 1);
      mat_set_texture(p->composite, nm, p->vert[i]);
    }
  }
  mat_set_vec2(p->grade, "uResolution", v2(w, h));
  mat_set_vec2(p->finish, "uResolution", v2(w, h));
  if (p->fxaa) mat_set_vec2(p->fxaa, "resolution", v2(1.0 / w, 1.0 / h));
}

// a ShaderMaterial for a full-screen pass (depthTest / depthWrite off like FullScreenQuad use)
static Material *pass_mat(ProgramId id, const char *name) {
  Material *m = mat_new(id, name);
  return m;
}

// UnrealBloomPass._getSeparableBlurMaterial(kernelRadius) uniforms
static void blur_uniforms(Material *m, int kernel) {
  double coef[32], offsets[16], weights[16];
  double sigma = kernel / 3.0;
  for (int i = 0; i < kernel; i++) coef[i] = 0.39894 * exp(-0.5 * i * i / (sigma * sigma)) / sigma;
  int n = 0;
  for (int i = 1; i < kernel; i += 2) {
    double wa = coef[i], wb = (i + 1 < kernel) ? coef[i + 1] : 0;
    double w = wa + wb;
    offsets[n] = (i * wa + (i + 1) * wb) / w;
    weights[n] = w;
    n++;
  }
  mat_set_float(m, "centerWeight", coef[0]);
  mat_set_floats(m, "gaussianOffsets", offsets, n);
  mat_set_floats(m, "gaussianWeights", weights, n);
}

Post *post_create(int w, int h, PostOptions o) {
  Post *p = xcalloc(1, sizeof *p);
  p->o = o;
  p->w = w;
  p->h = h;
  // FullScreenQuad: FullscreenTriangleGeometry + OrthographicCamera(-1, 1, 1, -1, 0, 1)
  Geometry *g = geo_new();
  const float pos[9] = { -1, 3, 0, -1, -1, 0, 3, -1, 0 }, uv[6] = { 0, 2, 0, 0, 2, 0 };
  geo_set_attr_copy(g, "position", 3, 3, pos);
  geo_set_attr_copy(g, "uv", 2, 3, uv);
  p->quad = node_mesh(gpu_geometry(g), nullptr);
  geo_free(g);
  p->quad->frustum_culled = false;
  p->quad_scene = node_new(NODE_GROUP, "fsQuad");
  node_add(p->quad_scene, p->quad);
  p->quad_cam = (Camera){ .node = node_new(NODE_GROUP, "fsCamera"), .ortho = true, .left = -1, .right = 1, .top = 1,
                          .bottom = -1, .near = 0, .far = 1, .zoom = 1 };

  if (o.bloom) {
    p->highpass = pass_mat(PROG_POST_HIGHPASS, "highpass");
    mat_set_float(p->highpass, "luminosityThreshold", 4.0);
    mat_set_float(p->highpass, "smoothWidth", 0.01);
    mat_set_color(p->highpass, "defaultColor", color_rgb(0, 0, 0));
    mat_set_float(p->highpass, "defaultOpacity", 0.0);
    static const ProgramId blur_prog[NMIPS] = { PROG_POST_BLUR3, PROG_POST_BLUR5, PROG_POST_BLUR7, PROG_POST_BLUR9, PROG_POST_BLUR11 };
    static const int kernels[NMIPS] = { 6, 10, 14, 18, 22 };
    for (int i = 0; i < NMIPS; i++) {
      p->blur[i] = pass_mat(blur_prog[i], "blur");
      blur_uniforms(p->blur[i], kernels[i]);
    }
    p->composite = pass_mat(PROG_POST_COMPOSITE, "composite");
    mat_set_float(p->composite, "bloomStrength", o.bloom_strength);
    mat_set_float(p->composite, "bloomRadius", 0.25);   // render(): this.radius
    const double factors[NMIPS] = { 1.0, 0.8, 0.6, 0.4, 0.2 };
    mat_set_floats(p->composite, "bloomFactors", factors, NMIPS);
    for (int i = 0; i < NMIPS; i++) {
      char nm[32];
      snprintf(nm, sizeof nm, "bloomTintColors[%d]", i);
      mat_set_vec3(p->composite, nm, v3(1, 1, 1));
    }
    p->blend = pass_mat(PROG_POST_BLEND, "bloom blend");
    p->blend->blending = BLEND_ADDITIVE;
    p->blend->premultiplied_alpha = true;
    p->blend->transparent = true;
    p->blend->depth_test = p->blend->depth_write = false;
    mat_set_float(p->blend, "opacity", 1.0);
  }
  p->grade = pass_mat(PROG_POST_GRADE, "CameraGrade");
  mat_set_float(p->grade, "uTime", 0);
  mat_set_vec3(p->grade, "uShadowTint", v3(0.93, 0.98, 1.08));
  mat_set_vec3(p->grade, "uHighlightTint", v3(1.02, 0.995, 0.985));
  mat_set_float(p->grade, "uSaturation", 0.97);
  mat_set_vec2(p->grade, "uSunUv", v2(0.5, 0.5));
  mat_set_float(p->grade, "uSunVis", 0);
  p->output = pass_mat(PROG_POST_OUTPUT, "OutputShader");
  mat_set_float(p->output, "toneMappingExposure", o.exposure);
  p->finish = pass_mat(PROG_POST_FINISH, "CameraFinish");
  mat_set_float(p->finish, "uTime", 0);
  mat_set_float(p->finish, "uVignette", 0.12);
  mat_set_float(p->finish, "uContrast", 0.4);
  if (o.fxaa) p->fxaa = pass_mat(PROG_POST_FXAA, "FXAAShader");
  create_targets(p);
  return p;
}

void post_resize(Post *p, int w, int h) {
  if (w == p->w && h == p->h) return;
  destroy_targets(p);
  p->w = w;
  p->h = h;
  create_targets(p);
}

Texture *post_screen(Post *p) { return p->screen; }

static void fs_pass(Post *p, SDL_GPUCommandBuffer *cb, Material *m, Texture *target, bool clear, SDL_FColor clear_col) {
  p->quad->material = m;
  RenderTargetDesc rt = { .color = target->gpu, .color_format = target->format, .samples = SDL_GPU_SAMPLECOUNT_1,
                          .w = target->w, .h = target->h, .clear_color = clear, .clear = clear_col };
  render_scene(cb, p->quad_scene, &p->quad_cam, &rt);
}

void post_render(Post *p, SDL_GPUCommandBuffer *cb, Node *scene, Camera *cam, double time, V3 sun_dir) {
  // post.js render(): sun position on screen for the veiling glare
  camera_update(cam);
  const double *e = cam->node->matrix_world.e;
  V3 fwd = v3_norm(v3(-e[8], -e[9], -e[10]));
  V3 sp = v3_add_scaled(m4_get_position(cam->node->matrix_world), sun_dir, 1000);
  sp = v3_apply_m4(v3_apply_m4(sp, cam->view), cam->projection);
  bool front = v3_dot(fwd, sun_dir) > 0;
  double off = fmax(fabs(sp.x), fabs(sp.y)) - 1;
  mat_set_float(p->grade, "uSunVis", front ? clampd(1 - off / 0.6, 0, 1) : 0);
  mat_set_vec2(p->grade, "uSunUv", v2(sp.x * 0.5 + 0.5, sp.y * 0.5 + 0.5));
  mat_set_float(p->grade, "uTime", time);
  mat_set_float(p->finish, "uTime", time);

  // RenderPass: clear (renderer clear colour black, alpha 1) and draw the scene into rt1
  bool msaa = p->rt1_msaa != nullptr;
  RenderTargetDesc srt = {
    .color = msaa ? p->rt1_msaa->gpu : p->rt1->gpu, .color_format = p->rt1->format,
    .resolve = msaa ? p->rt1->gpu : nullptr,
    .depth = p->rt1_depth->gpu, .depth_format = p->rt1_depth->format,
    .samples = sample_count(p->o.samples), .w = p->w, .h = p->h,
    .clear_color = true, .clear = { 0, 0, 0, 1 }, .clear_depth = true, .depth_store = SDL_GPU_STOREOP_DONT_CARE,
  };
  render_scene(cb, scene, cam, &srt);

  Texture *read = p->rt1, *write = p->rt2;
  if (p->o.bloom) {
    const SDL_FColor clear0 = { 0, 0, 0, 0 };   // setClearColor(black, 0) + renderer.clear()
    mat_set_texture(p->highpass, "tDiffuse", read);
    fs_pass(p, cb, p->highpass, p->bright, true, clear0);
    Texture *in = p->bright;
    for (int i = 0; i < NMIPS; i++) {
      mat_set_texture(p->blur[i], "colorTexture", in);
      mat_set_vec2(p->blur[i], "direction", v2(1, 0));
      fs_pass(p, cb, p->blur[i], p->horiz[i], true, clear0);
      mat_set_texture(p->blur[i], "colorTexture", p->horiz[i]);
      mat_set_vec2(p->blur[i], "direction", v2(0, 1));
      fs_pass(p, cb, p->blur[i], p->vert[i], true, clear0);
      in = p->vert[i];
    }
    fs_pass(p, cb, p->composite, p->horiz[0], true, clear0);
    // additive blend of the bloom into the readBuffer (the MSAA scene target, then resolved)
    mat_set_texture(p->blend, "tDiffuse", p->horiz[0]);
    p->quad->material = p->blend;
    RenderTargetDesc brt = srt;
    brt.clear_color = brt.clear_depth = false;
    brt.depth = nullptr;
    brt.depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    render_scene(cb, p->quad_scene, &p->quad_cam, &brt);
  }
  // CameraGrade (ShaderPass: needsSwap)
  mat_set_texture(p->grade, "tDiffuse", read);
  fs_pass(p, cb, p->grade, write, false, (SDL_FColor){});
  Texture *t = read; read = write; write = t;
  // OutputPass
  mat_set_texture(p->output, "tDiffuse", read);
  fs_pass(p, cb, p->output, write, false, (SDL_FColor){});
  t = read; read = write; write = t;
  // CameraFinish: to the screen, or to the ping-pong target when FXAA follows
  mat_set_texture(p->finish, "tDiffuse", read);
  if (p->fxaa) {
    fs_pass(p, cb, p->finish, write, false, (SDL_FColor){});
    mat_set_texture(p->fxaa, "tDiffuse", write);
    fs_pass(p, cb, p->fxaa, p->screen, false, (SDL_FColor){});
  } else {
    fs_pass(p, cb, p->finish, p->screen, false, (SDL_FColor){});
  }
}
