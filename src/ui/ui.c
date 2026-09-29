// The page UI (see ui.h).
#include "ui/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canvas/canvas.h"
#include "canvas/font.h"
#include "gfx/gpu.h"
#include "math/vmath.h"

// ---- CSS timing functions and transitions ------------------------------------------------------

typedef struct Ease { double x1, y1, x2, y2; } Ease;
static const Ease EASE = { 0.25, 0.1, 0.25, 1 };           // ease
static const Ease LINEAR = { 0, 0, 1, 1 };

static double bez1(double a, double b, double t) {   // one coordinate of the curve (P0 = 0, P3 = 1)
  double u = 1 - t;
  return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}
static double ease_at(Ease e, double x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  double lo = 0, hi = 1, t = x;
  for (int i = 0; i < 40; i++) {   // bisection on x(t) (monotonic for x1, x2 in [0, 1])
    t = (lo + hi) / 2;
    if (bez1(e.x1, e.x2, t) < x) lo = t; else hi = t;
  }
  return bez1(e.y1, e.y2, t);
}

// a CSS transition on one property: retargeting starts from the current value
typedef struct Tween { double from, to, t, dur, delay; Ease e; } Tween;
static double tw(const Tween *w) {
  if (w->t < w->delay) return w->from;
  double u = w->dur > 0 ? (w->t - w->delay) / w->dur : 1;
  return w->from + (w->to - w->from) * ease_at(w->e, u);
}
static void tw_go(Tween *w, double to, double dur, double delay, Ease e) {
  if (to == w->to) return;
  *w = (Tween){ tw(w), to, 0, dur, delay, e };
}
static void tw_set(Tween *w, double v) { *w = (Tween){ v, v, 0, 0, 0, LINEAR }; }
static void tw_step(Tween *w, double dt) { w->t += dt; }

// ---- layers ------------------------------------------------------------------------------------

typedef struct Layer {
  Canvas *cv;                  // the layer's pixels (null: `ext` or a vignette)
  SDL_GPUTexture *tex, *ext;   // own texture / an external one (the loader group)
  int tw, th;
  bool dirty;
  double x, y, w, h;           // destination, device px, y down
  double opacity, gain;
  bool screen;                 // mix-blend-mode: screen
  bool clip;                   // scissor (device px)
  double cx0, cy0, cx1, cy1;
  bool vignette;               // the shader's elliptical gradient instead of a texture
  double ell[4], col[4], stops[2];
} Layer;

// a cleared canvas of (at least) w x h px for the layer, marked dirty
static Canvas *layer_canvas(Layer *L, int w, int h) {
  w = imax(1, w); h = imax(1, h);
  if (L->cv && (canvas_width(L->cv) != w || canvas_height(L->cv) != h)) { canvas_free(L->cv); L->cv = nullptr; }
  if (!L->cv) L->cv = canvas_new(w, h);
  else cv_clear_rect(L->cv, 0, 0, w, h);
  L->dirty = true;
  return L->cv;
}
static void layer_at(Layer *L, double x, double y) {   // 1:1 placement at device px
  L->x = x; L->y = y;
  L->w = canvas_width(L->cv); L->h = canvas_height(L->cv);
}
static void layer_free(Layer *L) {
  if (L->cv) canvas_free(L->cv);
  if (L->tex) SDL_ReleaseGPUTexture(g_gpu.dev, L->tex);
  *L = (Layer){};
}

typedef struct VertU { float rect[4], uv[4]; } VertU;
typedef struct FragU { float params[4], ell[4], color[4], stops[4]; } FragU;

// ---- the UI ------------------------------------------------------------------------------------

enum {   // loader layers, bottom to top
  LD_BG, LD_SKY, LD_DAWN, LD_RAYS, LD_HAZE, LD_GLOW, LD_SUN, LD_SEA, LD_WATER, LD_GLITTER,
  LD_LINE, LD_BAR, LD_MARK, LD_LABEL, LD_PCT, LD_CORNERS, LD_BLOOM, LD_VIG, LD_COUNT
};
enum { T_HINT, T_STICK, T_KNOB, T_JUMP, T_JUMP_LABEL, T_RIDE, T_MUTE, T_COUNT };

struct Ui {
  SDL_GPUGraphicsPipeline *pipe[2];   // normal, screen
  SDL_GPUSampler *sampler;
  SDL_GPUTexture *white;              // bound for the vignette draws
  SDL_GPUTransferBuffer *tb;
  uint32_t tb_size;
  SDL_GPUTexture *group;              // the loader, drawn as one group (its opacity fades it)
  int group_w, group_h;
  Layer group_layer;

  double k, W, H;                     // device px per CSS px, viewport (CSS px)
  int dw, dh;                         // device size the layouts were built for

  struct {
    bool on;                          // #loader in the page
    bool done, reported, scene_ready;
    double t, last_report, last_swap;
    double target, shown;
    char current[96], pending[96], drawn_label[96];
    double swap_at;                   // the label's text is replaced at this time (-1: none)
    Tween label_op, label_dy;         // .ld-label.swap
    int drawn_pct;
    double exit_t;                    // time since finish() (-1: not yet)
    Tween opacity, sun_ty, sun_scale, sun_gain, bloom;
    double hz, sun;                   // layout (CSS px)
    double mark_y;                    // the mark's resting position (device px)
    double water_t, glitter_t;        // animation clocks at the last procedural fill
    Layer L[LD_COUNT];
  } ld;

  // #overlay
  Layer vig, caption;
  Tween overlay_op;
  bool caption_touch;
  bool loading;                       // body.loading
  // #note
  Layer note;
  Tween note_op;
  char note_text[96];
  double note_left;
  // ride prompt
  Layer prompt;
  Tween prompt_op;
  char prompt_key[8], prompt_text[64];
  // HUD
  Layer hud;
  char hud_text[256];
  // touch
  Layer T[T_COUNT];
  Tween touch_op, stick_op, knob_op, hint_op, ride_op;
  int touch_version;
  bool touch_built;

  const Layer *list[64];
  int n;
  uint64_t paint_ns;                  // the last loader-only paint
  // #gpu-reset
  int reset;                          // 0 none, 1 "reloading at a lighter quality", 2 "again" (button)
  Layer reset_bg, reset_fg;
  Tween reset_op;
  double button[4];                   // the button's box (CSS px)
  bool button_hover;
  // dev: OD_UI_SHOT=path@N saves the N-th composed page (loader paints and frames, from 1)
  long calls, cap_n;
  char cap_path[256];
  SDL_GPUTexture *cap;
  int cap_w, cap_h;
};

static void push(Ui *u, Layer *L, double opacity) {
  if (opacity <= 0.001 || (!L->cv && !L->ext && !L->vignette)) return;
  CHECK(u->n < (int)ARRAY_LEN(u->list));
  L->opacity = opacity;
  u->list[u->n++] = L;
}

// ---- GPU ---------------------------------------------------------------------------------------

static SDL_GPUGraphicsPipeline *make_pipeline(bool screen) {
  SDL_GPUShader *vs = gpu_shader(SH_UI_VERT), *fs = gpu_shader(SH_UI_FRAG);
  SDL_GPUColorTargetDescription ct = {
    .format = g_gpu.swap_format,
    .blend_state = {
      .enable_blend = true,
      .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
      .dst_color_blendfactor = screen ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .color_blend_op = SDL_GPU_BLENDOP_ADD,
      .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
      .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
    },
  };
  SDL_GPUGraphicsPipelineCreateInfo ci = {
    .vertex_shader = vs, .fragment_shader = fs,
    .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP,
    .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
    .target_info = { .color_target_descriptions = &ct, .num_color_targets = 1 },
  };
  SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(g_gpu.dev, &ci);
  if (!p) FATAL("ui pipeline: %s", SDL_GetError());
  SDL_ReleaseGPUShader(g_gpu.dev, vs);
  SDL_ReleaseGPUShader(g_gpu.dev, fs);
  return p;
}

static SDL_GPUTexture *make_texture(int w, int h, bool target) {
  SDL_GPUTexture *t = SDL_CreateGPUTexture(g_gpu.dev, &(SDL_GPUTextureCreateInfo){
    .type = SDL_GPU_TEXTURETYPE_2D, .format = target ? g_gpu.swap_format : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0),
    .width = (uint32_t)w, .height = (uint32_t)h, .layer_count_or_depth = 1, .num_levels = 1 });
  if (!t) FATAL("ui texture: %s", SDL_GetError());
  return t;
}

// uploads every dirty layer of the list in one copy pass
static void upload(Ui *u, SDL_GPUCommandBuffer *cb) {
  uint32_t total = 0;
  for (int i = 0; i < u->n; i++) {
    const Layer *L = u->list[i];
    if (L->cv && L->dirty) total += (uint32_t)canvas_width(L->cv) * (uint32_t)canvas_height(L->cv) * 4u;
  }
  if (!total) return;
  if (total > u->tb_size) {
    if (u->tb) SDL_ReleaseGPUTransferBuffer(g_gpu.dev, u->tb);
    u->tb_size = total + total / 2;
    u->tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = u->tb_size });
    if (!u->tb) FATAL("ui upload buffer: %s", SDL_GetError());
  }
  uint8_t *map = SDL_MapGPUTransferBuffer(g_gpu.dev, u->tb, true);
  uint32_t off = 0;
  for (int i = 0; i < u->n; i++) {
    const Layer *L = u->list[i];
    if (!L->cv || !L->dirty) continue;
    size_t bytes = (size_t)canvas_width(L->cv) * canvas_height(L->cv) * 4;
    memcpy(map + off, canvas_pixels(L->cv), bytes);
    off += (uint32_t)bytes;
  }
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, u->tb);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  off = 0;
  for (int i = 0; i < u->n; i++) {
    Layer *L = (Layer *)u->list[i];
    if (!L->cv || !L->dirty) continue;
    int w = canvas_width(L->cv), h = canvas_height(L->cv);
    if (!L->tex || L->tw != w || L->th != h) {
      if (L->tex) SDL_ReleaseGPUTexture(g_gpu.dev, L->tex);
      L->tex = make_texture(w, h, false);
      L->tw = w; L->th = h;
    }
    SDL_UploadToGPUTexture(cp, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = u->tb, .offset = off },
                           &(SDL_GPUTextureRegion){ .texture = L->tex, .w = (uint32_t)w, .h = (uint32_t)h, .d = 1 }, false);
    off += (uint32_t)w * (uint32_t)h * 4u;
    L->dirty = false;
  }
  SDL_EndGPUCopyPass(cp);
}

static void draw_list(Ui *u, SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int w, int h, int from, int to, bool clear) {
  SDL_GPUColorTargetInfo ct = { .texture = target, .load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
                                .store_op = SDL_GPU_STOREOP_STORE, .clear_color = { 0, 0, 0, 0 } };
  SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &ct, 1, nullptr);
  SDL_SetGPUViewport(rp, &(SDL_GPUViewport){ 0, 0, (float)w, (float)h, 0, 1 });
  int bound = -1;
  for (int i = from; i < to; i++) {
    const Layer *L = u->list[i];
    int pi = L->screen ? 1 : 0;
    if (pi != bound) { SDL_BindGPUGraphicsPipeline(rp, u->pipe[pi]); bound = pi; }
    SDL_Rect sc = { 0, 0, w, h };
    if (L->clip) {
      int x0 = (int)fmax(0, floor(L->cx0)), y0 = (int)fmax(0, floor(L->cy0));
      int x1 = (int)fmin(w, ceil(L->cx1)), y1 = (int)fmin(h, ceil(L->cy1));
      if (x1 <= x0 || y1 <= y0) continue;
      sc = (SDL_Rect){ x0, y0, x1 - x0, y1 - y0 };
    }
    SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTexture *tex = L->vignette ? u->white : L->ext ? L->ext : L->tex;
    SDL_BindGPUFragmentSamplers(rp, 0, &(SDL_GPUTextureSamplerBinding){ .texture = tex, .sampler = u->sampler }, 1);
    VertU vu = { { (float)(L->x / w * 2 - 1), (float)(1 - L->y / h * 2), (float)((L->x + L->w) / w * 2 - 1), (float)(1 - (L->y + L->h) / h * 2) },
                 { 0, 0, 1, 1 } };
    if (L->vignette) vu = (VertU){ { -1, 1, 1, -1 }, { 0, 0, 1, 1 } };
    FragU fu = { { (float)L->opacity, (float)(L->gain > 0 ? L->gain : 1), L->vignette ? 1.0f : 0.0f, 0 },
                 { (float)L->ell[0], (float)L->ell[1], (float)L->ell[2], (float)L->ell[3] },
                 { (float)L->col[0], (float)L->col[1], (float)L->col[2], (float)L->col[3] },
                 { (float)L->stops[0], (float)L->stops[1], 0, 0 } };
    SDL_PushGPUVertexUniformData(cb, 0, &vu, sizeof vu);
    SDL_PushGPUFragmentUniformData(cb, 0, &fu, sizeof fu);
    SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
  }
  SDL_EndGPURenderPass(rp);
}

// ---- text --------------------------------------------------------------------------------------

// a font string for the cased UI set: e.g. font(buf, "italic 400", 30, "Georgia, serif")
static const char *font_css(char *buf, size_t n, const char *style, double px, const char *family) {
  snprintf(buf, n, "cased %s %.4fpx %s", style, px, family);
  return buf;
}
// the alphabetic baseline of a line box (top, line-height) for a font, like CSS's half-leading
static double baseline_in(const char *css, double top, double line_h) {
  FontSpec f = font_parse(css);
  double a = font_ascent(&f), d = font_descent(&f);
  return top + (line_h - (a + d)) / 2 + a;
}
// text with a CSS text-shadow (offset y, blur radius) under it, left-aligned at x (canvas units
// are CSS px scaled by k); the canvas blur works in device px
static void shadow_text(Canvas *c, double k, const char *text, double x, double y, const char *color,
                        const char *shadow, double sh_dy, double sh_blur) {
  if (shadow) {
    cv_filter_blur(c, sh_blur / 2 * k);
    cv_fill_color(c, shadow);
    cv_fill_text(c, text, x, y + sh_dy);
    cv_filter_blur(c, 0);
  }
  cv_fill_color(c, color);
  cv_fill_text(c, text, x, y);
}
static void upper(char *dst, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) dst[i] = (char)((unsigned char)src[i] < 0x80 ? toupper((unsigned char)src[i]) : src[i]);
  dst[i] = 0;
}

// ---- loader ------------------------------------------------------------------------------------

static double clampv(double lo, double v, double hi) { return v < lo ? lo : v > hi ? hi : v; }   // CSS clamp()

// linear gradient stops given as "css" strings at offsets
typedef struct Stop { double t; const char *c; } Stop;
static void vgrad_fill(Canvas *c, double x, double y, double w, double h, const Stop *s, int n, bool vertical) {
  Gradient g = vertical ? cv_linear_gradient(c, 0, y, 0, y + h) : cv_linear_gradient(c, x, 0, x + w, 0);
  for (int i = 0; i < n; i++) grad_add_stop(&g, s[i].t, s[i].c);
  cv_fill_gradient(c, &g);
  cv_fill_rect(c, x, y, w, h);
}

// a vertical gradient layer: 1 px wide, stretched across [x, x + w] (the gradient only varies in y)
static void column_layer(Ui *u, Layer *L, double x, double y, double w, double h, const Stop *s, int n) {
  int ph = (int)ceil(h * u->k);
  Canvas *c = layer_canvas(L, 1, ph);
  cv_save(c);
  cv_scale(c, 1, u->k);
  vgrad_fill(c, 0, 0, 1, h, s, n, true);
  cv_restore(c);
  L->x = x * u->k; L->y = y * u->k; L->w = w * u->k; L->h = ph;
}

// .ld-l: a double rule (1px lines, 3px tall box)
static void double_rule(Canvas *c, double x, double y, double w, const char *color, double alpha) {
  cv_save(c);
  cv_global_alpha(c, alpha);
  cv_fill_color(c, color);
  cv_fill_rect(c, x, y, w, 1);
  cv_fill_rect(c, x, y + 2, w, 1);
  cv_restore(c);
}

static void loader_layout(Ui *u) {
  double W = u->W, H = u->H, k = u->k, vmin = fmin(W, H) / 100, vmax = fmax(W, H) / 100;
  bool portrait = H > W;
  double hz = H * (portrait ? 0.60 : 0.63);
  double S = portrait ? clampv(104, 30 * vmin, 260) : clampv(104, 23 * vmin, 240);
  u->ld.hz = hz; u->ld.sun = S;
  Layer *L = u->ld.L;
  char f[160];

  // background (#1a1936)
  {
    Canvas *c = layer_canvas(&L[LD_BG], 1, 1);
    cv_fill_color(c, "#1a1936");
    cv_fill_rect(c, 0, 0, 1, 1);
    L[LD_BG].x = 0; L[LD_BG].y = 0; L[LD_BG].w = W * k; L[LD_BG].h = H * k;
  }
  // .ld-sky, .ld-dawn, .ld-haze, .ld-sea: vertical gradients
  static const Stop sky[] = { { 0, "#15152f" }, { 0.20, "#262449" }, { 0.40, "#4b3864" }, { 0.57, "#86546f" },
                              { 0.71, "#c47a7a" }, { 0.83, "#e9a283" }, { 0.93, "#f6c79a" }, { 1, "#fbe2b6" } };
  column_layer(u, &L[LD_SKY], 0, 0, W, hz, sky, ARRAY_LEN(sky));
  static const Stop dawn[] = { { 0, "#2d2d5e" }, { 0.30, "#6c4f7e" }, { 0.60, "#cf8a8b" }, { 0.82, "#f6bb92" }, { 1, "#fff0c9" } };
  column_layer(u, &L[LD_DAWN], 0, 0, W, hz, dawn, ARRAY_LEN(dawn));
  static const Stop haze[] = { { 0, "rgba(255, 226, 196, 0)" }, { 1, "rgba(255, 228, 196, 0.38)" } };
  column_layer(u, &L[LD_HAZE], 0, hz * 0.78, W, hz * 0.22, haze, ARRAY_LEN(haze));
  static const Stop sea[] = { { 0, "#d9a795" }, { 0.09, "#ad7a83" }, { 0.32, "#775676" }, { 0.66, "#3d3357" }, { 1, "#1b1a34" } };
  column_layer(u, &L[LD_SEA], 0, hz, W, H - hz, sea, ARRAY_LEN(sea));

  // .ld-rays: 0.5-degree wedges every 6 degrees around the sun's foot, masked by
  // radial-gradient(circle closest-side, #000 0%, rgba(0,0,0,.35) 30%, transparent 62%) of a 150vmax square
  {
    Canvas *c = layer_canvas(&L[LD_RAYS], (int)ceil(W * k), (int)ceil(hz * k));
    cv_save(c);
    cv_scale(c, k, k);
    double cx = W / 2, cy = hz, R = 75 * vmax;
    Gradient g = cv_radial_gradient(c, cx, cy, 0, cx, cy, R);
    grad_add_stop(&g, 0, "rgba(255, 236, 205, 0.55)");
    grad_add_stop(&g, 0.30, "rgba(255, 236, 205, 0.1925)");
    grad_add_stop(&g, 0.62, "rgba(255, 236, 205, 0)");
    cv_fill_gradient(c, &g);
    cv_begin_path(c);
    for (int i = 0; i < 60; i++) {   // conic 0deg = up, clockwise
      double a0 = (i * 6.0) * DEG2RAD, a1 = (i * 6.0 + 0.5) * DEG2RAD, r = R * 0.62;
      cv_move_to(c, cx, cy);
      cv_line_to(c, cx + sin(a0) * r, cy - cos(a0) * r);
      cv_line_to(c, cx + sin(a1) * r, cy - cos(a1) * r);
      cv_close_path(c);
    }
    cv_fill(c);
    cv_restore(c);
    layer_at(&L[LD_RAYS], 0, 0);
  }

  // .ld-sun glow: the three box-shadows (drawn small, they are soft), under the disc
  {
    double pad = 110 + 300 * 1.2, gs = 0.125;   // CSS px beyond the disc; canvas scale
    double size = S + 2 * pad;
    Canvas *c = layer_canvas(&L[LD_GLOW], (int)ceil(size * k * gs), (int)ceil(size * k * gs));
    cv_save(c);
    cv_scale(c, k * gs, k * gs);
    struct { double blur, spread; const char *col; } sh[3] = {
      { 300, 110, "rgba(255, 150, 124, 0.16)" }, { 130, 44, "rgba(255, 176, 124, 0.32)" }, { 36, 8, "rgba(255, 218, 165, 0.55)" } };
    for (int i = 0; i < 3; i++) {   // the last listed is the bottom-most
      cv_filter_blur(c, sh[i].blur / 2 * k * gs);
      cv_fill_color(c, sh[i].col);
      cv_begin_path(c);
      cv_arc(c, size / 2, size / 2, S / 2 + sh[i].spread, 0, 2 * PI_D, false);
      cv_fill(c);
    }
    cv_filter_blur(c, 0);
    cv_restore(c);
  }
  // .ld-sun disc: radial-gradient(circle at 50% 46%, ...) (farthest-corner) in a circle
  {
    Canvas *c = layer_canvas(&L[LD_SUN], (int)ceil(S * k), (int)ceil(S * k));
    cv_save(c);
    cv_scale(c, k, k);
    double gx = S * 0.5, gy = S * 0.46, r = js_hypot2(S * 0.5, S * 0.54);
    Gradient g = cv_radial_gradient(c, gx, gy, 0, gx, gy, r);
    grad_add_stop(&g, 0, "#fffcee");
    grad_add_stop(&g, 0.40, "#fff2cb");
    grad_add_stop(&g, 0.66, "#ffdca2");
    grad_add_stop(&g, 0.86, "#ffc585");
    grad_add_stop(&g, 1, "#ffb67f");
    cv_fill_gradient(c, &g);
    cv_begin_path(c);
    cv_arc(c, S / 2, S / 2, S / 2, 0, 2 * PI_D, false);
    cv_fill(c);
    cv_restore(c);
  }

  // the animated sea layers are filled per frame (loader_animate)
  L[LD_WATER].x = 0; L[LD_WATER].y = hz * k; L[LD_WATER].w = W * k;
  L[LD_GLITTER].screen = true;

  // .ld-horizon (1px line centred on the horizon)
  {
    Canvas *c = layer_canvas(&L[LD_LINE], (int)ceil(W * k), (int)ceil(3 * k));
    cv_save(c);
    cv_scale(c, k, k);
    static const Stop line[] = { { 0, "rgba(92, 58, 84, 0)" }, { 0.12, "rgba(92, 58, 84, 0.5)" }, { 0.88, "rgba(92, 58, 84, 0.5)" }, { 1, "rgba(92, 58, 84, 0)" } };
    vgrad_fill(c, 0, 1, W, 1, line, 4, false);
    cv_restore(c);
    layer_at(&L[LD_LINE], 0, (hz - 1.5) * k);
  }
  // .ld-bar at full width (scaleX(p) is the layer's width): the gradient bar and its two outer
  // box-shadows, which are painted only outside the bar's box
  {
    double m = 20, bh = 2;   // margin for the glow
    Canvas *c = layer_canvas(&L[LD_BAR], (int)ceil(W * k), (int)ceil((bh + 2 * m) * k));
    cv_save(c);
    cv_scale(c, k, k);
    // clip: everything but the bar box (outer rect + reversed inner rect, non-zero)
    cv_begin_path(c);
    cv_rect(c, -50, -50, W + 100, bh + 2 * m + 100);
    cv_move_to(c, 0, m); cv_line_to(c, 0, m + bh); cv_line_to(c, W, m + bh); cv_line_to(c, W, m); cv_close_path(c);
    cv_save(c);
    cv_clip(c);
    cv_filter_blur(c, 1 * k);   // 0 0 2px rgba(255,250,230,1) (bottom)
    cv_fill_color(c, "rgba(255, 250, 230, 1)");
    cv_fill_rect(c, 0, m, W, bh);
    cv_filter_blur(c, 5 * k);   // 0 0 10px 1px rgba(255,196,110,.9)
    cv_fill_color(c, "rgba(255, 196, 110, 0.9)");
    cv_fill_rect(c, -1, m - 1, W + 2, bh + 2);
    cv_filter_blur(c, 0);
    cv_restore(c);
    static const Stop bar[] = { { 0, "rgba(233, 183, 101, 0)" }, { 0.10, "#e9b765" }, { 0.50, "#fff6d8" }, { 0.90, "#e9b765" }, { 1, "rgba(233, 183, 101, 0)" } };
    vgrad_fill(c, 0, m, W, bh, bar, 5, false);
    cv_restore(c);
    L[LD_BAR].y = (hz - 0.5 - m) * k;
    L[LD_BAR].h = canvas_height(c);
  }

  // .ld-mark: fan, time, title, rule, subtitle (centred at 36% of the horizon height)
  {
    double fan_w = clampv(64, 8 * vmin, 96), fan_h = fan_w / 3;
    double gap1 = clampv(14, 2.4 * vmin, 24), gap2 = clampv(12, 2 * vmin, 18);
    double ts = clampv(26, 6.2 * W / 100, 82), ss = clampv(14, 2 * vmin, 19);
    double time_h = 10.5, title_h = ts * 1.05, rule_h = 7, sub_h = ss * 1.2;
    double total = fan_h + 14 + time_h + gap1 + title_h + gap1 + rule_h + gap2 + sub_h;
    double m = 30;   // margin for the title's drop shadow
    double top = hz * 0.36 - total / 2;
    Canvas *c = layer_canvas(&L[LD_MARK], (int)ceil(W * k), (int)ceil((total + 2 * m) * k));
    cv_save(c);
    cv_scale(c, k, k);
    cv_translate(c, 0, m);
    double y = 0, cx = W / 2;
    // fan (svg 120 x 40, stroke 0.9 in viewBox units)
    {
      double s = fan_w / 120;
      cv_save(c);
      cv_translate(c, cx - fan_w / 2, y);
      cv_scale(c, s, s);
      cv_stroke_color(c, "rgba(255, 228, 190, 0.75)");
      cv_line_width(c, 0.9);
      cv_begin_path(c);
      cv_arc(c, 60, 38, 52, PI_D, 2 * PI_D, false);
      cv_stroke(c);
      cv_begin_path(c);
      cv_arc(c, 60, 38, 34, PI_D, 2 * PI_D, false);
      cv_stroke(c);
      cv_save(c);
      cv_global_alpha(c, 0.8);
      static const double rays[7][2] = { { 60, 4 }, { 38, 9 }, { 82, 9 }, { 20, 20 }, { 100, 20 }, { 10, 32 }, { 110, 32 } };
      cv_begin_path(c);
      for (int i = 0; i < 7; i++) { cv_move_to(c, 60, 38); cv_line_to(c, rays[i][0], rays[i][1]); }
      cv_stroke(c);
      cv_restore(c);
      cv_begin_path(c);
      cv_move_to(c, 2, 38); cv_line_to(c, 118, 38);
      cv_stroke(c);
      cv_restore(c);
      y += fan_h + 14;
    }
    // time: rule, "6:52 AM", rule
    {
      double lw = clampv(28, 6 * vmin, 64), sp = 0.42 * 10.5;
      font_css(f, sizeof f, "400", 10.5, "'Helvetica Neue', system-ui, sans-serif");
      cv_font(c, f);
      cv_letter_spacing(c, sp);
      double tw_ = cv_measure_text(c, "6:52 AM") + sp;   // + padding-left .42em
      double row = lw + 14 + tw_ + 14 + lw, x = cx - row / 2;
      const char *col = "rgba(255, 236, 214, 0.72)";
      double_rule(c, x, y + (time_h - 3) / 2, lw, col, 0.55);
      cv_fill_color(c, col);
      cv_fill_text(c, "6:52 AM", x + lw + 14 + sp, baseline_in(f, y, time_h));
      double_rule(c, x + lw + 14 + tw_ + 14, y + (time_h - 3) / 2, lw, col, 0.55);
      cv_letter_spacing(c, 0);
      y += time_h + gap1;
    }
    // title: gradient text with a drop shadow (0 2px 16px rgba(36,18,40,.4))
    {
      font_css(f, sizeof f, "400", ts, "Didot, 'Bodoni 72', serif");
      cv_font(c, f);
      double sp = 0.3 * ts;
      cv_letter_spacing(c, sp);
      double w = cv_measure_text(c, "OCEAN DRIVE") + sp;
      double x = cx - w / 2 + sp, base = baseline_in(f, y, title_h);
      cv_filter_blur(c, 8 * k);
      cv_fill_color(c, "rgba(36, 18, 40, 0.4)");
      cv_fill_text(c, "OCEAN DRIVE", x, base + 2);
      cv_filter_blur(c, 0);
      Gradient g = cv_linear_gradient(c, 0, y, 0, y + title_h);
      grad_add_stop(&g, 0.25, "#fffaf1");
      grad_add_stop(&g, 1, "#f2d3a4");
      cv_fill_gradient(c, &g);
      cv_fill_text(c, "OCEAN DRIVE", x, base);
      cv_letter_spacing(c, 0);
      y += title_h + gap1;
    }
    // rule: double rule, diamond, double rule
    {
      double lw = clampv(46, 11 * vmin, 120);
      const char *col = "rgba(255, 226, 186, 0.8)";
      double row = lw + 14 + 7 + 14 + lw, x = cx - row / 2;
      double_rule(c, x, y + 2, lw, col, 0.55);
      double_rule(c, x + lw + 14 + 7 + 14, y + 2, lw, col, 0.55);
      cv_save(c);
      cv_translate(c, cx, y + 3.5);
      cv_rotate(c, PI_D / 4);
      cv_stroke_color(c, col);
      cv_line_width(c, 1);
      cv_stroke_rect(c, -3, -3, 6, 6);
      cv_fill_color(c, col);
      cv_fill_rect(c, -1, -1, 2, 2);
      cv_restore(c);
      y += rule_h + gap2;
    }
    // subtitle
    {
      font_css(f, sizeof f, "italic 400", ss, "Didot, 'Bodoni 72', serif");
      cv_font(c, f);
      double sp = 0.06 * ss;
      cv_letter_spacing(c, sp);
      const char *t = "Miami Beach · Sunrise";
      double w = cv_measure_text(c, t);
      cv_fill_color(c, "rgba(255, 243, 228, 0.88)");
      cv_fill_text(c, t, cx - w / 2, baseline_in(f, y, sub_h));
      cv_letter_spacing(c, 0);
    }
    cv_restore(c);
    L[LD_MARK].x = 0;
    u->ld.mark_y = L[LD_MARK].y = (top - m) * k;
    L[LD_MARK].w = canvas_width(c);
    L[LD_MARK].h = canvas_height(c);
  }

  // .ld-corner l / r
  {
    double bottom = clampv(16, 3.6 * H / 100, 34), side = clampv(16, 4 * W / 100, 48);
    Canvas *c = layer_canvas(&L[LD_CORNERS], (int)ceil(W * k), (int)ceil(20 * k));
    cv_save(c);
    cv_scale(c, k, k);
    font_css(f, sizeof f, "400", 9, "'Helvetica Neue', system-ui, sans-serif");
    cv_font(c, f);
    double sp = 0.34 * 9;
    cv_letter_spacing(c, sp);
    cv_fill_color(c, "rgba(255, 234, 212, 0.4)");
    double base = baseline_in(f, 20 - 9, 9);
    const char *l = "25°46′ N · 80°08′ W", *r = "COLLINS PARK · LOW TIDE";
    if (W <= 520) {
      double w = cv_measure_text(c, l);
      cv_fill_text(c, l, W / 2 - w / 2, base);
    } else {
      cv_fill_text(c, l, side, base);
      cv_fill_text(c, r, W - side - cv_measure_text(c, r), base);
    }
    cv_letter_spacing(c, 0);
    cv_restore(c);
    layer_at(&L[LD_CORNERS], 0, (H - bottom - 20) * k);
  }

  // .ld-bloom: radial-gradient(circle at 50% hz, ...) (farthest-corner), drawn small
  {
    double gs = 0.125;
    Canvas *c = layer_canvas(&L[LD_BLOOM], (int)ceil(W * k * gs), (int)ceil(H * k * gs));
    cv_save(c);
    cv_scale(c, k * gs, k * gs);
    double r = js_hypot2(W / 2, fmax(hz, H - hz));
    Gradient g = cv_radial_gradient(c, W / 2, hz, 0, W / 2, hz, r);
    grad_add_stop(&g, 0, "rgba(255, 250, 235, 1)");
    grad_add_stop(&g, 0.14, "rgba(255, 232, 196, 0.85)");
    grad_add_stop(&g, 0.38, "rgba(255, 214, 178, 0.35)");
    grad_add_stop(&g, 0.70, "rgba(255, 200, 170, 0)");
    cv_fill_gradient(c, &g);
    cv_fill_rect(c, 0, 0, W, H);
    cv_restore(c);
    L[LD_BLOOM].x = 0; L[LD_BLOOM].y = 0; L[LD_BLOOM].w = W * k; L[LD_BLOOM].h = H * k;
  }

  // .ld-vig: radial-gradient(ellipse 85% 80% at 50% 52%, transparent 55%, rgba(10,6,22,.55) 100%)
  {
    Layer *V = &L[LD_VIG];
    V->vignette = true;
    V->ell[0] = W / 2 * k; V->ell[1] = H * 0.52 * k; V->ell[2] = W * 0.85 * k; V->ell[3] = H * 0.80 * k;
    V->col[0] = 10 / 255.0; V->col[1] = 6 / 255.0; V->col[2] = 22 / 255.0; V->col[3] = 0.55;
    V->stops[0] = 0.55; V->stops[1] = 1;
  }
  u->ld.drawn_pct = -1;
  u->ld.drawn_label[0] = 0;
  u->ld.water_t = u->ld.glitter_t = -1;
}

// area-average of a periodic box (on in [a, b) of each period P) over [u0, u1]
static double stripe_cov(double u0, double u1, double P, double a, double b) {
  if (u1 <= u0) return 0;
  // F(u): on-length in [0, u)
  #define FON(u) (floor((u) / P) * (b - a) + clampd(fmod(fmod((u), P) + P, P) - a, 0, b - a))
  double r = (FON(u1) - FON(u0)) / (u1 - u0);
  #undef FON
  return r;
}

// the moving water: .ld-bands on a plane tilted by rotateX(64deg) under perspective(240px)
// (x-independent: one column), and .ld-glitter's drifting glints under the sun (screen-blended)
static void loader_animate(Ui *u) {
  double k = u->k, W = u->W, H = u->H, hz = u->ld.hz, S = u->ld.sun, t = u->ld.t;
  double sea_h = H - hz;
  Layer *L = u->ld.L;
  if (t - u->ld.water_t >= 1 / 60.0 || !L[LD_WATER].cv) {
    u->ld.water_t = t;
    int rows = (int)ceil(sea_h * k);
    Canvas *c = layer_canvas(&L[LD_WATER], 1, rows);
    uint8_t *px = canvas_pixels_rw(c);
    double cs = cos(64 * DEG2RAD), sn = sin(64 * DEG2RAD), P = 240;
    double drift = 66 * fmod(t / 9, 1);
    for (int r = 0; r < rows; r++) {
      double y0 = r / k, y1 = (r + 1) / k;
      // screen y -> plane distance s: y = s cs P / (P - s sn)
      double s0 = P * y0 / (P * cs + y0 * sn), s1 = P * y1 / (P * cs + y1 * sn);
      double u0 = s0 + 66 - drift, u1 = s1 + 66 - drift;
      double a1 = 0.07 * stripe_cov(u0, u1, 6, 0, 1), a2 = 0.13 * stripe_cov(u0, u1, 11, 0, 2);
      // band 1 (255,232,212) over band 2 (18,14,38), premultiplied
      double cr = 255 * a1 + 18 * a2 * (1 - a1), cg = 232 * a1 + 14 * a2 * (1 - a1), cb = 212 * a1 + 38 * a2 * (1 - a1);
      double a = a1 + a2 * (1 - a1);
      double yc = (y0 + y1) / 2 / sea_h;   // mask: #000 0-30%, rgba(0,0,0,.3) at 100%
      double m = yc < 0.3 ? 1 : 1 - 0.7 * (yc - 0.3) / 0.7;
      uint8_t *p = px + (size_t)r * 4;
      p[0] = (uint8_t)js_round(cr * m); p[1] = (uint8_t)js_round(cg * m); p[2] = (uint8_t)js_round(cb * m);
      p[3] = (uint8_t)js_round(a * m * 255);
    }
    L[LD_WATER].h = rows;
  }
  if (t - u->ld.glitter_t >= 1 / 60.0 || !L[LD_GLITTER].cv) {
    u->ld.glitter_t = t;
    double gw = 2.6 * S, gh = 0.64 * sea_h;
    int pw = (int)ceil(gw * k), ph = (int)ceil(gh * k);
    Canvas *c = layer_canvas(&L[LD_GLITTER], pw, ph);
    uint8_t *px = canvas_pixels_rw(c);
    double drift = 36 * fmod(t / 4, 1);
    // ::after stripes: repeating-linear-gradient(81deg, ...) over (97deg, ...)
    double a81 = 81 * DEG2RAD, a97 = 97 * DEG2RAD;
    double d81x = sin(a81), d81y = -cos(a81), d97x = sin(a97), d97y = -cos(a97);
    double L81 = fabs(gw * d81x) + fabs(gh * d81y), L97 = fabs(gw * d97x) + fabs(gh * d97y);
    double s81x = gw / 2 - d81x * L81 / 2, s81y = gh / 2 - d81y * L81 / 2;
    double s97x = gw / 2 - d97x * L97 / 2, s97y = gh / 2 - d97y * L97 / 2;
    for (int py = 0; py < ph; py++) {
      double y0 = py / k, y1 = (py + 1) / k, yc = (y0 + y1) / 2;
      double v0 = y0 + 36 - drift, v1 = y1 + 36 - drift;
      double l1 = 0.95 * stripe_cov(v0, v1, 6, 2, 3.2), l2 = 0.8 * stripe_cov(v0, v1, 12, 7, 8.5);
      double gr = 255 * l1 + 255 * l2 * (1 - l1), gg = 214 * l1 + 246 * l2 * (1 - l1), gb = 150 * l1 + 220 * l2 * (1 - l1);
      double ga = l1 + l2 * (1 - l1);
      double fy = yc / gh;   // fade: #000 0%, rgba(0,0,0,.4) 30%, transparent 90%
      double fade = fy < 0.3 ? 1 - 0.6 * fy / 0.3 : fy < 0.9 ? 0.4 * (1 - (fy - 0.3) / 0.6) : 0;
      uint8_t *row = px + (size_t)py * pw * 4;
      if (ga <= 0 || fade <= 0) continue;
      for (int qx = 0; qx < pw; qx++) {
        double x = (qx + 0.5) / k;
        // wedge: conic-gradient(from 180deg at 50% -40%): 0deg = straight down
        double dx = x - gw / 2, dy = yc + 0.4 * gh;
        double phi = atan2(fabs(dx), dy) / DEG2RAD;
        double wedge = phi < 15 ? 1 - 0.4 * phi / 15 : phi < 32 ? 0.6 * (1 - (phi - 15) / 17) : 0;
        if (wedge <= 0) continue;
        double t81 = fmod((x - s81x) * d81x + (yc - s81y) * d81y, 31); if (t81 < 0) t81 += 31;
        double t97 = fmod((x - s97x) * d97x + (yc - s97y) * d97y, 37); if (t97 < 0) t97 += 37;
        double b81 = t81 < 5 ? 0.7 : t81 >= 17 && t81 < 20 ? 0.5 : 0;
        double b97 = t97 < 3 ? 0.6 : t97 >= 14 && t97 < 17 ? 0.45 : 0;
        double keep = (1 - b97) * (1 - b81);   // black stripes over the glints
        double a = ga * keep;   // (the black stripes add no light: screen-blended, only colour counts)
        double m = wedge * fade;
        uint8_t *p = row + (size_t)qx * 4;
        p[0] = (uint8_t)js_round(gr * keep * m); p[1] = (uint8_t)js_round(gg * keep * m); p[2] = (uint8_t)js_round(gb * keep * m);
        p[3] = (uint8_t)js_round(clampd(a * m, 0, 1) * 255);
      }
    }
    L[LD_GLITTER].x = (W / 2 - gw / 2) * k;
    L[LD_GLITTER].y = hz * k;
    L[LD_GLITTER].w = pw; L[LD_GLITTER].h = ph;
  }
}

static void loader_label_layer(Ui *u) {
  double k = u->k, W = u->W, vmin = fmin(u->W, u->H) / 100;
  double size = clampv(14, 1.8 * vmin, 17), lh = size * 1.3, m = 12;
  char f[160];
  font_css(f, sizeof f, "italic 400", size, "Didot, 'Bodoni 72', Georgia, serif");
  Canvas *c = layer_canvas(&u->ld.L[LD_LABEL], (int)ceil(W * k), (int)ceil((lh + 2 * m) * k));
  cv_save(c);
  cv_scale(c, k, k);
  cv_font(c, f);
  double sp = 0.03 * size;
  cv_letter_spacing(c, sp);
  double w = cv_measure_text(c, u->ld.current);
  shadow_text(c, k, u->ld.current, W / 2 - w / 2, baseline_in(f, m, lh), "rgba(255, 244, 230, 0.9)",
              "rgba(20, 12, 30, 0.35)", 1, 10);
  cv_restore(c);
  snprintf(u->ld.drawn_label, sizeof u->ld.drawn_label, "%s", u->ld.current);
}

static void loader_pct_layer(Ui *u, int pct) {
  double k = u->k, W = u->W;
  char f[160], s[8];
  snprintf(s, sizeof s, "%02d%%", pct);
  font_css(f, sizeof f, "400", 9.5, "'Helvetica Neue', system-ui, sans-serif");
  Canvas *c = layer_canvas(&u->ld.L[LD_PCT], (int)ceil(W * k), (int)ceil(12 * k));
  cv_save(c);
  cv_scale(c, k, k);
  cv_font(c, f);
  double sp = 0.4 * 9.5;
  cv_letter_spacing(c, sp);
  double w = cv_measure_text(c, s) + sp;   // padding-left .4em
  cv_fill_color(c, "rgba(255, 234, 212, 0.55)");
  cv_fill_text(c, s, W / 2 - w / 2 + sp, baseline_in(f, 1, 9.5));
  cv_restore(c);
  u->ld.drawn_pct = pct;
}

// the loader driver's tick (index.html): progress easing, label swaps, finish
static void loader_tick(Ui *u, double real_dt) {
  // (the progress easing steps at most 0.1 s; swaps, timeouts and transitions run on real time)
  double dt = fmin(0.1, real_dt);
  u->ld.t += real_dt;
  double now = u->ld.t;
  // (the running transitions advance by the time that passed before this tick)
  Tween *tws[] = { &u->ld.label_op, &u->ld.label_dy, &u->ld.opacity, &u->ld.sun_ty, &u->ld.sun_scale, &u->ld.sun_gain, &u->ld.bloom };
  for (size_t i = 0; i < ARRAY_LEN(tws); i++) tw_step(tws[i], real_dt);

  if (!u->ld.reported) u->ld.target = fmin(0.08, now * 0.025);   // modules still downloading
  double goal = u->ld.target;
  if (u->ld.scene_ready) goal = u->ld.target = 1;
  else {
    // long phases creep toward the next step instead of stalling
    if (u->ld.reported)
      goal = u->ld.target + (fmin(0.97, u->ld.target + 0.13) - u->ld.target) * (1 - exp(-(now - u->ld.last_report) / 9));
    if (now - u->ld.last_report > 25) snprintf(u->ld.pending, sizeof u->ld.pending, "Still painting — some machines take a little longer…");
  }
  double sh = u->ld.shown;
  sh = fmax(sh, sh + (goal - sh) * (1 - pow(goal == 1 ? 0.0005 : 0.02, dt)));
  if (goal == 1 && 1 - sh < 0.0005) sh = 1;
  u->ld.shown = sh;
  if (strcmp(u->ld.pending, u->ld.current) && now - u->ld.last_swap > 0.65 && u->ld.swap_at < 0) {
    u->ld.last_swap = now;
    u->ld.swap_at = now + 0.3;   // the text is replaced 300 ms into the fade
    tw_go(&u->ld.label_op, 0, 0.35, 0, EASE);
    tw_go(&u->ld.label_dy, 3, 0.35, 0, EASE);
  }
  if (u->ld.swap_at >= 0 && now >= u->ld.swap_at) {
    u->ld.swap_at = -1;
    snprintf(u->ld.current, sizeof u->ld.current, "%s", u->ld.pending);
    tw_go(&u->ld.label_op, 1, 0.35, 0, EASE);
    tw_go(&u->ld.label_dy, 0, 0.35, 0, EASE);
  }
  if (!u->ld.done && u->ld.scene_ready && sh > 0.98) {   // finish()
    u->ld.done = true;
    snprintf(u->ld.pending, sizeof u->ld.pending, "Good morning.");
    u->ld.exit_t = 0;
    Ease e1 = { 0.4, 0, 0.2, 1 }, e2 = { 0.3, 0, 0.2, 1 };
    tw_go(&u->ld.opacity, 0, 1.2, 0.25, e1);
    tw_set(&u->ld.sun_ty, 0.66 - 0.84 * sh);
    tw_go(&u->ld.sun_ty, -0.16, 1.1, 0, e2);
    tw_go(&u->ld.sun_scale, 1.7, 1.1, 0, e2);
    tw_go(&u->ld.sun_gain, 1.25, 1.1, 0, EASE);
    tw_go(&u->ld.bloom, 0.9, 0.9, 0, EASE);
  }
  if (u->ld.exit_t >= 0) {
    u->ld.exit_t += real_dt;
    if (u->ld.exit_t >= 0.9) u->loading = false;   // body.loading removed
    if (u->ld.exit_t >= 1.7) u->ld.on = false;     // #loader removed
  }
}

// pushes the loader's layers (into the group list)
static void loader_push(Ui *u) {
  double k = u->k, W = u->W, hz = u->ld.hz, S = u->ld.sun, p = u->ld.shown;
  Layer *L = u->ld.L;
  loader_animate(u);
  int pct = (int)js_round(p * 100);
  if (pct != u->ld.drawn_pct) loader_pct_layer(u, pct);
  if (strcmp(u->ld.drawn_label, u->ld.current) || !L[LD_LABEL].cv) loader_label_layer(u);

  push(u, &L[LD_BG], 1);
  push(u, &L[LD_SKY], 1);
  push(u, &L[LD_DAWN], p * 0.6);
  push(u, &L[LD_RAYS], 0.05 + p * 0.11);
  push(u, &L[LD_HAZE], 1);
  // the sun: translate(-50%, 66% - p 84%) (exit: translate(-50%, -16%) scale(1.7)), clipped by the sky
  double ty = u->ld.exit_t >= 0 ? tw(&u->ld.sun_ty) : 0.66 - 0.84 * p;
  double sc = u->ld.exit_t >= 0 ? tw(&u->ld.sun_scale) : 1, gain = u->ld.exit_t >= 0 ? tw(&u->ld.sun_gain) : 1;
  double cx = W / 2, cy = hz - S + ty * S + S / 2;
  for (int i = LD_GLOW; i <= LD_SUN; i++) {
    Layer *X = &L[i];
    double size = i == LD_SUN ? S : S + 2 * (110 + 300 * 1.2);
    X->x = (cx - size * sc / 2) * k; X->y = (cy - size * sc / 2) * k;
    X->w = X->h = size * sc * k;
    X->gain = gain;
    X->clip = true; X->cx0 = 0; X->cy0 = 0; X->cx1 = W * k; X->cy1 = hz * k;
    push(u, X, 1);
  }
  push(u, &L[LD_SEA], 1);
  push(u, &L[LD_WATER], 1);
  push(u, &L[LD_GLITTER], 0.55 + p * 0.45);
  push(u, &L[LD_LINE], 1);
  L[LD_BAR].x = (W / 2 - W * p / 2) * k;
  L[LD_BAR].w = W * p * k;
  push(u, &L[LD_BAR], 1);
  // the mark's rise: 2.2s cubic-bezier(.2,.6,.2,1) both, from the page's start
  double rise = ease_at((Ease){ 0.2, 0.6, 0.2, 1 }, u->ld.t / 2.2);
  L[LD_MARK].y = u->ld.mark_y + (1 - rise) * 14 * k;
  push(u, &L[LD_MARK], rise);
  // status: label and percentage at hz + clamp(22px, 5vh, 46px)
  double st = hz + clampv(22, 5 * u->H / 100, 46);
  double vmin = fmin(u->W, u->H) / 100, lsize = clampv(14, 1.8 * vmin, 17), lh = lsize * 1.3;
  L[LD_LABEL].x = 0;
  L[LD_LABEL].y = (st - 12 + tw(&u->ld.label_dy)) * k;
  L[LD_LABEL].w = canvas_width(L[LD_LABEL].cv); L[LD_LABEL].h = canvas_height(L[LD_LABEL].cv);
  push(u, &L[LD_LABEL], tw(&u->ld.label_op));
  layer_at(&L[LD_PCT], 0, (st + lh + 11 - 1) * k);
  push(u, &L[LD_PCT], 1);
  push(u, &L[LD_CORNERS], 1);
  push(u, &L[LD_BLOOM], tw(&u->ld.bloom));
  push(u, &L[LD_VIG], 1);
}

// ---- overlay caption, note, prompt, HUD ------------------------------------------------------

typedef struct Run { const char *text; bool bold; } Run;

static void caption_layer(Ui *u, bool touch) {
  double k = u->k, W = u->W, H = u->H;
  double left = 6 * W / 100, bottom = 9 * H / 100;
  double maxw = fmin(576, W - left);
  char fh[160], fp[160], fn[160], fb[160];
  font_css(fh, sizeof fh, "italic 400", 30, "Georgia, 'Times New Roman', serif");
  font_css(fp, sizeof fp, "400", 12, "system-ui, sans-serif");
  font_css(fn, sizeof fn, "400", 13, "system-ui, sans-serif");
  font_css(fb, sizeof fb, "500", 13, "system-ui, sans-serif");
  Run runs[2] = { { touch ? "Tap to walk" : "Click to walk", true },
                  { touch ? " — left thumb to move, drag to look, Ride by the bike or the ATV"
                          : " — WASD to move, mouse to look, Space to jump, Shift to stroll faster, E to ride the bike or the ATV, M to mute", false } };
  // greedy word wrap of the .how paragraph (words keep their run's weight)
  typedef struct Word { char s[48]; bool bold; double w, space; int line; } Word;
  Word words[64];
  int nw = 0;
  Canvas *m = canvas_new(1, 1);
  for (int r = 0; r < 2; r++) {
    const char *p = runs[r].text;
    while (*p) {
      bool lead_space = *p == ' ';
      while (*p == ' ') p++;
      if (!*p) break;
      const char *e = p;
      while (*e && *e != ' ') e++;
      CHECK(nw < 64 && e - p < 48);
      Word *w = &words[nw++];
      memcpy(w->s, p, (size_t)(e - p));
      w->s[e - p] = 0;
      w->bold = runs[r].bold;
      cv_font(m, w->bold ? fb : fn);
      w->w = cv_measure_text(m, w->s);
      cv_font(m, fn);
      w->space = (lead_space || (nw > 1 && words[nw - 2].bold == w->bold)) ? cv_measure_text(m, " ") : 0;
      if (nw == 1) w->space = 0;
      p = e;
    }
  }
  canvas_free(m);
  int lines = 1;
  double x = 0;
  for (int i = 0; i < nw; i++) {
    if (i > 0 && x + words[i].space + words[i].w > maxw) { lines++; x = 0; words[i].space = 0; }
    x += words[i].space + words[i].w;
    words[i].line = lines - 1;
  }
  double h1_h = 36, place_top = h1_h + 6, place_h = 16.8, how_top = place_top + place_h + 22, how_lh = 22.1;
  double total = how_top + lines * how_lh, mg = 16;
  double top = H - bottom - total;
  Canvas *c = layer_canvas(&u->caption, (int)ceil((maxw + 2 * mg) * k), (int)ceil((total + 2 * mg) * k));
  cv_save(c);
  cv_scale(c, k, k);
  cv_translate(c, mg, mg);
  const char *col = "#fff8ef", *sh = "rgba(30, 18, 12, 0.45)";
  cv_font(c, fh);
  shadow_text(c, k, "Ocean Drive", 0, baseline_in(fh, 0, h1_h), col, sh, 1, 10);
  cv_save(c);
  cv_global_alpha(c, 0.85);
  cv_font(c, fp);
  cv_letter_spacing(c, 0.22 * 12);
  shadow_text(c, k, "MIAMI BEACH · 6:52 AM", 0, baseline_in(fp, place_top, place_h), col, sh, 1, 10);
  cv_letter_spacing(c, 0);
  cv_restore(c);
  cv_save(c);
  cv_global_alpha(c, 0.78);
  x = 0;
  int line = 0;
  for (int i = 0; i < nw; i++) {
    if (words[i].line != line) { line = words[i].line; x = 0; }
    x += words[i].space;
    cv_font(c, words[i].bold ? fb : fn);
    shadow_text(c, k, words[i].s, x, baseline_in(fn, how_top + line * how_lh, how_lh), col, sh, 1, 10);
    x += words[i].w;
  }
  cv_restore(c);
  cv_restore(c);
  layer_at(&u->caption, (left - mg) * k, (top - mg) * k);
  u->caption_touch = touch;
}

static void overlay_layout(Ui *u) {
  double k = u->k, W = u->W, H = u->H;
  // radial-gradient(ellipse at 50% 55%, transparent 45%, rgba(20,14,18,.32) 100%): farthest-corner
  double cx = W / 2, cy = H * 0.55;
  double a = fmin(cx, W - cx) / fmin(cy, H - cy);
  double dx = W / 2, dy = fmax(cy, H - cy);
  double ry = sqrt((dx / a) * (dx / a) + dy * dy), rx = a * ry;
  Layer *V = &u->vig;
  V->vignette = true;
  V->ell[0] = cx * k; V->ell[1] = cy * k; V->ell[2] = rx * k; V->ell[3] = ry * k;
  V->col[0] = 20 / 255.0; V->col[1] = 14 / 255.0; V->col[2] = 18 / 255.0; V->col[3] = 0.32;
  V->stops[0] = 0.45; V->stops[1] = 1;
  caption_layer(u, u->caption_touch);
}

// a single centred / left line of text with a shadow into a layer sized to it
static void text_layer(Ui *u, Layer *L, const char *css, double spacing, const char *text, const char *color,
                       const char *shadow, double sh_blur, double line_h, double *out_w) {
  double k = u->k, mg = sh_blur + 2;
  Canvas *m = canvas_new(1, 1);
  cv_font(m, css);
  cv_letter_spacing(m, spacing);
  double w = cv_measure_text(m, text);
  canvas_free(m);
  Canvas *c = layer_canvas(L, (int)ceil((w + 2 * mg) * k), (int)ceil((line_h + 2 * mg) * k));
  cv_save(c);
  cv_scale(c, k, k);
  cv_font(c, css);
  cv_letter_spacing(c, spacing);
  shadow_text(c, k, text, mg, baseline_in(css, mg, line_h), color, shadow, 1, sh_blur);
  cv_restore(c);
  *out_w = w;
}

static void note_place(Ui *u) {
  if (!u->note.cv) return;
  double k = u->k, mg = 8 + 2;
  double w = canvas_width(u->note.cv) / k - 2 * mg;
  layer_at(&u->note, (u->W / 2 - w / 2 - mg) * k, (u->H - 7 * u->H / 100 - 16.8 - mg) * k);
}
static void note_layer(Ui *u) {
  char f[160];
  double w;
  font_css(f, sizeof f, "400", 12, "system-ui, sans-serif");
  text_layer(u, &u->note, f, 0.08 * 12, u->note_text, "rgba(255, 248, 239, 0.85)", "rgba(0, 0, 0, 0.4)", 8, 16.8, &w);
  note_place(u);
}

// #ride-prompt: [<b>E</b>] text, centred at 61% of the height
static void prompt_layer(Ui *u) {
  double k = u->k, W = u->W, H = u->H;
  char fk[160], ft[160];
  font_css(ft, sizeof ft, "400", 12, "system-ui, sans-serif");
  font_css(fk, sizeof fk, "500", 12, "system-ui, sans-serif");
  Canvas *m = canvas_new(1, 1);
  cv_font(m, ft);
  cv_letter_spacing(m, 0.16 * 12);
  double tw_ = cv_measure_text(m, u->prompt_text);
  cv_font(m, fk);
  cv_letter_spacing(m, 0);
  bool key = u->prompt_key[0] != 0;
  double kw = key ? cv_measure_text(m, u->prompt_key) : 0;
  canvas_free(m);
  // the key box: inline-block, padding .28em .3em, min-width 1.35em (content box), 1px border
  double pad_y = 0.28 * 12, pad_x = 0.3 * 12;
  double box_w = key ? fmax(kw, 1.35 * 12) + 2 * pad_x + 2 : 0, box_h = 12 + 2 * pad_y + 2;
  double gap = key ? 0.55 * 12 : 0;
  double total = box_w + gap + tw_;
  double line_h = key ? box_h : 12, mg = 10;
  Canvas *c = layer_canvas(&u->prompt, (int)ceil((total + 2 * mg) * k), (int)ceil((line_h + 2 * mg) * k));
  cv_save(c);
  cv_scale(c, k, k);
  cv_translate(c, mg, mg);
  double base = key ? 1 + pad_y + baseline_in(ft, 0, 12) : baseline_in(ft, 0, 12);
  const char *col = "rgba(255, 250, 242, 0.88)", *sh = "rgba(30, 18, 12, 0.55)";
  if (key) {
    // rounded 1px border (radius 3)
    cv_stroke_color(c, "rgba(255, 250, 242, 0.55)");
    cv_line_width(c, 1);
    double x0 = 0.5, y0 = 0.5, x1 = box_w - 0.5, y1 = box_h - 0.5, r = 2.5;
    cv_begin_path(c);
    cv_move_to(c, x0 + r, y0);
    cv_line_to(c, x1 - r, y0); cv_arc(c, x1 - r, y0 + r, r, -PI_D / 2, 0, false);
    cv_line_to(c, x1, y1 - r); cv_arc(c, x1 - r, y1 - r, r, 0, PI_D / 2, false);
    cv_line_to(c, x0 + r, y1); cv_arc(c, x0 + r, y1 - r, r, PI_D / 2, PI_D, false);
    cv_line_to(c, x0, y0 + r); cv_arc(c, x0 + r, y0 + r, r, PI_D, PI_D * 1.5, false);
    cv_close_path(c);
    cv_stroke(c);
    cv_font(c, fk);
    shadow_text(c, k, u->prompt_key, box_w / 2 - kw / 2, base, col, sh, 1, 8);
  }
  cv_font(c, ft);
  cv_letter_spacing(c, 0.16 * 12);
  shadow_text(c, k, u->prompt_text, box_w + gap, base, col, sh, 1, 8);
  cv_restore(c);
  layer_at(&u->prompt, (W / 2 - total / 2 - mg) * k, (H * 0.61 - mg) * k);
}

static void hud_layer(Ui *u) {
  char f[160];
  double w;
  font_css(f, sizeof f, "400", 12, "ui-monospace, monospace");
  text_layer(u, &u->hud, f, 0, u->hud_text, "rgba(255, 250, 240, 0.7)", nullptr, 0, 16.8, &w);
  layer_at(&u->hud, (12 - 2) * u->k, (u->H - 10 - 16.8 - 2) * u->k);
}

// ---- touch controls ----------------------------------------------------------------------------

// a .ring: 1px border circle (solid or dashed) with its faint outer / inner 0.5px shadow rings
static void ring(Canvas *c, double cx, double cy, double d, const char *border, bool dashed, const char *fill) {
  double r = d / 2;
  if (fill) {
    cv_fill_color(c, fill);
    cv_begin_path(c);
    cv_arc(c, cx, cy, r - 0.5, 0, 2 * PI_D, false);
    cv_fill(c);
  }
  cv_line_width(c, 0.5);
  cv_stroke_color(c, "rgba(40, 24, 16, 0.08)");
  cv_begin_path(c); cv_arc(c, cx, cy, r + 0.25, 0, 2 * PI_D, false); cv_stroke(c);
  cv_stroke_color(c, "rgba(40, 24, 16, 0.06)");
  cv_begin_path(c); cv_arc(c, cx, cy, r - 1.25, 0, 2 * PI_D, false); cv_stroke(c);
  cv_line_width(c, 1);
  cv_stroke_color(c, border);
  if (!dashed) {
    cv_begin_path(c); cv_arc(c, cx, cy, r - 0.5, 0, 2 * PI_D, false); cv_stroke(c);
    return;
  }
  int n = (int)floor(2 * PI_D * (r - 0.5) / 6);   // dashes of ~3px with ~3px gaps
  for (int i = 0; i < n; i++) {
    double a0 = 2 * PI_D * i / n, a1 = a0 + PI_D / n;
    cv_begin_path(c); cv_arc(c, cx, cy, r - 0.5, a0, a1, false); cv_stroke(c);
  }
}

static void touch_layers(Ui *u, const Touch *t) {
  double k = u->k, W = u->W, H = u->H;
  char f[160];
  // hint (static), stick ring, knob
  {
    Canvas *c = layer_canvas(&u->T[T_HINT], (int)ceil(92 * k), (int)ceil(92 * k));
    cv_save(c); cv_scale(c, k, k);
    ring(c, 46, 46, 88, "rgba(255, 250, 242, 0.22)", true, nullptr);
    cv_restore(c);
    layer_at(&u->T[T_HINT], (13 * W / 100 - 46) * k, (H - 11 * H / 100 - 46) * k);
  }
  {
    double d = TOUCH_R * 2 + 24;
    Canvas *c = layer_canvas(&u->T[T_STICK], (int)ceil((d + 4) * k), (int)ceil((d + 4) * k));
    cv_save(c); cv_scale(c, k, k);
    ring(c, d / 2 + 2, d / 2 + 2, d, "rgba(255, 250, 242, 0.34)", false, nullptr);
    cv_fill_color(c, "rgba(255, 250, 242, 0.3)");
    cv_begin_path(c); cv_arc(c, d / 2 + 2, d / 2 + 2, 1.5, 0, 2 * PI_D, false); cv_fill(c);
    cv_restore(c);
  }
  {
    Canvas *c = layer_canvas(&u->T[T_KNOB], (int)ceil(46 * k), (int)ceil(46 * k));
    cv_save(c); cv_scale(c, k, k);
    ring(c, 23, 23, 42, "rgba(255, 250, 242, 0.45)", false, "rgba(255, 250, 242, 0.1)");
    cv_restore(c);
  }
  // buttons: .jump (chevron), its label, .ride (text), .mute (speaker icon)
  const char *btn_border = "rgba(255, 250, 242, 0.32)", *btn_col = "rgba(255, 250, 242, 0.62)";
  struct { int id; TouchButton b; bool down; } bs[3] = {
    { T_JUMP, touch_jump_button(W, H), t->jump_down }, { T_RIDE, touch_ride_button(W, H), t->ride_down },
    { T_MUTE, touch_mute_button(W, H), t->mute_down } };
  for (int i = 0; i < 3; i++) {
    double d = bs[i].b.r * 2, s = bs[i].down ? 0.94 : 1;
    Canvas *c = layer_canvas(&u->T[bs[i].id], (int)ceil((d + 4) * k), (int)ceil((d + 4) * k));
    cv_save(c); cv_scale(c, k, k);
    cv_translate(c, d / 2 + 2, d / 2 + 2);
    cv_scale(c, s, s);
    cv_fill_color(c, bs[i].down ? "rgba(255, 250, 242, 0.16)" : "rgba(255, 250, 242, 0.04)");
    cv_begin_path(c); cv_arc(c, 0, 0, d / 2 - 0.5, 0, 2 * PI_D, false); cv_fill(c);
    cv_stroke_color(c, btn_border);
    cv_line_width(c, 1);
    cv_begin_path(c); cv_arc(c, 0, 0, d / 2 - 0.5, 0, 2 * PI_D, false); cv_stroke(c);
    cv_stroke_color(c, btn_col);
    cv_line_width(c, 1.2);
    cv_line_cap(c, CAP_ROUND);
    cv_line_join(c, JOIN_ROUND);
    if (bs[i].id == T_JUMP) {   // svg 22x22: M6 13.5 L11 8.5 L16 13.5
      cv_begin_path(c);
      cv_move_to(c, 6 - 11, 13.5 - 11); cv_line_to(c, 0, 8.5 - 11); cv_line_to(c, 16 - 11, 13.5 - 11);
      cv_stroke(c);
    } else if (bs[i].id == T_MUTE) {   // svg 18x18
      cv_translate(c, -9, -9);
      cv_begin_path(c);
      cv_move_to(c, 3, 7); cv_line_to(c, 5.5, 7); cv_line_to(c, 9, 4); cv_line_to(c, 9, 14); cv_line_to(c, 5.5, 11); cv_line_to(c, 3, 11);
      cv_close_path(c);
      cv_stroke(c);
      cv_begin_path(c);
      if (t->muted) {
        cv_move_to(c, 12, 7); cv_line_to(c, 16, 11); cv_move_to(c, 16, 7); cv_line_to(c, 12, 11);
      } else {
        cv_move_to(c, 11.5, 6.5); cv_quadratic_to(c, 13, 9, 11.5, 11.5);
        cv_move_to(c, 13.5, 5); cv_quadratic_to(c, 16, 9, 13.5, 13);
      }
      cv_stroke(c);
    } else {   // Ride / Off: 500 9px, letter-spacing .22em, padding-left .22em
      const char *txt = t->ride_mode == 2 ? "OFF" : "RIDE";
      font_css(f, sizeof f, "500", 9, "system-ui, sans-serif");
      cv_font(c, f);
      cv_letter_spacing(c, 0.22 * 9);
      double w = cv_measure_text(c, txt) + 0.22 * 9;
      cv_fill_color(c, btn_col);
      cv_fill_text(c, txt, -w / 2 + 0.22 * 9, baseline_in(f, -4.5, 9));
      cv_letter_spacing(c, 0);
    }
    cv_restore(c);
    layer_at(&u->T[bs[i].id], (bs[i].b.x - d / 2 - 2) * k, (bs[i].b.y - d / 2 - 2) * k);
  }
  // the jump label under the button (8.5px, letter-spacing .3em, padding-left .3em)
  {
    char up[16];
    upper(up, sizeof up, t->jump_label);
    font_css(f, sizeof f, "400", 8.5, "system-ui, sans-serif");
    double w;
    text_layer(u, &u->T[T_JUMP_LABEL], f, 0.3 * 8.5, up, "rgba(255, 250, 242, 0.42)", "rgba(30, 18, 12, 0.35)", 6, 8.5, &w);
    TouchButton b = touch_jump_button(W, H);
    double mg = 6 + 2, bw = w + 0.3 * 8.5;
    layer_at(&u->T[T_JUMP_LABEL], (b.x - bw / 2 + 0.3 * 8.5 - mg) * k, (b.y + b.r + 7 - mg) * k);
  }
  u->touch_version = t->version;
  u->touch_built = true;
}

static void touch_push(Ui *u, const Touch *t, double dt) {
  double k = u->k;
  bool rebuild = !u->touch_built || t->version != u->touch_version;
  if (rebuild) touch_layers(u, t);
  tw_go(&u->touch_op, t->enabled ? 1 : 0, 0.6, 0, EASE);
  tw_go(&u->hint_op, t->hint_gone ? 0 : 1, 1.2, 0, EASE);
  tw_go(&u->stick_op, t->stick_show ? 1 : 0, t->stick_show ? 0.08 : 0.25, 0, EASE);
  tw_go(&u->knob_op, t->stick_show ? 1 : 0, t->stick_show ? 0.08 : 0.25, 0, EASE);
  tw_go(&u->ride_op, t->ride_mode ? 1 : 0, 0.3, 0, EASE);
  Tween *tws[] = { &u->touch_op, &u->hint_op, &u->stick_op, &u->knob_op, &u->ride_op };
  for (size_t i = 0; i < ARRAY_LEN(tws); i++) tw_step(tws[i], dt);
  double o = tw(&u->touch_op);
  if (o <= 0.001) return;
  double d = TOUCH_R * 2 + 24;
  layer_at(&u->T[T_STICK], (t->stick_x - d / 2 - 2) * k, (t->stick_y - d / 2 - 2) * k);
  layer_at(&u->T[T_KNOB], (t->knob_x - 23) * k, (t->knob_y - 23) * k);
  // (#touch's opacity is a group, but its parts barely overlap: per-layer is equivalent)
  push(u, &u->T[T_HINT], o * tw(&u->hint_op));
  push(u, &u->T[T_STICK], o * tw(&u->stick_op));
  push(u, &u->T[T_KNOB], o * tw(&u->knob_op));
  push(u, &u->T[T_JUMP], o);
  push(u, &u->T[T_JUMP_LABEL], o);
  push(u, &u->T[T_RIDE], o * tw(&u->ride_op));
  push(u, &u->T[T_MUTE], o);
}

// ---- #gpu-reset --------------------------------------------------------------------------------

// greedy wrap of `text` into lines no wider than w; returns the line count (starts in `starts`)
static int wrap(Canvas *c, const char *text, double w, int *starts, int *lens, int max) {
  int n = 0, len = (int)strlen(text), i = 0;
  while (i < len && n < max) {
    int line_end = i, j = i;
    while (j <= len) {
      int k = j;
      while (k < len && text[k] != ' ') k++;
      char buf[256];
      int m = k - i < 255 ? k - i : 255;
      memcpy(buf, text + i, (size_t)m); buf[m] = 0;
      if (line_end > i && cv_measure_text(c, buf) > w) break;
      line_end = k;
      j = k + 1;
      if (k >= len) break;
    }
    starts[n] = i; lens[n] = line_end - i; n++;
    i = line_end + 1;
  }
  return n;
}
// text-wrap: balance: the narrowest width that keeps the greedy line count
static int wrap_balanced(Canvas *c, const char *text, double w, int *starts, int *lens, int max) {
  int n = wrap(c, text, w, starts, lens, max);
  if (n <= 1) return n;
  double lo = 0, hi = w;
  for (int it = 0; it < 24; it++) {
    double mid = (lo + hi) / 2;
    if (wrap(c, text, mid, starts, lens, max) > n) lo = mid; else hi = mid;
  }
  return wrap(c, text, hi, starts, lens, max);
}

static void reset_layers(Ui *u) {
  double k = u->k, W = u->W, H = u->H, vmin = fmin(W, H) / 100;
  // background: radial-gradient(ellipse at 50% 60%, #6d4a4f 0%, #3a2733 55%, #1d1520 100%), drawn small
  {
    double gs = 0.25;
    Canvas *c = layer_canvas(&u->reset_bg, (int)ceil(W * k * gs), (int)ceil(H * k * gs));
    cv_save(c);
    cv_scale(c, W * k * gs / W, H * k * gs / H);
    double cx = W / 2, cy = H * 0.6;
    double a = fmin(cx, W - cx) / fmin(cy, H - cy);
    double dy = fmax(cy, H - cy), ry = sqrt((W / 2 / a) * (W / 2 / a) + dy * dy), rx = a * ry;
    // the ellipse as a circle gradient in a scaled space
    cv_translate(c, cx, cy);
    cv_scale(c, rx / ry, 1);
    Gradient g = cv_radial_gradient(c, 0, 0, 0, 0, 0, ry);
    grad_add_stop(&g, 0, "#6d4a4f");
    grad_add_stop(&g, 0.55, "#3a2733");
    grad_add_stop(&g, 1, "#1d1520");
    cv_fill_gradient(c, &g);
    cv_fill_rect(c, -cx * ry / rx - 1, -cy - 1, W * ry / rx + 2, H + 2);
    cv_restore(c);
    u->reset_bg.x = 0; u->reset_bg.y = 0; u->reset_bg.w = W * k; u->reset_bg.h = H * k;
  }
  // the message (flex column, centred, padding 24px)
  const char *title = u->reset == 2 ? "The graphics driver reset again." : "The graphics driver reset — reloading at a lighter quality…";
  const char *sub = u->reset == 2 ? "This machine may need a lighter setting." : nullptr;
  double ts = clampv(18, 2.4 * vmin, 22), tlh = ts * 1.35, maxw = fmin(480, W - 48);
  char ft[160], fs[160], fb[160];
  font_css(ft, sizeof ft, "italic 400", ts, "Georgia, 'Times New Roman', serif");
  font_css(fs, sizeof fs, "400", 13, "system-ui, sans-serif");
  font_css(fb, sizeof fb, "400", 12, "system-ui, sans-serif");
  Canvas *m = canvas_new(1, 1);
  int st[16], ln[16], ss[16], sl[16];
  cv_font(m, ft);
  int nt = wrap_balanced(m, title, maxw, st, ln, 16);
  int ns = 0;
  if (sub) { cv_font(m, fs); cv_letter_spacing(m, 0.02 * 13); ns = wrap_balanced(m, sub, maxw, ss, sl, 16); }
  cv_font(m, fb);
  cv_letter_spacing(m, 0.16 * 12);
  const char *btn = "RELOAD AT LOW QUALITY";
  double bw = cv_measure_text(m, btn) + 40 + 2, bh = 12 + 18 + 2;
  canvas_free(m);
  double h = nt * tlh + 8 + (sub ? ns * 13 * 1.6 + 8 : 0) + (u->reset == 2 ? 14 + bh : 0);
  double top = H / 2 - h / 2;
  Canvas *c = layer_canvas(&u->reset_fg, (int)ceil(W * k), (int)ceil(H * k));
  cv_save(c);
  cv_scale(c, k, k);
  double y = top;
  cv_font(c, ft);
  cv_fill_color(c, "#fff4e6");
  for (int i = 0; i < nt; i++) {
    char buf[256];
    snprintf(buf, sizeof buf, "%.*s", ln[i], title + st[i]);
    double w = cv_measure_text(c, buf);
    cv_fill_text(c, buf, W / 2 - w / 2, baseline_in(ft, y, tlh));
    y += tlh;
  }
  y += 8;
  if (sub) {
    cv_font(c, fs);
    cv_letter_spacing(c, 0.02 * 13);
    cv_fill_color(c, "rgba(255, 238, 222, 0.78)");
    for (int i = 0; i < ns; i++) {
      char buf[256];
      snprintf(buf, sizeof buf, "%.*s", sl[i], sub + ss[i]);
      double w = cv_measure_text(c, buf);
      cv_fill_text(c, buf, W / 2 - w / 2, baseline_in(fs, y, 13 * 1.6));
      y += 13 * 1.6;
    }
    cv_letter_spacing(c, 0);
    y += 8;
  }
  if (u->reset == 2) {
    y += 14;
    double bx = W / 2 - bw / 2;
    u->button[0] = bx; u->button[1] = y; u->button[2] = bw; u->button[3] = bh;
    double r = bh / 2;
    cv_begin_path(c);
    cv_arc(c, bx + r, y + r, r - 0.5, PI_D / 2, PI_D * 1.5, false);
    cv_arc(c, bx + bw - r, y + r, r - 0.5, -PI_D / 2, PI_D / 2, false);
    cv_close_path(c);
    if (u->button_hover) { cv_fill_color(c, "rgba(255, 236, 205, 0.1)"); cv_fill(c); }
    cv_stroke_color(c, "rgba(255, 236, 205, 0.5)");
    cv_line_width(c, 1);
    cv_stroke(c);
    cv_font(c, fb);
    cv_letter_spacing(c, 0.16 * 12);
    cv_fill_color(c, "#fff4e6");
    cv_fill_text(c, btn, bx + 21, baseline_in(fb, y + 10, 12));
    cv_letter_spacing(c, 0);
  }
  cv_restore(c);
  layer_at(&u->reset_fg, 0, 0);
}

void ui_gpu_reset(Ui *u, int mode) {
  u->reset = mode;
  u->ld.on = false;       // (#loader removed, body.loading off, #overlay hidden)
  u->loading = false;
  u->dw = u->dh = 0;      // lay out again on the next render
  tw_set(&u->reset_op, 0);
  tw_go(&u->reset_op, 1, 0.6, 0, EASE);   // gpu-reset-in 0.6s ease
}

bool ui_reset_button(Ui *u, double x, double y, bool click) {
  bool in = u->reset == 2 && x >= u->button[0] && x <= u->button[0] + u->button[2] && y >= u->button[1] && y <= u->button[1] + u->button[3];
  if (!click && in != u->button_hover) { u->button_hover = in; if (u->k > 0) reset_layers(u); }
  return in;
}

// ---- API ---------------------------------------------------------------------------------------

Ui *ui_create(bool loader) {
  Ui *u = xcalloc(1, sizeof *u);
  u->pipe[0] = make_pipeline(false);
  u->pipe[1] = make_pipeline(true);
  u->sampler = SDL_CreateGPUSampler(g_gpu.dev, &(SDL_GPUSamplerCreateInfo){
    .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
    .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE });
  if (!u->sampler) FATAL("ui sampler: %s", SDL_GetError());
  u->white = make_texture(1, 1, false);
  const char *cap = SDL_getenv("OD_UI_SHOT");
  if (cap && strchr(cap, '@')) {
    snprintf(u->cap_path, sizeof u->cap_path, "%.*s", (int)(strchr(cap, '@') - cap), cap);
    u->cap_n = atol(strchr(cap, '@') + 1);
  }
  u->ld.on = loader;
  u->loading = loader;
  u->ld.exit_t = -1;
  u->ld.swap_at = -1;
  snprintf(u->ld.current, sizeof u->ld.current, "Waiting for first light…");
  snprintf(u->ld.pending, sizeof u->ld.pending, "%s", u->ld.current);
  tw_set(&u->ld.label_op, 1);
  tw_set(&u->ld.opacity, 1);
  tw_set(&u->ld.sun_scale, 1);
  tw_set(&u->ld.sun_gain, 1);
  tw_set(&u->ld.bloom, 0);
  tw_set(&u->overlay_op, 0);
  tw_set(&u->note_op, 0);
  tw_set(&u->prompt_op, 0);
  return u;
}

void ui_destroy(Ui *u) {
  if (!u) return;
  for (int i = 0; i < LD_COUNT; i++) layer_free(&u->ld.L[i]);
  for (int i = 0; i < T_COUNT; i++) layer_free(&u->T[i]);
  Layer *ls[] = { &u->vig, &u->caption, &u->note, &u->prompt, &u->hud, &u->reset_bg, &u->reset_fg };
  for (size_t i = 0; i < ARRAY_LEN(ls); i++) layer_free(ls[i]);
  if (u->group) SDL_ReleaseGPUTexture(g_gpu.dev, u->group);
  if (u->tb) SDL_ReleaseGPUTransferBuffer(g_gpu.dev, u->tb);
  SDL_ReleaseGPUTexture(g_gpu.dev, u->white);
  SDL_ReleaseGPUSampler(g_gpu.dev, u->sampler);
  SDL_ReleaseGPUGraphicsPipeline(g_gpu.dev, u->pipe[0]);
  SDL_ReleaseGPUGraphicsPipeline(g_gpu.dev, u->pipe[1]);
  free(u);
}

bool ui_loading(const Ui *u) { return u->loading; }
void ui_scene_ready(Ui *u) { u->ld.scene_ready = true; }

void ui_note(Ui *u, const char *text) {
  snprintf(u->note_text, sizeof u->note_text, "%s", text);
  if (u->k > 0) note_layer(u);
  else if (u->note.cv) { canvas_free(u->note.cv); u->note.cv = nullptr; }
  tw_go(&u->note_op, 1, 0.8, 0, EASE);
  u->note_left = 3.5;
}

// (re)builds the layouts for a new device size
static void layout(Ui *u, int w, int h) {
  int cw = 0, ch = 0;
  if (g_gpu.win) SDL_GetWindowSize(g_gpu.win, &cw, &ch);
  if (cw <= 0 || ch <= 0) { cw = w; ch = h; }
  u->k = (double)w / cw;
  u->W = cw;
  u->H = (double)h / u->k;
  u->dw = w; u->dh = h;
  if (u->ld.on) loader_layout(u);
  overlay_layout(u);
  if (u->note_text[0]) note_layer(u);
  if (u->prompt_text[0]) prompt_layer(u);
  if (u->hud_text[0]) hud_layer(u);
  if (u->reset) reset_layers(u);
  u->touch_built = false;
}

static void render(Ui *u, SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int w, int h, double dt, const UiState *s) {
  if (w != u->dw || h != u->dh) layout(u, w, h);
  u->n = 0;
  // #overlay (hidden while loading; .6s fade)
  if (s) {
    if (s->touch != u->caption_touch) caption_layer(u, s->touch);
    tw_go(&u->overlay_op, s->overlay && !u->loading ? 1 : 0, 0.6, 0, EASE);
    tw_step(&u->overlay_op, dt);
    double o = tw(&u->overlay_op);
    push(u, &u->vig, o);
    push(u, &u->caption, o);
    // #note
    if (u->note_left > 0 && (u->note_left -= dt) <= 0) tw_go(&u->note_op, 0, 0.8, 0, EASE);
    tw_step(&u->note_op, dt);
    if (u->note_text[0] && !u->note.cv) note_layer(u);
    push(u, &u->note, tw(&u->note_op));
    // HUD
    if (s->hud) {
      if (strcmp(s->hud, u->hud_text)) {
        snprintf(u->hud_text, sizeof u->hud_text, "%s", s->hud);
        if (u->hud_text[0]) hud_layer(u);
      }
      if (u->hud_text[0]) push(u, &u->hud, 1);
    }
    // ride prompt (html.touch hides it); the text stays while it fades out
    const char *pk = s->prompt_key ? s->prompt_key : "", *pt = s->prompt_text ? s->prompt_text : "";
    if (pt[0] && (strcmp(pk, u->prompt_key) || strcmp(pt, u->prompt_text))) {
      snprintf(u->prompt_key, sizeof u->prompt_key, "%s", pk);
      snprintf(u->prompt_text, sizeof u->prompt_text, "%s", pt);
      prompt_layer(u);
    }
    tw_go(&u->prompt_op, pt[0] ? 1 : 0, 0.35, 0, EASE);
    tw_step(&u->prompt_op, dt);
    if (!s->touch) push(u, &u->prompt, tw(&u->prompt_op));
    if (s->touch_ctl) touch_push(u, s->touch_ctl, dt);
  }
  if (u->reset) {   // #gpu-reset (z-index 20: over the page)
    if (!u->reset_fg.cv || !u->reset_bg.cv) reset_layers(u);
    tw_step(&u->reset_op, dt);
    push(u, &u->reset_bg, tw(&u->reset_op));
    push(u, &u->reset_fg, tw(&u->reset_op));
  }
  int page_n = u->n;
  // the loader, drawn as a group into its own target, then composited with its opacity
  if (u->ld.on) {
    loader_tick(u, dt);
    if (u->ld.on) loader_push(u);
  }
  upload(u, cb);
  if (page_n > 0 || !u->ld.on) draw_list(u, cb, target, w, h, 0, page_n, false);
  if (u->ld.on && u->n > page_n) {
    if (!u->group || u->group_w != w || u->group_h != h) {
      if (u->group) SDL_ReleaseGPUTexture(g_gpu.dev, u->group);
      u->group = make_texture(w, h, true);
      u->group_w = w; u->group_h = h;
    }
    draw_list(u, cb, u->group, w, h, page_n, u->n, true);
    u->group_layer = (Layer){ .ext = u->group, .x = 0, .y = 0, .w = w, .h = h };
    u->list[0] = &u->group_layer;
    u->group_layer.opacity = tw(&u->ld.opacity);
    u->n = 1;
    if (u->group_layer.opacity > 0.001) draw_list(u, cb, target, w, h, 0, 1, false);
  }
}

void ui_render(Ui *u, SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int w, int h, double dt, const UiState *s) {
  u->calls++;
  render(u, cb, target, w, h, dt, s);
}

SDL_GPUTexture *ui_capture_target(Ui *u, int w, int h) {
  if (!u->cap_n || u->calls + 1 != u->cap_n) return nullptr;
  if (u->cap && (u->cap_w != w || u->cap_h != h)) { SDL_ReleaseGPUTexture(g_gpu.dev, u->cap); u->cap = nullptr; }
  if (!u->cap) { u->cap = make_texture(w, h, true); u->cap_w = w; u->cap_h = h; }
  return u->cap;
}

void ui_capture_present(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *cap, SDL_GPUTexture *swap, int w, int h) {
  SDL_BlitGPUTexture(cb, &(SDL_GPUBlitInfo){
    .source = { .texture = cap, .w = (uint32_t)w, .h = (uint32_t)h },
    .destination = { .texture = swap, .w = (uint32_t)w, .h = (uint32_t)h },
    .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST });
}

void ui_capture_save(Ui *u) {
  SDL_WaitForGPUIdle(g_gpu.dev);
  uint8_t *px = xmalloc((size_t)u->cap_w * u->cap_h * 4);
  gpu_read_rgba8(u->cap, u->cap_w, u->cap_h, px);
  if (g_gpu.swap_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || g_gpu.swap_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
    for (size_t i = 0; i < (size_t)u->cap_w * u->cap_h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
  for (size_t i = 0; i < (size_t)u->cap_w * u->cap_h; i++) px[i * 4 + 3] = 255;
  LOG("ui shot %s (%d x %d, page %ld): %s", u->cap_path, u->cap_w, u->cap_h, u->calls,
      save_bmp_rgba8(u->cap_path, px, u->cap_w, u->cap_h) ? "saved" : "FAILED");
  free(px);
}

void ui_progress(Ui *u, double fraction, const char *label) {
  if (!u->ld.on) return;
  u->ld.reported = true;
  u->ld.last_report = u->ld.t;
  u->ld.target = fmax(u->ld.target, fmin(1, fraction));
  if (label) snprintf(u->ld.pending, sizeof u->ld.pending, "%s", label);
}

void ui_paint(Ui *u) {
  if (!u->ld.on) return;
  // one painted frame (nextFrame): the time since the last one is the step's duration
  uint64_t now = SDL_GetTicksNS();
  double dt = u->paint_ns ? (double)(now - u->paint_ns) * 1e-9 : 1 / 60.0;
  u->paint_ns = now;
  SDL_PumpEvents();
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  SDL_GPUTexture *swap = nullptr;
  uint32_t sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, g_gpu.win, &swap, &sw, &sh)) FATAL("swapchain: %s", SDL_GetError());
  SDL_GPUTexture *cap = swap ? ui_capture_target(u, (int)sw, (int)sh) : nullptr;
  u->calls++;
  if (swap) render(u, cb, cap ? cap : swap, (int)sw, (int)sh, dt, nullptr);
  if (cap) ui_capture_present(cb, cap, swap, (int)sw, (int)sh);
  SDL_SubmitGPUCommandBuffer(cb);
  if (cap) ui_capture_save(u);
}

void ui_load_step(Ui *u, double fraction, const char *label) {
  ui_progress(u, fraction, label);
  ui_paint(u);
}
